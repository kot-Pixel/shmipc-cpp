/*
 * test1_s2c.c — Server → Client unidirectional benchmark / correctness test
 *
 * 3 presets, each with its own payload range up to the preset's max:
 *   LOW_FREQ  : 256 B → 1 MB   (13 payloads)
 *   GENERAL   : 256 B → 4 MB   (15 payloads)
 *   HI_THRU   : 256 B → 8 MB   (16 payloads)
 * × 3 write modes (BLOCK / NONBLK / 100ms)
 *
 * Architecture: fork() — parent = server (sends), child = client (receives)
 *
 * Pipe protocol (parent→child srv2cli, child→parent cli2srv):
 *   'G'           = go (server about to send a batch)
 *   'A'           = ACK (client ready)
 *   'X'           = exit
 *   cli_result_t  = recv stats sent by client after each batch
 */

#include "test_common.h"
#include <pthread.h>

/* ── Presets ──────────────────────────────────────────────────────────── */
static const preset_t PRESETS[] = {
    { "LOW_FREQ", "t1_lf",   8u<<20,  32,  4096,  PAYS_LF,  N_LF,  1u<<20 },
    { "GENERAL",  "t1_gen", 16u<<20,  64, 16384,  PAYS_GEN, N_GEN, 4u<<20 },
    { "HI_THRU",  "t1_hi",  64u<<20, 256, 65536,  PAYS_HI,  N_HI,  8u<<20 },
};
#define N_PRE 3

/* ── Contexts & callbacks ─────────────────────────────────────────────── */
typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
} srv_ctx_t;

static void srv_on_connected   (shmipc_session_t *s, void *ctx)
    { ((srv_ctx_t*)ctx)->sess = s; ((srv_ctx_t*)ctx)->connected = 1; }
static void srv_on_disconnected(shmipc_session_t *s, void *ctx)
    { (void)s; ((srv_ctx_t*)ctx)->connected = 0; }

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
    uint32_t seq; memcpy(&seq, data, 4);
    if (seq == STOP_SEQ) { c->stop_seen = 1; return; }
    c->recv++;
    if (len != c->expected_total) c->errs++;
}

typedef struct { uint32_t recv; uint32_t errs; } cli_result_t;

