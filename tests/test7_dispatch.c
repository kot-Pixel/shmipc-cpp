/*
 * test7_dispatch.c — async dispatch thread (ShmDispatchQueue + dispatchLoop)
 *
 * Verifies shmipc_*_set_async_dispatch(depth > 0) before start/connect:
 *   [A] S→C: server bursts messages; client on_data is slow — dispatch must
 *        drain SHM so all messages arrive in order.
 *   [B] C→S: client bursts; server on_data is slow — same on server side.
 */
#define _GNU_SOURCE
#include "test_common.h"

static FILE *out_fp = NULL;
#define tprintf(...) fprintf(out_fp, __VA_ARGS__)

static int g_failed;

#define T7_SHM   (8u << 20)
#define T7_CAP   32u
#define T7_SLICE 4096u

#define T7_MSGS     48u
#define T7_PLEN     64u
#define T7_SLOW_US  1200u
#define T7_DISPATCH 64u

typedef struct {
    uint32_t          expected_seq;
    volatile uint32_t recv;
    volatile int      stop_seen;
    volatile int      bad_order;
} order_ctx_t;

static void handle_rx_msg(order_ctx_t *c, const void *d, uint32_t l)
{
    if (l < HDR_SZ) return;
    uint32_t seq, plen;
    memcpy(&seq, d, 4);
    memcpy(&plen, d + 4, 4);
    if (seq == STOP_SEQ) {
        c->stop_seen = 1;
        return;
    }
    if (seq != c->expected_seq) {
        c->bad_order = 1;
        return;
    }
    c->expected_seq++;
    c->recv++;
    usleep(T7_SLOW_US);
}

typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
} srv_wait_t;

static void srv_wait_on_conn(shmipc_session_t *s, void *ctx)
{
    srv_wait_t *w = (srv_wait_t *)ctx;
    w->sess      = s;
    w->connected = 1;
}
static void srv_wait_on_disc(shmipc_session_t *s, void *ctx)
{
    (void)s;
    ((srv_wait_t *)ctx)->connected = 0;
}

typedef struct {
    order_ctx_t ox;
} cli_wrap_t;

static void cli_on_data(shmipc_session_t *s, const void *d, uint32_t l, void *ctx)
{
    (void)s;
    handle_rx_msg(&((cli_wrap_t *)ctx)->ox, d, l);
}

typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
    order_ctx_t       ox;
} srv_wrap_t;

static void srv_wrap_on_conn(shmipc_session_t *s, void *ctx)
{
    srv_wrap_t *w = (srv_wrap_t *)ctx;
    w->sess      = s;
    w->connected = 1;
}
static void srv_wrap_on_disc(shmipc_session_t *s, void *ctx)
{
    (void)s;
    ((srv_wrap_t *)ctx)->connected = 0;
}
static void srv_on_data(shmipc_session_t *s, const void *d, uint32_t l, void *ctx)
{
    (void)s;
    handle_rx_msg(&((srv_wrap_t *)ctx)->ox, d, l);
}

typedef struct {
    uint32_t recv;
    uint32_t expected;
    int      bad_order;
    int      stop_seen;
    int      pass;
} disp_pipe_t;

/* ══════════ Test A: S→C, client async dispatch ══════════ */
static void run_server_t7a(int wr, int rd)
{
    srv_wait_t w = {NULL, 0};
    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &w);
    shmipc_server_register_on_connected(srv, srv_wait_on_conn);
    shmipc_server_register_on_disconnected(srv, srv_wait_on_disc);
    if (shmipc_server_start(srv, "t7a") != SHMIPC_OK)
        goto done;

    char ch;
    pipe_rd(rd, &ch, 1);
    wait_flag(&w.connected, 15);
    if (!w.connected)
        goto done;

    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + T7_PLEN);
    ch = 'G';
    pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);

    for (uint32_t i = 0; i < T7_MSGS; i++) {
        fill_msg(buf, i, T7_PLEN);
        if (shmipc_session_write(w.sess, buf, HDR_SZ + T7_PLEN, 0) != SHMIPC_OK)
            break;
    }
    fill_stop(buf);
    shmipc_session_write(w.sess, buf, HDR_SZ, 0);
    free(buf);

    disp_pipe_t res;
    pipe_rd(rd, &res, sizeof(res));
    int ok = (res.pass && res.recv == T7_MSGS && !res.bad_order && res.stop_seen);
    if (!ok) g_failed++;
    tprintf("  [A] S→C  client async dispatch  recv=%u/%u  order=%s  stop=%s  %s\n",
            res.recv, T7_MSGS, res.bad_order ? "BAD" : "ok",
            res.stop_seen ? "ok" : "no", ok ? "PASS" : "FAIL");

