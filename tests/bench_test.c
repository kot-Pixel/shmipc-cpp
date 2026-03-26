/*
 * bench_test.c  —  shmipc bidirectional benchmark / correctness test
 *
 * Architecture: fork() — parent = server, child = client.
 * One fork per preset; within each fork the full matrix runs sequentially.
 *
 * ┌───────────────────────────────────────────────────────────────────────┐
 * │  3 presets × 13 payload sizes × 3 modes × 2 directions = 234 cases   │
 * │                                                                       │
 * │  Payloads  : 256B 512B 1K 2K 4K 8K 16K 32K 64K 128K 256K 512K 1M    │
 * │  Modes     : BLOCK  NONBLK  100ms                                     │
 * │  Directions: S→C (server sends, client receives)                      │
 * │              C→S (client sends, server receives)                      │
 * │  Presets   : LOW_FREQ  GENERAL  HI_THRU                               │
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * Message wire format
 * ───────────────────
 *   [0..3]  uint32_t  seq      monotone sequence number
 *   [4..7]  uint32_t  pay_len  payload byte count (0 for STOP)
 *   [8..]   uint8_t   payload[]
 *
 * STOP sentinel: seq = 0xFFFFFFFF, pay_len = 0, total = 8 bytes.
 * Always sent with SHMIPC_TIMEOUT_INFINITE so it is never dropped.
 *
 * Pipe protocol (srv2cli / cli2srv)
 * ──────────────────────────────────
 *   'G' = server about to send  (S→C test)
 *   'C' = client about to send  (C→S test)
 *   'A' = ready ACK             (both directions)
 *   'X' = exit
 *   cli_result_t  — after S→C test (client recv stats)
 *   c2s_stats_t   — after C→S test (client send stats)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>

#include "shmipc/shmipc.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  Test matrix
 * ═══════════════════════════════════════════════════════════════════════ */

static const uint32_t PAYLOADS[] = {
    256, 512, 1024, 2048, 4096, 8192,
    16384, 32768, 65536, 131072, 262144, 524288, 1048576
};
#define N_PAY  (int)(sizeof(PAYLOADS)/sizeof(PAYLOADS[0]))

static int msgs_for(uint32_t plen) {
    if (plen >= 512*1024) return 30;
    if (plen >= 128*1024) return 80;
    if (plen >=  32*1024) return 150;
    return 300;
}

static const int32_t  MODES[]    = { SHMIPC_TIMEOUT_INFINITE,
                                     SHMIPC_TIMEOUT_NONBLOCKING,
                                     100 };
static const char*    MODE_STR[] = { "BLOCK ", "NONBLK", " 100ms" };
#define N_MOD  3

typedef struct { const char *name, *chan; uint32_t shm, cap, slice; } preset_t;
static const preset_t PRESETS[] = {
    { "LOW_FREQ", "bench_lf",   8u<<20,  32,  4096  },
    { "GENERAL",  "bench_gen", 16u<<20,  64,  16384 },
    { "HI_THRU",  "bench_hi",  64u<<20, 256,  65536 },
};
#define N_PRE  3

#define HDR_SZ    8u
#define STOP_SEQ  0xFFFFFFFFu

/* ═══════════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static const char *fmtsz(uint32_t n) {
    static char b[16];
    if      (n >= 1u<<20) snprintf(b, sizeof b, "%4uMB", n>>20);
    else if (n >= 1u<<10) snprintf(b, sizeof b, "%4uKB", n>>10);
    else                  snprintf(b, sizeof b, "%4u B", n);
    return b;
}

static void fill_msg(uint8_t *buf, uint32_t seq, uint32_t pay_len) {
    memcpy(buf,     &seq,     4);
    memcpy(buf + 4, &pay_len, 4);
    for (uint32_t i = 0; i < pay_len; i++)
        buf[HDR_SZ + i] = (uint8_t)((seq * 31u + i) % 251u);
}

static void fill_stop(uint8_t *buf) {
    uint32_t seq = STOP_SEQ, len = 0;
    memcpy(buf, &seq, 4); memcpy(buf + 4, &len, 4);
}

static void pipe_wr(int fd, const void *p, size_t n) {
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r > 0) { p = (const char*)p + r; n -= (size_t)r; }
        else if (errno != EINTR) break;
    }
}
static void pipe_rd(int fd, void *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r > 0) { p = (char*)p + r; n -= (size_t)r; }
        else if (r == 0 || errno != EINTR) break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Result structs sent over pipes
 * ═══════════════════════════════════════════════════════════════════════ */