/* ── Table printing ───────────────────────────────────────────────────── */
static void print_header(const preset_t *p) {
    printf("\n┌──────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  [S→C]  Preset: %-8s  shm=%2uMB  queue=%3u  slice=%-6s  max=%-6s        │\n",
           p->name, p->shm>>20, p->cap, fmtsz(p->slice), fmtsz(p->max_pay));
    printf("├──────────┬────────┬──────┬───────┬───────┬──────────┬────────┬──────────┤\n");
    printf("│ Payload  │  Mode  │ msgs │  sent │  drop │    MB/s  │   recv │  result  │\n");
    printf("├──────────┼────────┼──────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_footer(void) {
    printf("└──────────┴────────┴──────┴───────┴───────┴──────────┴────────┴──────────┘\n");
}
static void print_sep(void) {
    printf("├──────────┼────────┼──────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_row(const char *payload, const char *mode,
                      int nmsg, uint32_t sent, uint32_t drop,
                      double mbps, uint32_t recv, const char *verdict) {
    printf("│ %8s │ %s │ %4d │ %5u │ %5u │ %8.2f │ %6u │%s│\n",
           payload, mode, nmsg, sent, drop, mbps, recv, verdict);
}

/* ── Server process (parent) ──────────────────────────────────────────── */
static void run_server(int s2c_wr, int c2s_rd, const preset_t *p) {
    srv_ctx_t ctx = { NULL, 0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);

    if (shmipc_server_start(srv, p->chan) != SHMIPC_OK) {
        fprintf(stderr, "[server] start failed on '%s'\n", p->chan);
        shmipc_server_destroy(srv); return;
    }

    char ch; pipe_rd(c2s_rd, &ch, 1);   /* wait for client READY */

    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) { fprintf(stderr, "[server] no connect\n"); goto done; }

    print_header(p);

    uint8_t *buf = (uint8_t*)malloc(HDR_SZ + p->max_pay);
    if (!buf) { perror("malloc"); goto done; }

    for (int si = 0; si < p->n_pay; si++) {
        uint32_t plen  = p->pays[si];
        uint32_t total = HDR_SZ + plen;
        int      nmsg  = msgs_for(plen);

        for (int mi = 0; mi < N_MOD; mi++) {
            int32_t tmo = MODES[mi];

            ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1);            /* ACK */

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

            double mbps = (ms > 0.01)
                        ? (double)ok * total / (1<<20) / (ms / 1000.0) : 0.0;
            const char *verdict;
            if (cr.errs > 0)                         verdict = "  FAIL  ";
            else if (tmo == SHMIPC_TIMEOUT_INFINITE) verdict = (cr.recv == ok) ? "  PASS  " : " MISS!  ";
            else                                     verdict = (cr.recv <= ok)  ? "  OK    " : " EXTRA? ";

            print_row(fmtsz(plen), MODE_STR[mi],
                      nmsg, ok, drop + tout, mbps, cr.recv, verdict);
        }
        if (si < p->n_pay - 1) print_sep();
    }

    print_footer();
    free(buf);

done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

/* ── Client process (child) ───────────────────────────────────────────── */
static void run_client(int s2c_rd, int c2s_wr, const preset_t *p) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t cfg  = { p->shm, p->cap, p->slice };
    shmipc_client_set_config(cli, &cfg);

    cli_ctx_t ctx = { 0, 0, 0, 0 };
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_register_on_data(cli, cli_on_data);

    int conn = 0;
    for (int t = 0; t < 50 && !conn; t++) {
        if (shmipc_client_connect(cli, p->chan) == SHMIPC_OK) conn = 1;
        else usleep(100000);
    }
    if (!conn) { fprintf(stderr, "[client] connect failed\n"); exit(1); }

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);   /* READY */

    int done = 0;
    for (int si = 0; si < p->n_pay && !done; si++) {
        uint32_t total = HDR_SZ + p->pays[si];

        for (int mi = 0; mi < N_MOD && !done; mi++) {
            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') { done = 1; break; }

            ctx.recv           = 0;
            ctx.errs           = 0;
            ctx.stop_seen      = 0;
            ctx.expected_total = total;

            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

            wait_flag(&ctx.stop_seen, 120);

            cli_result_t cr = { ctx.recv, ctx.errs };
            pipe_wr(c2s_wr, &cr, sizeof cr);
        }
    }
    if (!done) pipe_rd(s2c_rd, &ch, 1);   /* consume 'X' */

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ════════════════════════════════════════════════════════════════════════
 * Multi-Writer stress: N concurrent threads all call shmipc_session_write
 * on the SAME session handle, exercising the internal mutex.
 *
 * Matrix: 3 payload sizes × 3 thread counts (BLOCK mode only)
 * ════════════════════════════════════════════════════════════════════════ */

#define MT_MSGS_PER  25                                   /* msgs per thread */
static const uint32_t MT_PAYS[]    = { 1024, 65536, 1048576 };
static const int      MT_THREADS[] = { 2, 4, 8 };
#define N_MT_PAY  3
#define N_MT_THR  3

typedef struct { shmipc_session_t *sess; uint32_t plen; uint32_t ok, drop; } mt_args_t;

static void *mt_writer(void *v) {
    mt_args_t *a  = (mt_args_t*)v;
    uint32_t   tot = HDR_SZ + a->plen;
    uint8_t   *buf = (uint8_t*)malloc(tot);
    if (!buf) return NULL;
    for (int i = 0; i < MT_MSGS_PER; i++) {
        fill_msg(buf, (uint32_t)i, a->plen);
        int rc = shmipc_session_write(a->sess, buf, tot, SHMIPC_TIMEOUT_INFINITE);
        if (rc == SHMIPC_OK) a->ok++; else a->drop++;
    }
    free(buf); return NULL;
}