done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static void run_client_t7a(int rd, int wr)
{
    cli_wrap_t cw;
    memset(&cw, 0, sizeof(cw));

    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_context(cli, &cw);
    shmipc_client_set_config(cli,
                             &(shmipc_config_t){T7_SHM, T7_CAP, T7_SLICE});
    shmipc_client_set_async_dispatch(cli, T7_DISPATCH);
    shmipc_client_register_on_data(cli, cli_on_data);

    char ch = 'R';
    pipe_wr(wr, &ch, 1);
    for (int i = 0; i < 50; i++) {
        if (shmipc_client_connect(cli, "t7a") == SHMIPC_OK)
            break;
        usleep(100000);
    }
    pipe_rd(rd, &ch, 1);
    ch = 'A';
    pipe_wr(wr, &ch, 1);

    double t0 = now_ms();
    while (!cw.ox.stop_seen && !cw.ox.bad_order && (now_ms() - t0) < 120000.0)
        usleep(2000);

    disp_pipe_t res;
    res.recv      = cw.ox.recv;
    res.expected  = T7_MSGS;
    res.bad_order = cw.ox.bad_order;
    res.stop_seen = cw.ox.stop_seen;
    res.pass      = (cw.ox.recv == T7_MSGS && !cw.ox.bad_order && cw.ox.stop_seen);
    pipe_wr(wr, &res, sizeof(res));

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ══════════ Test B: C→S, server async dispatch ══════════ */
static void run_server_t7b(int wr, int rd)
{
    srv_wrap_t sw;
    memset(&sw, 0, sizeof(sw));
    int child_sync = 0;

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_async_dispatch(srv, T7_DISPATCH);
    shmipc_server_set_context(srv, &sw);
    shmipc_server_register_on_connected(srv, srv_wrap_on_conn);
    shmipc_server_register_on_disconnected(srv, srv_wrap_on_disc);
    shmipc_server_register_on_data(srv, srv_on_data);
    if (shmipc_server_start(srv, "t7b") != SHMIPC_OK)
        goto done;

    char ch;
    pipe_rd(rd, &ch, 1);
    wait_flag(&sw.connected, 15);
    if (!sw.connected)
        goto done;

    ch = 'G';
    pipe_wr(wr, &ch, 1);
    pipe_rd(rd, &ch, 1);
    child_sync = 1;

    double tw = now_ms();
    while (!sw.ox.stop_seen && !sw.ox.bad_order && (now_ms() - tw) < 120000.0)
        usleep(2000);

    int ok = (sw.ox.recv == T7_MSGS && !sw.ox.bad_order && sw.ox.stop_seen);
    if (!ok) g_failed++;
    tprintf("  [B] C→S  server async dispatch  recv=%u/%u  order=%s  stop=%s  %s\n",
            sw.ox.recv, T7_MSGS, sw.ox.bad_order ? "BAD" : "ok",
            sw.ox.stop_seen ? "ok" : "no", ok ? "PASS" : "FAIL");

done:
    if (child_sync) {
        ch = 'D';
        pipe_wr(wr, &ch, 1);
    }
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static void run_client_t7b(int rd, int wr)
{
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_config(cli,
                             &(shmipc_config_t){T7_SHM, T7_CAP, T7_SLICE});
    /* No client-side dispatch — receiver is server only for this leg. */

    char ch = 'R';
    pipe_wr(wr, &ch, 1);
    for (int i = 0; i < 50; i++) {
        if (shmipc_client_connect(cli, "t7b") == SHMIPC_OK)
            break;
        usleep(100000);
    }
    pipe_rd(rd, &ch, 1);
    ch = 'A';
    pipe_wr(wr, &ch, 1);

    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + T7_PLEN);
    for (uint32_t i = 0; i < T7_MSGS; i++) {
        fill_msg(buf, i, T7_PLEN);
        shmipc_client_write(cli, buf, HDR_SZ + T7_PLEN, 0);
    }
    fill_stop(buf);
    shmipc_client_write(cli, buf, HDR_SZ, 0);
    free(buf);

    pipe_rd(rd, &ch, 1);

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int saved   = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);

    out_fp = fdopen(saved, "w");
    setvbuf(out_fp, NULL, _IONBF, 0);

    g_failed = 0;
    tprintf("\n=== shmipc async dispatch test ===\n");

#define FORK_TEST(client_fn, server_fn)                                 \
    do {                                                                 \
        int _p1[2], _p2[2];                                              \
        if (pipe(_p1) < 0 || pipe(_p2) < 0) break;                       \
        pid_t _pid = fork();                                             \
        if (_pid == 0) {                                                 \
            dup2(devnull, STDOUT_FILENO);                                \
            dup2(devnull, STDERR_FILENO);                                \
            close(_p1[1]);                                               \
            close(_p2[0]);                                               \
            client_fn(_p1[0], _p2[1]);                                   \
            exit(0);                                                     \
        }                                                                \
        close(_p1[0]);                                                   \
        close(_p2[1]);                                                   \
        server_fn(_p1[1], _p2[0]);                                       \
        waitpid(_pid, NULL, 0);                                          \
        close(_p1[1]);                                                   \
        close(_p2[0]);                                                   \
    } while (0)

    FORK_TEST(run_client_t7a, run_server_t7a);
    FORK_TEST(run_client_t7b, run_server_t7b);

#undef FORK_TEST

    tprintf("\noverall: %s\n", g_failed ? "FAIL" : "PASS");
    fclose(out_fp);
    close(devnull);
    return g_failed ? 1 : 0;
}
