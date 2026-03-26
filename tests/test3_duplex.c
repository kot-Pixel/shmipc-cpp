/*
 * test3_duplex.c — Full-duplex asymmetric benchmark
 *
 * Both directions transfer data simultaneously using pthread send-threads.
 * The two sides carry DIFFERENT payload sizes to stress asymmetric workloads:
 *
 *   server sends LARGE messages  ←→  client sends SMALL messages
 *   (and the reversed pairs too)
 *
 * Test matrix:
 *   3 presets × N filtered pairs (both payloads ≤ preset max) × 3 modes
 *
 *   Pairs defined in ALL_PAIRS (from 256 B ↔ 8 MB down to symmetric):
 *     256B↔8MB  1KB↔4MB  4KB↔1MB  16KB↔256KB  64KB↔64KB
 *     256KB↔16KB  1MB↔4KB  4MB↔1KB  8MB↔256B
 *   Each preset only runs pairs where BOTH payloads ≤ preset max.
 *
 * Architecture: fork() — parent = server, child = client.
 * Both sides spawn a pthread send-thread per test case.
 *
 * Pipe protocol (srv2cli / cli2srv):
 *   'D'                 = duplex go
 *   'A'                 = ACK (client ready, both sides start sending)
 *   'X'                 = exit
 *   duplex_cli_stats_t  = client reports {send stats + recv stats} to server
 */

#include "test_common.h"
#include <pthread.h>

/* ── Duplex payload pairs (srv payload, cli payload) ─────────────────── */
typedef struct { uint32_t srv; uint32_t cli; } pair_t;

static const pair_t ALL_PAIRS[] = {
    {    256, 8u<<20 },   /*  256 B  ↔  8 MB  */
    { 1u<<10, 4u<<20 },   /*   1 KB  ↔  4 MB  */
    { 4u<<10, 1u<<20 },   /*   4 KB  ↔  1 MB  */
    {16u<<10,256u<<10},   /*  16 KB  ↔ 256 KB */
    {64u<<10, 64u<<10},   /*  64 KB  ↔  64 KB (symmetric) */
    {256u<<10,16u<<10},   /* 256 KB  ↔  16 KB */
    { 1u<<20, 4u<<10 },   /*   1 MB  ↔   4 KB */
    { 4u<<20, 1u<<10 },   /*   4 MB  ↔   1 KB */
    { 8u<<20,    256 },   /*   8 MB  ↔  256 B  */
};
#define N_ALL_PAIRS (int)(sizeof(ALL_PAIRS)/sizeof(ALL_PAIRS[0]))

/* ── Presets ──────────────────────────────────────────────────────────── */
typedef struct {
    const char *name, *chan;
    uint32_t    shm, cap, slice;
    uint32_t    max_pay;
} preset3_t;

static const preset3_t PRESETS[] = {
    { "LOW_FREQ", "t3_lf",   8u<<20,  32,  4096,  1u<<20 },
    { "GENERAL",  "t3_gen", 16u<<20,  64, 16384,  4u<<20 },
    { "HI_THRU",  "t3_hi",  64u<<20, 256, 65536,  8u<<20 },
};
#define N_PRE 3

/* ── Generic send thread (works for server session AND client) ────────── */
typedef struct {
    shmipc_session_t *sess;   /* non-NULL if server */
    shmipc_client_t  *cli;    /* non-NULL if client */
    uint32_t          plen;
    int32_t           tmo;
    int               nmsg;
    /* output */
    uint32_t          ok, drop, tout;
    double            ms;
} send_args_t;

static void *send_thread_func(void *varg) {
    send_args_t *a  = (send_args_t*)varg;
    uint32_t total  = HDR_SZ + a->plen;
    uint8_t *buf    = (uint8_t*)malloc(total);
    if (!buf) return NULL;

    double t0 = now_ms();
    for (int i = 0; i < a->nmsg; i++) {
        fill_msg(buf, (uint32_t)i, a->plen);
        int rc;
        if (a->sess) rc = shmipc_session_write(a->sess, buf, total, a->tmo);
        else         rc = shmipc_client_write (a->cli,  buf, total, a->tmo);
        if      (rc == SHMIPC_OK)      a->ok++;
        else if (rc == SHMIPC_TIMEOUT) a->tout++;
        else                           a->drop++;
    }
    /* Always send STOP with infinite blocking to guarantee delivery */
    uint8_t sbuf[HDR_SZ];
    fill_stop(sbuf);
    for (;;) {
        int rc;
        if (a->sess) rc = shmipc_session_write(a->sess, sbuf, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);
        else         rc = shmipc_client_write (a->cli,  sbuf, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);
        if (rc == SHMIPC_OK) break;
    }
    a->ms = now_ms() - t0;
    free(buf);
    return NULL;
}