/* S→C: client recv stats */
typedef struct { uint32_t recv; uint32_t errs; } cli_result_t;

/* C→S: client send stats */
typedef struct { uint32_t ok; uint32_t drop; uint32_t tout; uint32_t us; } c2s_stats_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Server context & callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
    /* C→S receive counters (reset before each C→S test) */
    volatile uint32_t srv_recv;
    volatile uint32_t srv_errs;
    uint32_t          srv_expected_total;
    volatile int      srv_stop_seen;
} srv_ctx_t;

static void srv_on_connected(shmipc_session_t *s, void *ctx) {
    srv_ctx_t *c = (srv_ctx_t*)ctx;
    c->sess = s; c->connected = 1;
}
static void srv_on_disconnected(shmipc_session_t *s, void *ctx) {
    (void)s; ((srv_ctx_t*)ctx)->connected = 0;
}
static void srv_on_data(shmipc_session_t *s, const void *data,
                        uint32_t len, void *ctx) {
    (void)s;
    srv_ctx_t *c = (srv_ctx_t*)ctx;
    if (len < HDR_SZ) { c->srv_errs++; return; }
    uint32_t seq;
    memcpy(&seq, data, 4);
    if (seq == STOP_SEQ) { c->srv_stop_seen = 1; return; }
    c->srv_recv++;
    if (len != c->srv_expected_total) c->srv_errs++;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Client context & callback
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    volatile uint32_t recv;
    volatile uint32_t errs;
    uint32_t          expected_total;
    volatile int      stop_seen;
} cli_ctx_t;

