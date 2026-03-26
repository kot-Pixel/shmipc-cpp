/*
 * test5_latency.c — Latency monitoring API validation
 *
 * What is measured:
 *   shmipc_client_get_latency : S→C delivery latency (server writes, client receives)
 *   shmipc_session_get_latency: C→S delivery latency (client writes, server receives)
 *
 * Three tests:
 *   [A] S→C 1 KB  × 3000 msgs — client_get_latency: verify count, ordering, sane values
 *   [B] C→S 64 KB × 3000 msgs — session_get_latency: same checks
 *   [C] reset_latency clears the counter (C→S direction)
 *
 * Note: the STOP sentinel (HDR_SZ bytes) is also enqueued as a regular SHM
 * event, so the histogram count is always N_DATA_MSGS + 1. Tests account for
 * this by accepting count == total + 1.
 */
#define _GNU_SOURCE
#include "test_common.h"

/* ── Global output handle (suppress library INFO/DEBUG noise) ──── */
static FILE *out_fp = NULL;
#define tprintf(...) fprintf(out_fp, __VA_ARGS__)

/* ── Channel config ────────────────────────────────────────────── */
#define LAT_SHM   (16u << 20)
#define LAT_CAP   64u
#define LAT_SLICE 16384u   /* 16 KB slices */

/* ── Shared contexts ───────────────────────────────────────────── */

typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
} srv_ctx_t;

static void srv_on_conn(shmipc_session_t *s, void *ctx)
    { ((srv_ctx_t*)ctx)->sess = s; ((srv_ctx_t*)ctx)->connected = 1; }
static void srv_on_disc(shmipc_session_t *s, void *ctx)
    { (void)s; ((srv_ctx_t*)ctx)->connected = 0; }

typedef struct {
    volatile uint32_t recv;
    volatile int      stop_seen;
} rx_ctx_t;

static void cli_on_data(shmipc_session_t *s, const void *d, uint32_t l, void *ctx)
{
    (void)s;
    rx_ctx_t *c = (rx_ctx_t*)ctx;
    if (l >= HDR_SZ) {
        uint32_t seq; memcpy(&seq, d, 4);
        if (seq == STOP_SEQ) { c->stop_seen = 1; return; }
    }
    c->recv++;
}

typedef struct {
    volatile uint32_t recv;
    volatile int      stop_seen;
} srv_rx_ctx_t;

typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
    srv_rx_ctx_t      rx;
} srv_rx_t;

static void srv_rx_on_conn(shmipc_session_t *s, void *ctx)
    { ((srv_rx_t*)ctx)->sess = s; ((srv_rx_t*)ctx)->connected = 1; }
static void srv_rx_on_disc(shmipc_session_t *s, void *ctx)
    { (void)s; ((srv_rx_t*)ctx)->connected = 0; }
static void srv_rx_on_data(shmipc_session_t *s, const void *d, uint32_t l, void *ctx)
{
    (void)s;
    srv_rx_t *z = (srv_rx_t*)ctx;
    if (l >= HDR_SZ) {
        uint32_t seq; memcpy(&seq, d, 4);
        if (seq == STOP_SEQ) { z->rx.stop_seen = 1; return; }
    }
    z->rx.recv++;
}

/* ── Result communicated via pipe ──────────────────────────────── */
typedef struct {
    shmipc_latency_stats_t lat;
    uint32_t               recv;
} lat_res_t;

/* ── Latency stats printer ─────────────────────────────────────── */
static void ns_str(uint64_t ns, char *buf, int sz)
{
    if      (ns >= 1000000) snprintf(buf, sz, "%.2f ms", ns / 1e6);
    else if (ns >= 1000)    snprintf(buf, sz, "%.2f µs", ns / 1e3);
    else                    snprintf(buf, sz, "%llu ns",  (unsigned long long)ns);
}