/* ── Contexts & callbacks ─────────────────────────────────────────────── */
typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
    volatile uint32_t srv_recv;
    volatile uint32_t srv_errs;
    uint32_t          srv_expected_total;
    volatile int      srv_stop_seen;
} srv_ctx_t;

static void srv_on_connected   (shmipc_session_t *s, void *ctx)
    { ((srv_ctx_t*)ctx)->sess = s; ((srv_ctx_t*)ctx)->connected = 1; }
static void srv_on_disconnected(shmipc_session_t *s, void *ctx)
    { (void)s; ((srv_ctx_t*)ctx)->connected = 0; }

static void srv_on_data(shmipc_session_t *s,
                        const void *data, uint32_t len, void *ctx) {
    (void)s;
    srv_ctx_t *c = (srv_ctx_t*)ctx;
    if (len < HDR_SZ) { c->srv_errs++; return; }
    uint32_t seq; memcpy(&seq, data, 4);
    if (seq == STOP_SEQ) { c->srv_stop_seen = 1; return; }
    c->srv_recv++;
    if (len != c->srv_expected_total) c->srv_errs++;
}

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

/* Stats struct sent by child (client) back to parent (server) */
typedef struct {
    uint32_t c_ok, c_drop, c_tout, c_us;   /* client send */
    uint32_t c_recv, c_errs;               /* client recv */
} duplex_cli_stats_t;