static void cli_on_data(shmipc_session_t *s,
                        const void *data, uint32_t len, void *ctx) {
    (void)s;
    cli_ctx_t *c = (cli_ctx_t*)ctx;
    if (len < HDR_SZ) { c->errs++; return; }
    uint32_t seq;
    memcpy(&seq, data, 4);
    if (seq == STOP_SEQ) { c->stop_seen = 1; return; }
    c->recv++;
    if (len != c->expected_total) c->errs++;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Table helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static void print_header(const preset_t *p) {
    printf("\n┌──────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  Preset: %-8s   shm=%uMB   queue=%3u   slice=%-6s                               │\n",
           p->name, p->shm >> 20, p->cap, fmtsz(p->slice));
    printf("├──────────┬────────┬─────┬──────┬───────┬───────┬──────────┬────────┬──────────┤\n");
    printf("│ Payload  │  Mode  │ Dir │ msgs │  sent │  drop │    MB/s  │   recv │  result  │\n");
    printf("├──────────┼────────┼─────┼──────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_footer(void) {
    printf("└──────────┴────────┴─────┴──────┴───────┴───────┴──────────┴────────┴──────────┘\n");
}
static void print_sep(void) {
    printf("├──────────┼────────┼─────┼──────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_row(const char *payload, const char *mode, const char *dir,
                      int nmsg,
                      uint32_t sent, uint32_t drop,
                      double mbps,
                      uint32_t recv, const char *verdict) {
    printf("│ %8s │ %s │ %3s │ %4d │ %5u │ %5u │ %8.2f │ %6u │%s│\n",
           payload, mode, dir, nmsg, sent, drop, mbps, recv, verdict);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Server process
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_server(int s2c_wr, int c2s_rd, const preset_t *p) {
    srv_ctx_t ctx = { NULL, 0, 0, 0, 0, 0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_data        (srv, srv_on_data);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);

    if (shmipc_server_start(srv, p->chan) != SHMIPC_OK) {
        fprintf(stderr, "[server] start failed for %s\n", p->chan);
        shmipc_server_destroy(srv); return;
    }

    /* Wait for client READY */
    char ch; pipe_rd(c2s_rd, &ch, 1);

    /* Wait for on_connected */
    for (int t = 0; !ctx.connected && t < 200; t++) usleep(5000);
    if (!ctx.connected) {
        fprintf(stderr, "[server] client did not connect for %s\n", p->name);
        goto done;
    }

    print_header(p);

    uint32_t max_pay = PAYLOADS[N_PAY - 1];
    uint8_t *buf = (uint8_t*)malloc(HDR_SZ + max_pay);
    if (!buf) { perror("malloc"); goto done; }

    for (int si = 0; si < N_PAY; si++) {
        uint32_t plen  = PAYLOADS[si];
        uint32_t total = HDR_SZ + plen;
        int      nmsg  = msgs_for(plen);

        for (int mi = 0; mi < N_MOD; mi++) {
            int32_t tmo = MODES[mi];

            /* ── S→C test ─────────────────────────────────────────── */
            ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1); /* ACK */

            uint32_t ok = 0, drop = 0, tout = 0;
            double t0 = now_ms();

            for (int i = 0; i < nmsg; i++) {
                fill_msg(buf, (uint32_t)i, plen);
                int rc = shmipc_session_write(ctx.sess, buf, total, tmo);
                if      (rc == SHMIPC_OK)      ok++;
                else if (rc == SHMIPC_TIMEOUT) tout++;
                else                           drop++;
            }
            double ms = now_ms() - t0;

            fill_stop(buf);
            while (shmipc_session_write(ctx.sess, buf, HDR_SZ,
                                        SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

            cli_result_t cr; pipe_rd(c2s_rd, &cr, sizeof cr);

            double mbps = (ms > 0.01) ? (double)ok * total / (1<<20) / (ms/1000.0) : 0.0;
            const char *verdict;
            if (cr.errs > 0)                               verdict = "  FAIL  ";
            else if (tmo == SHMIPC_TIMEOUT_INFINITE)       verdict = (cr.recv == ok) ? "  PASS  " : " MISS!  ";
            else                                           verdict = (cr.recv <= ok)  ? "  OK    " : " EXTRA? ";

            print_row(fmtsz(plen), MODE_STR[mi], "S→C",
                      nmsg, ok, drop + tout, mbps, cr.recv, verdict);

            /* ── C→S test ─────────────────────────────────────────── */
            /* Reset server receive counters */
            ctx.srv_recv          = 0;
            ctx.srv_errs          = 0;
            ctx.srv_expected_total = total;
            ctx.srv_stop_seen     = 0;

            ch = 'C'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1); /* ACK */

            /* Wait for STOP sentinel to arrive from client */
            for (int w = 0; !ctx.srv_stop_seen && w < 200000; w++) usleep(100);
            usleep(200000); /* drain */

            c2s_stats_t cs; pipe_rd(c2s_rd, &cs, sizeof cs);

            double c2s_ms = cs.us / 1000.0;
            double c2s_mbps = (c2s_ms > 0.01)
                            ? (double)cs.ok * total / (1<<20) / (c2s_ms/1000.0)
                            : 0.0;

            const char *c2s_verdict;
            if (ctx.srv_errs > 0)                              c2s_verdict = "  FAIL  ";
            else if (tmo == SHMIPC_TIMEOUT_INFINITE)           c2s_verdict = (ctx.srv_recv == cs.ok) ? "  PASS  " : " MISS!  ";
            else                                               c2s_verdict = (ctx.srv_recv <= cs.ok)  ? "  OK    " : " EXTRA? ";

            print_row(fmtsz(plen), MODE_STR[mi], "C→S",
                      nmsg, cs.ok, cs.drop + cs.tout, c2s_mbps,
                      ctx.srv_recv, c2s_verdict);
        }

        if (si < N_PAY - 1) print_sep();
    }

    print_footer();
    free(buf);

done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Client process
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_client(int s2c_rd, int c2s_wr, const preset_t *p) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t cfg  = { p->shm, p->cap, p->slice };
    shmipc_client_set_config(cli, &cfg);

    cli_ctx_t ctx = { 0, 0, 0, 0 };
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_register_on_data(cli, cli_on_data);

    int conn = 0;
    for (int t = 0; t < 40 && !conn; t++) {
        if (shmipc_client_connect(cli, p->chan) == SHMIPC_OK) conn = 1;
        else usleep(100000);
    }
    if (!conn) { fprintf(stderr, "[client] connect failed\n"); exit(1); }

    /* Allocate send buffer for C→S tests */
    uint32_t max_pay = PAYLOADS[N_PAY - 1];
    uint8_t *send_buf = (uint8_t*)malloc(HDR_SZ + max_pay);
    if (!send_buf) { perror("malloc"); exit(1); }

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1); /* READY */

    int done = 0;
    for (int si = 0; si < N_PAY && !done; si++) {
        uint32_t plen  = PAYLOADS[si];
        uint32_t total = HDR_SZ + plen;
        int      nmsg  = msgs_for(plen);

        for (int mi = 0; mi < N_MOD && !done; mi++) {
            int32_t tmo = MODES[mi];

            /* ── S→C: receive from server ────────────────────────── */
            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') { done = 1; break; }

            ctx.recv           = 0;
            ctx.errs           = 0;
            ctx.stop_seen      = 0;
            ctx.expected_total = total;

            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

            for (int w = 0; !ctx.stop_seen && w < 200000; w++) usleep(100);
            usleep(200000);

            cli_result_t cr = { ctx.recv, ctx.errs };
            pipe_wr(c2s_wr, &cr, sizeof cr);

            /* ── C→S: send to server ─────────────────────────────── */
            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') { done = 1; break; }

            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

            uint32_t c2s_ok = 0, c2s_drop = 0, c2s_tout = 0;
            double t0 = now_ms();

            for (int i = 0; i < nmsg; i++) {
                fill_msg(send_buf, (uint32_t)i, plen);
                int rc = shmipc_client_write(cli, send_buf, total, tmo);
                if      (rc == SHMIPC_OK)      c2s_ok++;
                else if (rc == SHMIPC_TIMEOUT) c2s_tout++;
                else                           c2s_drop++;
            }

            double ms = now_ms() - t0;

            /* Send STOP sentinel (always blocking) */
            fill_stop(send_buf);
            while (shmipc_client_write(cli, send_buf, HDR_SZ,
                                       SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

            c2s_stats_t cs = {
                c2s_ok, c2s_drop, c2s_tout,
                (uint32_t)(ms * 1000.0)   /* microseconds */
            };
            pipe_wr(c2s_wr, &cs, sizeof cs);
        }
    }

    /* Consume the final EXIT command so server's pipe write doesn't SIGPIPE */
    if (!done) pipe_rd(s2c_rd, &ch, 1);

    free(send_buf);
    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    signal(SIGPIPE, SIG_IGN);   /* handle broken-pipe gracefully */

    printf("\n╔════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  shmipc bidirectional benchmark  (%d payloads × %d modes × 2 dirs × %d presets)       ║\n",
           N_PAY, N_MOD, N_PRE);
    printf("╚════════════════════════════════════════════════════════════════════════════════════╝\n");

    int all_pass = 1;

    for (int pi = 0; pi < N_PRE; pi++) {
        int s2c[2], c2s[2];
        if (pipe(s2c) || pipe(c2s)) { perror("pipe"); return 1; }

        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }

        if (child == 0) {
            close(s2c[1]); close(c2s[0]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            run_client(s2c[0], c2s[1], &PRESETS[pi]);
            close(s2c[0]); close(c2s[1]);
            exit(0);
        }

        close(s2c[0]); close(c2s[1]);
        run_server(s2c[1], c2s[0], &PRESETS[pi]);
        close(s2c[1]); close(c2s[0]);

        int st = 0; waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_pass = 0;
    }

    printf("\nOverall: %s\n\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}