static void print_lat(const char *label,
                       const shmipc_latency_stats_t *s,
                       uint32_t recv, uint32_t sent)
{
    /* STOP sentinel is also an SHM event → histogram count = sent + 1. */
    int count_ok = (s->count == (uint64_t)sent + 1);
    int order_ok = (s->min_ns <= s->p50_ns  &&
                    s->p50_ns <= s->p90_ns   &&
                    s->p90_ns <= s->p99_ns   &&
                    s->p99_ns <= s->p999_ns  &&
                    s->p999_ns<= s->max_ns);
    int nonzero  = (s->count > 0 && s->max_ns > 0);
    const char *v = (count_ok && order_ok && nonzero) ? "PASS" : "FAIL";

    char mn[32], avg[32], p50[32], p90[32], p99[32], p999[32], mx[32];
    ns_str(s->min_ns,  mn,   sizeof(mn));
    ns_str(s->avg_ns,  avg,  sizeof(avg));
    ns_str(s->p50_ns,  p50,  sizeof(p50));
    ns_str(s->p90_ns,  p90,  sizeof(p90));
    ns_str(s->p99_ns,  p99,  sizeof(p99));
    ns_str(s->p999_ns, p999, sizeof(p999));
    ns_str(s->max_ns,  mx,   sizeof(mx));

    tprintf("\n  %-44s  recv=%u/%u  samples=%llu  %s\n",
            label, recv, sent, (unsigned long long)s->count, v);
    tprintf("    min=%-14s  avg=%-14s  p50=%s\n",  mn,  avg, p50);
    tprintf("    p90=%-14s  p99=%-14s  p999=%-14s  max=%s\n",
            p90, p99, p999, mx);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Test A: S→C 1 KB × 3000 — client_get_latency
 * ═══════════════════════════════════════════════════════════════════ */
#define A_MSGS 3000
#define A_PLEN 1024

static void run_server_a(int wr, int rd)
{
    srv_ctx_t ctx = {NULL, 0};
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_on_conn);
    shmipc_server_register_on_disconnected(srv, srv_on_disc);
    if (shmipc_server_start(srv, "t5a") != SHMIPC_OK) { goto done; }

    char ch; pipe_rd(rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto done;

    uint8_t *buf = (uint8_t*)malloc(HDR_SZ + A_PLEN);
    ch = 'G'; pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);   /* ACK */

    for (int i = 0; i < A_MSGS; i++) {
        fill_msg(buf, (uint32_t)i, A_PLEN);
        shmipc_session_write(ctx.sess, buf, HDR_SZ + A_PLEN, 0);
    }
    fill_stop(buf);
    shmipc_session_write(ctx.sess, buf, HDR_SZ, 0);
    free(buf);

    lat_res_t res;
    pipe_rd(rd, &res, sizeof(res));
    print_lat("[A] S→C 1KB×3000  client_get_latency", &res.lat, res.recv, A_MSGS);

done:
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_a(int rd, int wr)
{
    rx_ctx_t ctx = {0, 0};
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_set_config(cli, &(shmipc_config_t){LAT_SHM, LAT_CAP, LAT_SLICE});
    shmipc_client_register_on_data(cli, cli_on_data);

    char ch = 'R'; pipe_wr(wr, &ch, 1);
    for (int i = 0; i < 50; i++) {
        if (shmipc_client_connect(cli, "t5a") == SHMIPC_OK) break;
        usleep(100000);
    }
    pipe_rd(rd, &ch, 1);       /* 'G' */
    ch = 'A'; pipe_wr(wr, &ch, 1);

    while (!ctx.stop_seen) usleep(100);

    lat_res_t res;
    shmipc_client_get_latency(cli, &res.lat);
    res.recv = ctx.recv;
    pipe_wr(wr, &res, sizeof(res));

    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Test B: C→S 64 KB × 3000 — session_get_latency
 * ═══════════════════════════════════════════════════════════════════ */
#define B_MSGS 3000
#define B_PLEN 65536

static void run_server_b(int wr, int rd)
{
    srv_rx_t ctx = {NULL, 0, {0, 0}};
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_rx_on_conn);
    shmipc_server_register_on_disconnected(srv, srv_rx_on_disc);
    shmipc_server_register_on_data        (srv, srv_rx_on_data);
    if (shmipc_server_start(srv, "t5b") != SHMIPC_OK) { goto done; }

    char ch; pipe_rd(rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto done;

    ch = 'G'; pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);   /* ACK */

    while (!ctx.rx.stop_seen) usleep(100);

    shmipc_latency_stats_t lat;
    shmipc_session_get_latency(ctx.sess, &lat);
    lat_res_t res = {lat, ctx.rx.recv};
    pipe_wr(wr, &res, sizeof(res));   /* server→child: result */

    /* wait for child to print and exit */
    pipe_rd(rd, &ch, 1);

done:
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_b(int rd, int wr)
{
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_config(cli, &(shmipc_config_t){LAT_SHM, LAT_CAP, LAT_SLICE});

    char ch = 'R'; pipe_wr(wr, &ch, 1);
    for (int i = 0; i < 50; i++) {
        if (shmipc_client_connect(cli, "t5b") == SHMIPC_OK) break;
        usleep(100000);
    }
    pipe_rd(rd, &ch, 1);       /* 'G' */
    ch = 'A'; pipe_wr(wr, &ch, 1);

    uint8_t *buf = (uint8_t*)malloc(HDR_SZ + B_PLEN);
    for (int i = 0; i < B_MSGS; i++) {
        fill_msg(buf, (uint32_t)i, B_PLEN);
        shmipc_client_write(cli, buf, HDR_SZ + B_PLEN, 0);
    }
    fill_stop(buf);
    shmipc_client_write(cli, buf, HDR_SZ, 0);
    free(buf);

    /* Read result from server pipe, print, then ACK */
    lat_res_t res;
    pipe_rd(rd, &res, sizeof(res));
    print_lat("[B] C→S 64KB×3000  session_get_latency", &res.lat, res.recv, B_MSGS);

    ch = 'K'; pipe_wr(wr, &ch, 1);   /* ACK done printing */
    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Test C: reset_latency clears the counter (C→S direction)
 *  client sends N1 msgs → server queries (expect N1+1)
 *  client calls reset  → client sends N2 msgs → server queries (expect N2+1)
 * ═══════════════════════════════════════════════════════════════════ */
#define C_MSGS_1 500
#define C_MSGS_2 600
#define C_PLEN   256

typedef struct {
    uint64_t before;  /* count after batch1, before reset */
    uint64_t after;   /* count after batch2 (post-reset)  */
} reset_res_t;

static void run_server_c(int wr, int rd)
{
    srv_rx_t ctx = {NULL, 0, {0, 0}};
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, srv_rx_on_conn);
    shmipc_server_register_on_disconnected(srv, srv_rx_on_disc);
    shmipc_server_register_on_data        (srv, srv_rx_on_data);
    if (shmipc_server_start(srv, "t5c") != SHMIPC_OK) { goto done; }

    char ch; pipe_rd(rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) goto done;

    /* Batch 1 */
    ch = 'G'; pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);   /* client ACK */
    while (!ctx.rx.stop_seen) usleep(100);
    {
        shmipc_latency_stats_t s;
        shmipc_session_get_latency(ctx.sess, &s);
        reset_res_t r; r.before = s.count; r.after = 0;
        /* Reset latency on the session */
        shmipc_session_reset_latency(ctx.sess);
        pipe_wr(wr, &r, sizeof(r));   /* tell child batch1 done + count */
    }

    /* Batch 2 */
    ctx.rx.stop_seen = 0; ctx.rx.recv = 0;
    ch = 'G'; pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);   /* client ACK */
    while (!ctx.rx.stop_seen) usleep(100);
    {
        shmipc_latency_stats_t s;
        shmipc_session_get_latency(ctx.sess, &s);
        reset_res_t r2;
        pipe_rd(rd, &r2, sizeof(r2));  /* get batch1 count from child */
        r2.after = s.count;
        pipe_wr(wr, &r2, sizeof(r2));  /* send final result to child */
    }

done:
    shmipc_server_stop(srv); shmipc_server_destroy(srv);
}