static void print_mt_header(void) {
    printf("\n┌────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  [S→C Multi-Writer]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  mode: BLOCK │\n");
    printf("├──────────┬─────────┬─────────┬───────┬───────┬──────────┬────────┬──────────┤\n");
    printf("│ Payload  │ Threads │  total  │  sent │  drop │    MB/s  │   recv │  result  │\n");
    printf("├──────────┼─────────┼─────────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_mt_sep(void) {
    printf("├──────────┼─────────┼─────────┼───────┼───────┼──────────┼────────┼──────────┤\n");
}
static void print_mt_footer(void) {
    printf("└──────────┴─────────┴─────────┴───────┴───────┴──────────┴────────┴──────────┘\n");
}
static void print_mt_row(const char *pay, int nth, int tot,
                         uint32_t sent, uint32_t drop, double mbps,
                         uint32_t recv, const char *v) {
    printf("│ %8s │    %d    │ %7d │ %5u │ %5u │ %8.2f │ %6u │%s│\n",
           pay, nth, tot, sent, drop, mbps, recv, v);
}

static void run_server_mt(int s2c_wr, int c2s_rd) {
    srv_ctx_t ctx = { NULL, 0 };
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);
    if (shmipc_server_start(srv, "t1_mt") != SHMIPC_OK) {
        shmipc_server_destroy(srv); return;
    }
    char ch; pipe_rd(c2s_rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto mt_done;

    print_mt_header();
    for (int pi = 0; pi < N_MT_PAY; pi++) {
        uint32_t plen = MT_PAYS[pi], total = HDR_SZ + plen;
        for (int ti = 0; ti < N_MT_THR; ti++) {
            int nth = MT_THREADS[ti];
            ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1);                        /* ACK */

            mt_args_t  args[8]; pthread_t tids[8];
            double t0 = now_ms();
            for (int t = 0; t < nth; t++) {
                args[t] = (mt_args_t){ ctx.sess, plen, 0, 0 };
                pthread_create(&tids[t], NULL, mt_writer, &args[t]);
            }
            for (int t = 0; t < nth; t++) pthread_join(tids[t], NULL);
            double ms = now_ms() - t0;

            uint32_t tok = 0, tdrp = 0;
            for (int t = 0; t < nth; t++) { tok += args[t].ok; tdrp += args[t].drop; }

            uint8_t stop[HDR_SZ]; fill_stop(stop);
            while (shmipc_session_write(ctx.sess, stop, HDR_SZ,
                                        SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}
            cli_result_t cr; pipe_rd(c2s_rd, &cr, sizeof cr);

            double mbps = (ms > 0.01) ? (double)tok * total / (1<<20) / (ms/1000.0) : 0.0;
            const char *verdict = cr.errs  ? "  FAIL  "
                                : (cr.recv == tok ? "  PASS  " : " MISS!  ");
            print_mt_row(fmtsz(plen), nth, nth * MT_MSGS_PER,
                         tok, tdrp, mbps, cr.recv, verdict);
        }
        if (pi < N_MT_PAY - 1) print_mt_sep();
    }
    print_mt_footer();

mt_done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_mt_s2c(int s2c_rd, int c2s_wr) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t  cfg = { 16u<<20, 64, 16384 };
    shmipc_client_set_config(cli, &cfg);
    cli_ctx_t ctx = { 0, 0, 0, 0 };
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_register_on_data(cli, cli_on_data);
    int conn = 0;
    for (int t = 0; t < 50 && !conn; t++) {
        if (shmipc_client_connect(cli, "t1_mt") == SHMIPC_OK) conn = 1;
        else usleep(100000);
    }
    if (!conn) exit(1);
    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);
    int done = 0;
    for (int pi = 0; pi < N_MT_PAY && !done; pi++) {
        uint32_t total = HDR_SZ + MT_PAYS[pi];
        for (int ti = 0; ti < N_MT_THR && !done; ti++) {
            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') { done = 1; break; }
            ctx.recv = 0; ctx.errs = 0; ctx.stop_seen = 0;
            ctx.expected_total = total;
            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);
            wait_flag(&ctx.stop_seen, 120);
            cli_result_t cr = { ctx.recv, ctx.errs };
            pipe_wr(c2s_wr, &cr, sizeof cr);
        }
    }
    if (!done) pipe_rd(s2c_rd, &ch, 1);
    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    printf("\n╔════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   shmipc  ❯  test1: Server → Client  (%d+%d+%d payloads × %d modes × 3 presets)   ║\n",
           N_LF, N_GEN, N_HI, N_MOD);
    printf("╚════════════════════════════════════════════════════════════════════════════════════╝\n");

    int all_pass = 1;
    for (int pi = 0; pi < N_PRE; pi++) {
        int s2c[2], c2s[2];
        if (pipe(s2c) || pipe(c2s)) { perror("pipe"); return 1; }

        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }

        if (child == 0) {
            close(s2c[1]); close(c2s[0]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
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

    /* ── Multi-writer stress ─────────────────────────────────── */
    {
        int s2c[2], c2s[2];
        if (pipe(s2c) || pipe(c2s)) { perror("pipe"); return 1; }
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (child == 0) {
            close(s2c[1]); close(c2s[0]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
            run_client_mt_s2c(s2c[0], c2s[1]);
            close(s2c[0]); close(c2s[1]); exit(0);
        }
        close(s2c[0]); close(c2s[1]);
        run_server_mt(s2c[1], c2s[0]);
        close(s2c[1]); close(c2s[0]);
        int st = 0; waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_pass = 0;
    }

    printf("\nOverall: %s\n\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}