/* ── Table printing ───────────────────────────────────────────────────── */
static void print_header(const preset3_t *p) {
    printf("\n┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  [DUPLEX]  Preset: %-8s  shm=%2uMB  queue=%3u  slice=%-6s  max=%-6s                                     │\n",
           p->name, p->shm>>20, p->cap, fmtsz(p->slice), fmtsz(p->max_pay));
    printf("├──────────┬──────────┬────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤\n");
    printf("│  SrvPay  │  CliPay  │  Mode  │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │\n");
    printf("├──────────┼──────────┼────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_footer(void) {
    printf("└──────────┴──────────┴────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘\n");
}
static void print_sep(void) {
    printf("├──────────┼──────────┼────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_row(const char *spay, const char *cpay, const char *mode,
                      uint32_t ssent, uint32_t sdrp, double s2c_mbps, uint32_t crecv,
                      uint32_t csent, uint32_t cdrp, double c2s_mbps, uint32_t srecv,
                      const char *verdict) {
    printf("│ %8s │ %8s │ %s │ %5u │ %4u │ %8.2f │ %6u │ %5u │ %4u │ %8.2f │ %6u │%s│\n",
           spay, cpay, mode,
           ssent, sdrp, s2c_mbps, crecv,
           csent, cdrp, c2s_mbps, srecv,
           verdict);
}

/* ── Server process (parent) ──────────────────────────────────────────── */
static void run_server(int s2c_wr, int c2s_rd, const preset3_t *p) {
    srv_ctx_t ctx = { NULL, 0, 0, 0, 0, 0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_data        (srv, srv_on_data);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);

    if (shmipc_server_start(srv, p->chan) != SHMIPC_OK) {
        fprintf(stderr, "[server] start failed on '%s'\n", p->chan);
        shmipc_server_destroy(srv); return;
    }

    char ch; pipe_rd(c2s_rd, &ch, 1);   /* wait for client READY */

    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) { fprintf(stderr, "[server] no connect\n"); goto done; }

    print_header(p);

    int first_pair = 1;
    for (int pi = 0; pi < N_ALL_PAIRS; pi++) {
        const pair_t *pair = &ALL_PAIRS[pi];
        if (pair->srv > p->max_pay || pair->cli > p->max_pay) continue;

        uint32_t srv_total = HDR_SZ + pair->srv;
        uint32_t cli_total = HDR_SZ + pair->cli;
        int      nmsg_srv  = msgs_for(pair->srv);
        int      nmsg_cli  = msgs_for(pair->cli);

        if (!first_pair) print_sep();
        first_pair = 0;

        for (int mi = 0; mi < N_MOD; mi++) {
            int32_t tmo = MODES[mi];

            /* Reset server receive counters */
            ctx.srv_recv           = 0;
            ctx.srv_errs           = 0;
            ctx.srv_expected_total = cli_total;  /* server expects cli-sized messages */
            ctx.srv_stop_seen      = 0;

            /* Signal client and wait for ACK (client resets its counters before ACK) */
            ch = 'D'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1);             /* ACK — client ready */

            /* Spawn server send thread */
            send_args_t sargs = { ctx.sess, NULL, pair->srv, tmo, nmsg_srv, 0, 0, 0, 0.0 };
            pthread_t   stid;
            pthread_create(&stid, NULL, send_thread_func, &sargs);

            pthread_join(stid, NULL);

            /* Wait for C→S STOP (client's messages all arrived) */
            wait_flag(&ctx.srv_stop_seen, 120);

            /* Read client's combined stats */
            duplex_cli_stats_t cs; pipe_rd(c2s_rd, &cs, sizeof cs);

            /* Compute throughputs */
            double s2c_ms   = sargs.ms;
            double c2s_ms   = cs.c_us / 1000.0;
            double s2c_mbps = (s2c_ms > 0.01)
                            ? (double)sargs.ok * srv_total / (1<<20) / (s2c_ms / 1000.0) : 0.0;
            double c2s_mbps = (c2s_ms > 0.01)
                            ? (double)cs.c_ok * cli_total / (1<<20) / (c2s_ms / 1000.0) : 0.0;

            /* Verdict: both directions must pass independently */
            int s2c_pass, c2s_pass;
            if (tmo == SHMIPC_TIMEOUT_INFINITE) {
                s2c_pass = (cs.c_errs  == 0) && (cs.c_recv   == sargs.ok);
                c2s_pass = (ctx.srv_errs == 0) && (ctx.srv_recv == cs.c_ok);
            } else {
                s2c_pass = (cs.c_errs  == 0) && (cs.c_recv  <= sargs.ok);
                c2s_pass = (ctx.srv_errs == 0) && (ctx.srv_recv <= cs.c_ok);
            }
            const char *verdict;
            if (!s2c_pass || !c2s_pass)          verdict = "  FAIL  ";
            else if (tmo == SHMIPC_TIMEOUT_INFINITE) verdict = "  PASS  ";
            else                                  verdict = "  OK    ";

            print_row(fmtsz(pair->srv), fmtsz(pair->cli), MODE_STR[mi],
                      sargs.ok, sargs.drop + sargs.tout, s2c_mbps, cs.c_recv,
                      cs.c_ok,  cs.c_drop + cs.c_tout,  c2s_mbps, ctx.srv_recv,
                      verdict);
        }
    }

    print_footer();

done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

/* ── Client process (child) ───────────────────────────────────────────── */
static void run_client(int s2c_rd, int c2s_wr, const preset3_t *p) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t  cfg = { p->shm, p->cap, p->slice };
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

    for (int pi = 0; pi < N_ALL_PAIRS; pi++) {
        const pair_t *pair = &ALL_PAIRS[pi];
        if (pair->srv > p->max_pay || pair->cli > p->max_pay) continue;

        uint32_t srv_total = HDR_SZ + pair->srv;
        int      nmsg_cli  = msgs_for(pair->cli);

        for (int mi = 0; mi < N_MOD; mi++) {
            int32_t tmo = MODES[mi];

            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') { goto done; }

            /* Reset receive counters BEFORE sending ACK */
            ctx.recv           = 0;
            ctx.errs           = 0;
            ctx.stop_seen      = 0;
            ctx.expected_total = srv_total;   /* client expects srv-sized messages */

            /* ACK — server will start its send thread right after reading this */
            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

            /* Spawn client send thread immediately after ACK */
            send_args_t cargs = { NULL, cli, pair->cli, tmo, nmsg_cli, 0, 0, 0, 0.0 };
            pthread_t   ctid;
            pthread_create(&ctid, NULL, send_thread_func, &cargs);

            pthread_join(ctid, NULL);

            /* Wait for S→C STOP */
            wait_flag(&ctx.stop_seen, 120);

            duplex_cli_stats_t cs = {
                cargs.ok, cargs.drop, cargs.tout,
                (uint32_t)(cargs.ms * 1000.0),
                ctx.recv, ctx.errs
            };
            pipe_wr(c2s_wr, &cs, sizeof cs);
        }
    }

done:
    /* Drain remaining 'X' if we exited the loop normally */
    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ════════════════════════════════════════════════════════════════════════
 * Full-Duplex Multi-Writer stress
 * Both sides spawn N threads simultaneously, writing asymmetric payloads.
 * Matrix: 3 asymmetric pairs × 3 thread counts (BLOCK mode only).
 * ════════════════════════════════════════════════════════════════════════ */

#define MT_MSGS_PER  25
static const pair_t   MT_PAIRS[]   = {
    { 4u<<10, 1u<<20 },   /*  4KB  ↔  1MB */
    { 64u<<10, 64u<<10 }, /* 64KB  ↔ 64KB */
    { 1u<<20, 4u<<10  },  /*  1MB  ↔  4KB */
};
static const int MT_THREADS[] = { 2, 4, 8 };
#define N_MT_PAIRS 3
#define N_MT_THR   3

/* Send-only thread (no STOP sentinel — main thread sends it after join) */
typedef struct {
    shmipc_session_t *sess;
    shmipc_client_t  *cli;
    uint32_t          plen;
    int32_t           tmo;    /* write mode: SHMIPC_TIMEOUT_INFINITE, SHMIPC_NONBLOCK, or ms */
    uint32_t          ok, drop;
    double            ms;
} mt3_args_t;

static void *mt3_send_thread(void *v) {
    mt3_args_t *a   = (mt3_args_t*)v;
    uint32_t    tot = HDR_SZ + a->plen;
    uint8_t    *buf = (uint8_t*)malloc(tot);
    if (!buf) return NULL;
    double t0 = now_ms();
    for (int i = 0; i < MT_MSGS_PER; i++) {
        fill_msg(buf, (uint32_t)i, a->plen);
        int rc;
        if (a->sess) rc = shmipc_session_write(a->sess, buf, tot, a->tmo);
        else         rc = shmipc_client_write (a->cli,  buf, tot, a->tmo);
        if (rc == SHMIPC_OK) a->ok++; else a->drop++;
    }
    a->ms = now_ms() - t0;
    free(buf); return NULL;
}

static void print_mt3_header(void) {
    printf("\n┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  [DUPLEX Multi-Writer]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  mode: BLOCK                          │\n");
    printf("├──────────┬──────────┬─────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤\n");
    printf("│  SrvPay  │  CliPay  │ Threads │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │\n");
    printf("├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_mt3_sep(void) {
    printf("├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_mt3_footer(void) {
    printf("└──────────┴──────────┴─────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘\n");
}
static void print_mt3_row(const char *sp, const char *cp, int nth,
                          uint32_t ssent, uint32_t sdrp, double s2c_mbps, uint32_t crecv,
                          uint32_t csent, uint32_t cdrp, double c2s_mbps, uint32_t srecv,
                          const char *v) {
    printf("│ %8s │ %8s │    %d    │ %5u │ %4u │ %8.2f │ %6u │ %5u │ %4u │ %8.2f │ %6u │%s│\n",
           sp, cp, nth, ssent, sdrp, s2c_mbps, crecv, csent, cdrp, c2s_mbps, srecv, v);
}

static void run_server_duplex_mt(int s2c_wr, int c2s_rd) {
    srv_ctx_t ctx = { NULL, 0, 0, 0, 0, 0 };
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_data        (srv, srv_on_data);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);
    if (shmipc_server_start(srv, "t3_mt") != SHMIPC_OK) {
        shmipc_server_destroy(srv); return;
    }
    char ch; pipe_rd(c2s_rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto mt_done;

    print_mt3_header();
    int first = 1;
    for (int pi = 0; pi < N_MT_PAIRS; pi++) {
        const pair_t *pair = &MT_PAIRS[pi];
        if (!first) print_mt3_sep(); first = 0;
        for (int ti = 0; ti < N_MT_THR; ti++) {
            int nth = MT_THREADS[ti];
            uint32_t cli_total = HDR_SZ + pair->cli;
            uint32_t srv_total = HDR_SZ + pair->srv;

            ctx.srv_recv           = 0;
            ctx.srv_errs           = 0;
            ctx.srv_expected_total = cli_total;
            ctx.srv_stop_seen      = 0;

            ch = 'D'; pipe_wr(s2c_wr, &ch, 1);
            pipe_rd(c2s_rd, &ch, 1);                /* ACK */

            /* Spawn N server sender threads */
            mt3_args_t  args[8]; pthread_t tids[8];
            double t0 = now_ms();
            for (int t = 0; t < nth; t++) {
                args[t] = (mt3_args_t){ ctx.sess, NULL, pair->srv, SHMIPC_TIMEOUT_INFINITE, 0, 0, 0.0 };
                pthread_create(&tids[t], NULL, mt3_send_thread, &args[t]);
            }
            for (int t = 0; t < nth; t++) pthread_join(tids[t], NULL);
            double srv_ms = now_ms() - t0;

            /* Send S→C STOP after all threads done */
            uint8_t stop[HDR_SZ]; fill_stop(stop);
            while (shmipc_session_write(ctx.sess, stop, HDR_SZ,
                                        SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

            wait_flag(&ctx.srv_stop_seen, 120);

            duplex_cli_stats_t cs; pipe_rd(c2s_rd, &cs, sizeof cs);

            uint32_t srv_ok = 0, srv_drp = 0;
            for (int t = 0; t < nth; t++) { srv_ok += args[t].ok; srv_drp += args[t].drop; }

            double s2c_mbps = (srv_ms > 0.01)
                ? (double)srv_ok * srv_total / (1<<20) / (srv_ms / 1000.0) : 0.0;
            double c2s_ms   = cs.c_us / 1000.0;
            double c2s_mbps = (c2s_ms > 0.01)
                ? (double)cs.c_ok * cli_total / (1<<20) / (c2s_ms / 1000.0) : 0.0;

            int s2c_ok = (cs.c_errs == 0) && (cs.c_recv == srv_ok);
            int c2s_ok = (ctx.srv_errs == 0) && (ctx.srv_recv == cs.c_ok);
            const char *verdict = (!s2c_ok || !c2s_ok) ? "  FAIL  " : "  PASS  ";

            print_mt3_row(fmtsz(pair->srv), fmtsz(pair->cli), nth,
                          srv_ok, srv_drp, s2c_mbps, cs.c_recv,
                          cs.c_ok, cs.c_drop, c2s_mbps, ctx.srv_recv,
                          verdict);
        }
    }
    print_mt3_footer();

mt_done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_duplex_mt(int s2c_rd, int c2s_wr) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t  cfg = { 16u<<20, 64, 16384 };
    shmipc_client_set_config(cli, &cfg);
    cli_ctx_t ctx = { 0, 0, 0, 0 };
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_register_on_data(cli, cli_on_data);
    int conn = 0;
    for (int t = 0; t < 50 && !conn; t++) {
        if (shmipc_client_connect(cli, "t3_mt") == SHMIPC_OK) conn = 1;
        else usleep(100000);
    }
    if (!conn) exit(1);

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);
    for (int pi = 0; pi < N_MT_PAIRS; pi++) {
        const pair_t *pair = &MT_PAIRS[pi];
        for (int ti = 0; ti < N_MT_THR; ti++) {
            int nth = MT_THREADS[ti];
            pipe_rd(s2c_rd, &ch, 1);
            if (ch == 'X') goto mt_done;

            ctx.recv           = 0;
            ctx.errs           = 0;
            ctx.stop_seen      = 0;
            ctx.expected_total = HDR_SZ + pair->srv;

            ch = 'A'; pipe_wr(c2s_wr, &ch, 1);         /* ACK — server starts threads */

            /* Spawn N client sender threads immediately after ACK */
            mt3_args_t  args[8]; pthread_t tids[8];
            double t0 = now_ms();
            for (int t = 0; t < nth; t++) {
                args[t] = (mt3_args_t){ NULL, cli, pair->cli, SHMIPC_TIMEOUT_INFINITE, 0, 0, 0.0 };
                pthread_create(&tids[t], NULL, mt3_send_thread, &args[t]);
            }
            for (int t = 0; t < nth; t++) pthread_join(tids[t], NULL);
            double ms = now_ms() - t0;

            /* Send C→S STOP after all threads done */
            uint8_t stop[HDR_SZ]; fill_stop(stop);
            while (shmipc_client_write(cli, stop, HDR_SZ,
                                       SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

            wait_flag(&ctx.stop_seen, 120);

            uint32_t c_ok = 0, c_drp = 0;
            for (int t = 0; t < nth; t++) { c_ok += args[t].ok; c_drp += args[t].drop; }

            duplex_cli_stats_t cs = {
                c_ok, c_drp, 0, (uint32_t)(ms * 1000.0),
                ctx.recv, ctx.errs
            };
            pipe_wr(c2s_wr, &cs, sizeof cs);
        }
    }
mt_done:
    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ════════════════════════════════════════════════════════════════════════
 * MIXED-MODE Multi-Writer: one side BLOCK, the other NONBLOCK.
 *
 * When a NONBLOCK sender floods with N threads and the remote consumer
 * can't keep up, writes are dropped immediately rather than blocking.
 * This section verifies:
 *   - No data corruption (size mismatches = 0)
 *   - recv count == actually-sent (ok) count, regardless of drops
 *   - BLOCK side has 0 drops; NONBLOCK side may drop under contention
 *
 * Matrix: 2 mode combos × 3 payload pairs × 3 thread counts
 *   combo A: SRV=BLOCK  / CLI=NONBLK
 *   combo B: SRV=NONBLK / CLI=BLOCK
 * ════════════════════════════════════════════════════════════════════════ */

/* Command sent from server to client via pipe for each sub-test */
typedef struct {
    uint32_t srv_plen;   /* server's payload (= client receives this size) */
    uint32_t cli_plen;   /* client's payload (= server receives this size) */
    int32_t  cli_tmo;    /* write mode the CLIENT should use */
    int      nth;        /* thread count */
} mix_cmd_t;

static const char *tmo_label(int32_t tmo) {
    return (tmo == SHMIPC_TIMEOUT_INFINITE) ? "BLOCK  " : "NONBLK";
}

static void print_mix3_header(const char *srv_lbl, const char *cli_lbl) {
    printf("\n┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│  [DUPLEX Mixed-Mode MT]  Preset: GENERAL  shm=16MB  queue=64  slice=16KB  SrvMode=%-7s  CliMode=%-7s  threads: 2/4/8  │\n",
           srv_lbl, cli_lbl);
    printf("├──────────┬──────────┬─────────┬───────┬──────┬──────────┬────────┬───────┬──────┬──────────┬────────┬─────────┤\n");
    printf("│  SrvPay  │  CliPay  │ Threads │ Ssent │ Sdrp │ S→C MB/s │  Crecv │ Csent │ Cdrp │ C→S MB/s │  Srecv │ result  │\n");
    printf("├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_mix3_sep(void) {
    printf("├──────────┼──────────┼─────────┼───────┼──────┼──────────┼────────┼───────┼──────┼──────────┼────────┼─────────┤\n");
}
static void print_mix3_footer(void) {
    printf("└──────────┴──────────┴─────────┴───────┴──────┴──────────┴────────┴───────┴──────┴──────────┴────────┴─────────┘\n");
}
static void print_mix3_row(const char *sp, const char *cp, int nth,
                           uint32_t ssent, uint32_t sdrp, double s2c_mbps, uint32_t crecv,
                           uint32_t csent, uint32_t cdrp, double c2s_mbps, uint32_t srecv,
                           const char *v) {
    printf("│ %8s │ %8s │    %d    │ %5u │ %4u │ %8.2f │ %6u │ %5u │ %4u │ %8.2f │ %6u │%s│\n",
           sp, cp, nth, ssent, sdrp, s2c_mbps, crecv, csent, cdrp, c2s_mbps, srecv, v);
}

static void run_server_duplex_mixed(int s2c_wr, int c2s_rd) {
    srv_ctx_t ctx = { NULL, 0, 0, 0, 0, 0 };
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_connected);
    shmipc_server_register_on_data        (srv, srv_on_data);
    shmipc_server_register_on_disconnected(srv, srv_on_disconnected);
    if (shmipc_server_start(srv, "t3_mix") != SHMIPC_OK) {
        shmipc_server_destroy(srv); return;
    }
    char ch; pipe_rd(c2s_rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto mix_done;

    /* Two mode combos: (SRV=BLOCK/CLI=NONBLK) then (SRV=NONBLK/CLI=BLOCK) */
    static const struct { int32_t srv_tmo; int32_t cli_tmo; } COMBOS[2] = {
        { SHMIPC_TIMEOUT_INFINITE, SHMIPC_TIMEOUT_NONBLOCKING },
        { SHMIPC_TIMEOUT_NONBLOCKING, SHMIPC_TIMEOUT_INFINITE },
    };

    for (int ci = 0; ci < 2; ci++) {
        int32_t srv_tmo = COMBOS[ci].srv_tmo;
        int32_t cli_tmo = COMBOS[ci].cli_tmo;
        print_mix3_header(tmo_label(srv_tmo), tmo_label(cli_tmo));

        int first = 1;
        for (int pi = 0; pi < N_MT_PAIRS; pi++) {
            const pair_t *pair = &MT_PAIRS[pi];
            if (!first) print_mix3_sep(); first = 0;

            for (int ti = 0; ti < N_MT_THR; ti++) {
                int nth = MT_THREADS[ti];
                uint32_t srv_total = HDR_SZ + pair->srv;
                uint32_t cli_total = HDR_SZ + pair->cli;

                ctx.srv_recv           = 0;
                ctx.srv_errs           = 0;
                ctx.srv_expected_total = cli_total;
                ctx.srv_stop_seen      = 0;

                /* Send sub-test parameters to client */
                mix_cmd_t cmd = { pair->srv, pair->cli, cli_tmo, nth };
                ch = 'E'; pipe_wr(s2c_wr, &ch, 1);
                pipe_wr(s2c_wr, &cmd, sizeof cmd);
                pipe_rd(c2s_rd, &ch, 1);   /* ACK — client starts its threads */

                /* Spawn N server sender threads */
                mt3_args_t args[8]; pthread_t tids[8];
                double t0 = now_ms();
                for (int t = 0; t < nth; t++) {
                    args[t] = (mt3_args_t){ ctx.sess, NULL, pair->srv, srv_tmo, 0, 0, 0.0 };
                    pthread_create(&tids[t], NULL, mt3_send_thread, &args[t]);
                }
                for (int t = 0; t < nth; t++) pthread_join(tids[t], NULL);
                double srv_ms = now_ms() - t0;

                /* S→C STOP (always BLOCK to guarantee delivery) */
                uint8_t stop[HDR_SZ]; fill_stop(stop);
                while (shmipc_session_write(ctx.sess, stop, HDR_SZ,
                                            SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

                wait_flag(&ctx.srv_stop_seen, 120);

                duplex_cli_stats_t cs; pipe_rd(c2s_rd, &cs, sizeof cs);

                uint32_t srv_ok = 0, srv_drp = 0;
                for (int t = 0; t < nth; t++) { srv_ok += args[t].ok; srv_drp += args[t].drop; }

                double s2c_mbps = (srv_ms > 0.01)
                    ? (double)srv_ok * srv_total / (1<<20) / (srv_ms / 1000.0) : 0.0;
                double c2s_ms   = cs.c_us / 1000.0;
                double c2s_mbps = (c2s_ms > 0.01)
                    ? (double)cs.c_ok * cli_total / (1<<20) / (c2s_ms / 1000.0) : 0.0;

                /* PASS: no size-error messages AND recv == actually-sent (drops are OK) */
                int s2c_ok = (cs.c_errs == 0) && (cs.c_recv == srv_ok);
                int c2s_ok = (ctx.srv_errs == 0) && (ctx.srv_recv == cs.c_ok);
                const char *verdict = (!s2c_ok || !c2s_ok) ? "  FAIL  " : "  PASS  ";

                print_mix3_row(fmtsz(pair->srv), fmtsz(pair->cli), nth,
                               srv_ok, srv_drp, s2c_mbps, cs.c_recv,
                               cs.c_ok, cs.c_drop, c2s_mbps, ctx.srv_recv,
                               verdict);
            }
        }
        print_mix3_footer();
    }

mix_done:
    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_duplex_mixed(int s2c_rd, int c2s_wr) {
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_config_t  cfg = { 16u<<20, 64, 16384 };
    shmipc_client_set_config(cli, &cfg);
    cli_ctx_t ctx = { 0, 0, 0, 0 };
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_register_on_data(cli, cli_on_data);
    int conn = 0;
    for (int t = 0; t < 50 && !conn; t++) {
        if (shmipc_client_connect(cli, "t3_mix") == SHMIPC_OK) conn = 1;
        else usleep(100000);
    }
    if (!conn) exit(1);

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);
    for (;;) {
        pipe_rd(s2c_rd, &ch, 1);
        if (ch == 'X') break;               /* 'E' = sub-test command */

        mix_cmd_t cmd; pipe_rd(s2c_rd, &cmd, sizeof cmd);

        ctx.recv           = 0;
        ctx.errs           = 0;
        ctx.stop_seen      = 0;
        ctx.expected_total = HDR_SZ + cmd.srv_plen;  /* server sends srv_plen → client receives */

        ch = 'A'; pipe_wr(c2s_wr, &ch, 1);  /* ACK — server starts its threads too */

        /* Spawn N client sender threads with the mode the server specified */
        mt3_args_t args[8]; pthread_t tids[8];
        double t0 = now_ms();
        for (int t = 0; t < cmd.nth; t++) {
            args[t] = (mt3_args_t){ NULL, cli, cmd.cli_plen, cmd.cli_tmo, 0, 0, 0.0 };
            pthread_create(&tids[t], NULL, mt3_send_thread, &args[t]);
        }
        for (int t = 0; t < cmd.nth; t++) pthread_join(tids[t], NULL);
        double ms = now_ms() - t0;

        /* C→S STOP (always BLOCK to guarantee delivery) */
        uint8_t stop[HDR_SZ]; fill_stop(stop);
        while (shmipc_client_write(cli, stop, HDR_SZ, SHMIPC_TIMEOUT_INFINITE) != SHMIPC_OK) {}

        wait_flag(&ctx.stop_seen, 120);

        uint32_t c_ok = 0, c_drp = 0;
        for (int t = 0; t < cmd.nth; t++) { c_ok += args[t].ok; c_drp += args[t].drop; }

        duplex_cli_stats_t cs = {
            c_ok, c_drp, 0, (uint32_t)(ms * 1000.0),
            ctx.recv, ctx.errs
        };
        pipe_wr(c2s_wr, &cs, sizeof cs);
    }
    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    printf("\n╔════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   shmipc  ❯  test3: Full-Duplex Asymmetric  (%d pairs × %d modes × 3 presets)       ║\n",
           N_ALL_PAIRS, N_MOD);
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

    /* ── Full-duplex multi-writer stress ───────────────────────── */
    {
        int s2c[2], c2s[2];
        if (pipe(s2c) || pipe(c2s)) { perror("pipe"); return 1; }
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (child == 0) {
            close(s2c[1]); close(c2s[0]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
            run_client_duplex_mt(s2c[0], c2s[1]);
            close(s2c[0]); close(c2s[1]); exit(0);
        }
        close(s2c[0]); close(c2s[1]);
        run_server_duplex_mt(s2c[1], c2s[0]);
        close(s2c[1]); close(c2s[0]);
        int st = 0; waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_pass = 0;
    }

    /* ── Mixed-mode (BLOCK↔NONBLK) multi-writer stress ─────────── */
    {
        int s2c[2], c2s[2];
        if (pipe(s2c) || pipe(c2s)) { perror("pipe"); return 1; }
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (child == 0) {
            close(s2c[1]); close(c2s[0]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
            run_client_duplex_mixed(s2c[0], c2s[1]);
            close(s2c[0]); close(c2s[1]); exit(0);
        }
        close(s2c[0]); close(c2s[1]);
        run_server_duplex_mixed(s2c[1], c2s[0]);
        close(s2c[1]); close(c2s[0]);
        int st = 0; waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_pass = 0;
    }

    printf("\nOverall: %s\n\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}