static void run_client_c(int rd, int wr)
{
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_config(cli, &(shmipc_config_t){LAT_SHM, LAT_CAP, LAT_SLICE});

    char ch = 'R'; pipe_wr(wr, &ch, 1);
    for (int i = 0; i < 50; i++) {
        if (shmipc_client_connect(cli, "t5c") == SHMIPC_OK) break;
        usleep(100000);
    }

    uint8_t *buf = (uint8_t*)malloc(HDR_SZ + C_PLEN);

    /* Batch 1 */
    pipe_rd(rd, &ch, 1);   /* 'G' */
    ch = 'A'; pipe_wr(wr, &ch, 1);
    for (int i = 0; i < C_MSGS_1; i++) {
        fill_msg(buf, (uint32_t)i, C_PLEN);
        shmipc_client_write(cli, buf, HDR_SZ + C_PLEN, 0);
    }
    fill_stop(buf);
    shmipc_client_write(cli, buf, HDR_SZ, 0);

    reset_res_t batch1_info;
    pipe_rd(rd, &batch1_info, sizeof(batch1_info));   /* server sent before-reset count */

    /* Batch 2 */
    pipe_rd(rd, &ch, 1);   /* 'G' */
    ch = 'A'; pipe_wr(wr, &ch, 1);
    for (int i = 0; i < C_MSGS_2; i++) {
        fill_msg(buf, (uint32_t)i, C_PLEN);
        shmipc_client_write(cli, buf, HDR_SZ + C_PLEN, 0);
    }
    fill_stop(buf);
    shmipc_client_write(cli, buf, HDR_SZ, 0);

    free(buf);

    /* Send batch1.before count; receive final result from server */
    pipe_wr(wr, &batch1_info, sizeof(batch1_info));
    reset_res_t res;
    pipe_rd(rd, &res, sizeof(res));

    int before_ok = (res.before == (uint64_t)(C_MSGS_1 + 1));
    int after_ok  = (res.after  == (uint64_t)(C_MSGS_2 + 1));
    const char *v = (before_ok && after_ok) ? "PASS" : "FAIL";

    tprintf("\n  [C] session_reset_latency clears histogram\n");
    tprintf("      before_reset count=%llu (expect %d)  %s\n",
            (unsigned long long)res.before, C_MSGS_1 + 1, before_ok ? "ok" : "FAIL");
    tprintf("      after_reset  count=%llu (expect %d)  %s\n",
            (unsigned long long)res.after, C_MSGS_2 + 1, after_ok ? "ok" : "FAIL");
    tprintf("      overall: %s\n", v);

    shmipc_client_disconnect(cli); shmipc_client_destroy(cli);
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    /* Save real stdout on a new fd, redirect fd 1 and fd 2 to /dev/null
     * to suppress library INFO/DEBUG output.  All tprintf() calls write
     * directly to the saved fd via out_fp. */
    int saved   = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);

    out_fp = fdopen(saved, "w");
    setvbuf(out_fp, NULL, _IONBF, 0);

    tprintf("\n=== shmipc latency monitoring API test ===\n");

#define FORK_TEST(client_fn, server_fn)                                 \
    do {                                                                 \
        int _p1[2], _p2[2]; pipe(_p1); pipe(_p2);                      \
        pid_t _pid = fork();                                             \
        if (_pid == 0) {                                                 \
            dup2(devnull, STDOUT_FILENO);                                \
            /* restore out_fp for child prints (test B/C) */            \
            dup2(saved, saved);  /* keep saved open */                  \
            close(_p1[1]); close(_p2[0]);                               \
            client_fn(_p1[0], _p2[1]);                                  \
            exit(0);                                                     \
        }                                                                \
        close(_p1[0]); close(_p2[1]);                                   \
        server_fn(_p1[1], _p2[0]);                                      \
        waitpid(_pid, NULL, 0);                                          \
        close(_p1[1]); close(_p2[0]);                                   \
    } while (0)

    FORK_TEST(run_client_a, run_server_a);
    FORK_TEST(run_client_b, run_server_b);
    FORK_TEST(run_client_c, run_server_c);

#undef FORK_TEST

    tprintf("\ndone.\n");
    fclose(out_fp);
    close(devnull);
    return 0;
}
