/*
 * test4_zc.c — Zero-copy on_data_zc API validation
 *
 * Test sections:
 *   [ZC-S2C Integrity]       server→client, every byte verified via on_data_zc
 *   [ZC-C2S Integrity]       client→server, every byte verified via on_data_zc
 *   [ZC-WBUF-C2S / S2C]      write-side alloc_buf/send_buf (incl. multi-slice), recv ZC
 *   [ZC vs COPY Throughput]  recv MB/s comparison: on_data vs on_data_zc, S→C
 *
 * Preset: LOW_FREQ  (shm=8 MB, queue=32, slice=4 KB)
 *   payload ≤ 4096 B  →  borrowed SHM pointer  (true zero-copy path)
 *   payload >  4096 B  →  heap-copy fallback    (multi-slice path)
 *
 * Pipe protocol:
 *   parent→child  'G' = go (server about to send next batch)
 *   child→parent  'A' = ACK (client ready)
 *   child→parent  zc_result_t struct after each batch
 *   parent→child  'X' = exit
 *
 * Architecture: fork() — parent = server, child = client.
 */
#define _GNU_SOURCE
#include "test_common.h"

/* ── Output handle — set in main() to skip library INFO/DEBUG logs ── */
static FILE *out_fp = NULL;
#define tprintf(...) fprintf(out_fp, __VA_ARGS__)

/* ── Config ────────────────────────────────────────────────────────── */
#define ZC_SHM    (8u << 20)
#define ZC_CAP    32u
#define ZC_SLICE  4096u   /* LOW_FREQ slice size — ZC path boundary */
#define ZC_MSGS   30      /* messages per integrity batch            */
/* Throughput message counts scaled by payload size */
static int tput_msgs_for(uint32_t plen) {
    if (plen >= 1u<<20) return  100;
    if (plen >= 256<<10) return 200;
    if (plen >= 64<<10)  return 500;
    if (plen >= 4<<10)   return 2000;
    return 8000;
}

/* Integrity payloads — mix of single-slice (borrowed) and multi-slice (copy) */
static const uint32_t ZC_PAYS[] = {
    256,    /* single-slice, borrowed */
    1024,   /* single-slice, borrowed */
    4096,   /* single-slice, borrowed (exact boundary) */
    4097,   /* multi-slice,  heap copy (just over boundary) */
    8192,   /* multi-slice,  heap copy */
    65536,  /* multi-slice,  heap copy */
    524288, /* multi-slice,  heap copy (512 KB) */
};
#define N_ZC_PAY 7

/* Write-side ZC (alloc_buf / send_buf): boundary + multi-slice */
#define WBUF_MSGS 24
static const uint32_t WBUF_PAYS[] = { 4088u, 4090u, 8192u, 65536u };
#define N_WBUF_PAY 4

/* Throughput comparison payloads */
static const uint32_t TPUT_PAYS[] = { 1024, 65536, 262144 };  /* 1 KB, 64 KB, 256 KB */
#define N_TPUT_PAY 3

/* ── Result type communicated child→parent via pipe ─────────────────── */
typedef struct {
    uint32_t recv;
    uint32_t errs;
    uint32_t elapsed_ms;   /* time from first message to STOP received */
    uint32_t total_bytes;  /* payload bytes received (excluding HDR) */
} zc_result_t;

/* ── Server-side connection state ──────────────────────────────────── */
typedef struct {
    shmipc_session_t *sess;
    volatile int      connected;
    zc_result_t       res;          /* filled by srv-side ZC callback */
    uint32_t          expected_pay;
    volatile int      stop_seen;
    volatile int      timing_started;
    double            t_first_ms;
    double            t_stop_ms;
} zc_srv_t;

static void zc_srv_on_conn(shmipc_session_t *s, void *ctx)
    { ((zc_srv_t*)ctx)->sess = s; ((zc_srv_t*)ctx)->connected = 1; }
static void zc_srv_on_disc(shmipc_session_t *s, void *ctx)
    { (void)s; ((zc_srv_t*)ctx)->connected = 0; }

/* ── Client-side receive context ───────────────────────────────────── */
typedef struct {
    volatile uint32_t recv;
    volatile uint32_t errs;
    uint32_t          expected_pay;
    volatile int      stop_seen;
    volatile int      timing_started;
    double            t_first_ms;
    double            t_stop_ms;
} zc_cli_t;

/* ── Byte-level message verifier ────────────────────────────────────── */
/* Returns 0 = data message processed, 1 = STOP seen, -1 = bad frame. */
static int verify_msg(const void *raw, uint32_t len, uint32_t expected_pay,
                      volatile uint32_t *recv, volatile uint32_t *errs,
                      volatile int *stop_seen, volatile int *timing_started,
                      double *t_first, double *t_stop)
{
    const uint8_t *data = (const uint8_t *)raw;
    if (len < HDR_SZ) { (*errs)++; return -1; }

    uint32_t seq; memcpy(&seq, data, 4);
    if (seq == STOP_SEQ) { *t_stop = now_ms(); *stop_seen = 1; return 1; }

    if (!*timing_started) { *t_first = now_ms(); *timing_started = 1; }

    uint32_t pay_len; memcpy(&pay_len, data + 4, 4);
    if (len != HDR_SZ + pay_len || pay_len != expected_pay) {
        (*errs)++; (*recv)++; return -1;
    }

    const uint8_t *payload = data + HDR_SZ;
    for (uint32_t i = 0; i < pay_len; i++) {
        if (payload[i] != (uint8_t)((seq * 31u + i) % 251u)) {
            (*errs)++;
            break;  /* count one error per message, still count recv */
        }
    }
    (*recv)++;
    return 0;
}

/* ── on_data_zc callbacks ───────────────────────────────────────────── */

static void zc_cli_cb(shmipc_client_t *c, shmipc_buf_t *buf, void *ctx)
{
    (void)c;
    zc_cli_t *z = (zc_cli_t *)ctx;
    verify_msg(shmipc_buf_data(buf), shmipc_buf_len(buf),
               z->expected_pay,
               &z->recv, &z->errs, &z->stop_seen,
               &z->timing_started, &z->t_first_ms, &z->t_stop_ms);
    shmipc_buf_release(buf);  /* MUST be called exactly once */
}

static void zc_srv_cb(shmipc_session_t *s, shmipc_buf_t *buf, void *ctx)
{
    (void)s;
    zc_srv_t *z = (zc_srv_t *)ctx;
    verify_msg(shmipc_buf_data(buf), shmipc_buf_len(buf),
               z->expected_pay,
               &z->res.recv, &z->res.errs, &z->stop_seen,
               &z->timing_started, &z->t_first_ms, &z->t_stop_ms);
    shmipc_buf_release(buf);
}

/* Build wire message in tmp[], copy into SHM via alloc_buf (multi-slice when needed). */
static int send_client_wbuf_zc(shmipc_client_t *cli, uint8_t *tmp, uint32_t seq, uint32_t plen)
{
    uint32_t total = HDR_SZ + plen;
    fill_msg(tmp, seq, plen);
    shmipc_wbuf_t *wb = shmipc_client_alloc_buf(cli, total);
    if (!wb)
        return -1;
    uint32_t ns  = shmipc_wbuf_num_slices(wb);
    uint32_t off = 0;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t  nb  = shmipc_wbuf_slice_bytes(wb, i);
        uint8_t * dst = (uint8_t *)shmipc_wbuf_slice_data(wb, i);
        if (!dst || nb == 0 || off + nb > total) {
            shmipc_client_discard_buf(cli, wb);
            return -1;
        }
        memcpy(dst, tmp + off, nb);
        off += nb;
    }
    if (off != total) {
        shmipc_client_discard_buf(cli, wb);
        return -1;
    }
    return shmipc_client_send_buf(cli, wb, total);
}

static int send_session_wbuf_zc(shmipc_session_t *sess, uint8_t *tmp, uint32_t seq, uint32_t plen)
{
    uint32_t total = HDR_SZ + plen;
    fill_msg(tmp, seq, plen);
    shmipc_wbuf_t *wb = shmipc_session_alloc_buf(sess, total);
    if (!wb)
        return -1;
    uint32_t ns  = shmipc_wbuf_num_slices(wb);
    uint32_t off = 0;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t  nb  = shmipc_wbuf_slice_bytes(wb, i);
        uint8_t * dst = (uint8_t *)shmipc_wbuf_slice_data(wb, i);
        if (!dst || nb == 0 || off + nb > total) {
            shmipc_session_discard_buf(sess, wb);
            return -1;
        }
        memcpy(dst, tmp + off, nb);
        off += nb;
    }
    if (off != total) {
        shmipc_session_discard_buf(sess, wb);
        return -1;
    }
    return shmipc_session_send_buf(sess, wb, total);
}

/* on_data (copying) callback for throughput comparison */
static void copy_cli_cb(shmipc_session_t *s, const void *data, uint32_t len, void *ctx)
{
    (void)s;
    zc_cli_t *z = (zc_cli_t *)ctx;
    verify_msg(data, len,
               z->expected_pay,
               &z->recv, &z->errs, &z->stop_seen,
               &z->timing_started, &z->t_first_ms, &z->t_stop_ms);
    /* no release needed — library already freed the slice */
}

/* ── Integrity table helpers ────────────────────────────────────────── */
static void print_integ_header(const char *dir) {
    tprintf("\n┌──────────────────────────────────────────────────────────────────────────────┐\n");
    tprintf("│  [ZC %s Integrity]  LOW_FREQ  shm=8MB  queue=32  slice=4KB              │\n", dir);
    tprintf("│  Payload ≤ 4096 B → borrowed SHM pointer   > 4096 B → heap-copy fallback │\n");
    tprintf("├────────────┬────────────────────────────┬──────────┬───────┬───────────────┤\n");
    tprintf("│  Payload   │  Path                      │ recv/tot │ errs  │    result     │\n");
    tprintf("├────────────┼────────────────────────────┼──────────┼───────┼───────────────┤\n");
}

static void print_integ_row(uint32_t plen, uint32_t recv, uint32_t errs, uint32_t total)
{
    const char *path = (plen <= ZC_SLICE)
                       ? "zero-copy (borrowed SHM) "
                       : "heap-copy (multi-slice)  ";
    const char *verdict = (errs == 0 && recv == total)
                          ? "    PASS     "
                          : "    FAIL     ";
    tprintf("│ %8s   │ %s │ %3u/%-3u  │  %3u  │  %s │\n",
           fmtsz(plen), path, recv, total, errs, verdict);
}

static void print_integ_footer(void) {
    tprintf("└────────────┴────────────────────────────┴──────────┴───────┴───────────────┘\n");
}

/* ── Throughput table helpers ───────────────────────────────────────── */
static void print_tput_header(void) {
    tprintf("\n┌──────────────────────────────────────────────────────────────────────────────┐\n");
    tprintf("│  [ZC vs COPY Throughput]  S→C  LOW_FREQ  BLOCK mode                         │\n");
    tprintf("├──────────┬─────────────────────────────┬───────────┬──────────┬────────────┤\n");
    tprintf("│ Payload  │  Recv method                │  elapsed  │  MB/s    │ recv/total │\n");
    tprintf("├──────────┼─────────────────────────────┼───────────┼──────────┼────────────┤\n");
}

static void print_tput_row(uint32_t plen, const char *method,
                            uint32_t elapsed_ms, double mbps,
                            uint32_t recv, uint32_t total)
{
    tprintf("│ %8s │ %-27s │ %7u ms │ %8.2f │ %4u/%-4u  │\n",
           fmtsz(plen), method, elapsed_ms, mbps, recv, total);
}

static void print_tput_sep(void) {
    tprintf("├──────────┼─────────────────────────────┼───────────┼──────────┼────────────┤\n");
}

static void print_tput_footer(void) {
    tprintf("└──────────┴─────────────────────────────┴───────────┴──────────┴────────────┘\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Section 1: [ZC-S2C Integrity]
 *  Parent = server (sends), child = client (receives with on_data_zc)
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_server_integ_s2c(int s2c_wr, int c2s_rd)
{
    zc_srv_t ctx = { NULL, 0, {0,0,0,0}, 0, 0, 0, 0.0, 0.0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, zc_srv_on_conn);
    shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);

    shmipc_config_t cfg = { ZC_SHM, ZC_CAP, ZC_SLICE };
    /* server doesn't call set_config; it accepts client's negotiated params */
    (void)cfg;

    if (shmipc_server_start(srv, "t4_s2c") != SHMIPC_OK) {
        fprintf(stderr, "[srv-s2c] start failed\n");
        shmipc_server_destroy(srv); return;
    }

    char ch;
    pipe_rd(c2s_rd, &ch, 1);   /* wait for child READY */
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) { fprintf(stderr, "[srv-s2c] no connect\n"); goto done; }

    print_integ_header("S→C");

    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + ZC_PAYS[N_ZC_PAY - 1]);
    if (!buf) { perror("malloc"); goto done; }

    for (int pi = 0; pi < N_ZC_PAY; pi++) {
        uint32_t plen  = ZC_PAYS[pi];
        uint32_t total = HDR_SZ + plen;

        ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
        pipe_wr(s2c_wr, &plen, sizeof(plen));   /* tell client expected payload */
        pipe_rd(c2s_rd, &ch, 1);    /* ACK */

        for (int mi = 0; mi < ZC_MSGS; mi++) {
            fill_msg(buf, (uint32_t)mi, plen);
            shmipc_session_write(ctx.sess, buf, total, SHMIPC_TIMEOUT_INFINITE);
        }
        fill_stop(buf);
        shmipc_session_write(ctx.sess, buf, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);

        zc_result_t res;
        pipe_rd(c2s_rd, &res, sizeof(res));
        print_integ_row(plen, res.recv, res.errs, ZC_MSGS);
    }

    free(buf);
    print_integ_footer();

    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static void run_client_integ_s2c(int s2c_rd, int c2s_wr)
{
    zc_cli_t ctx = { 0, 0, 0, 0, 0, 0.0, 0.0 };

    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_set_config(cli, &(shmipc_config_t){ ZC_SHM, ZC_CAP, ZC_SLICE });
    shmipc_client_register_on_data_zc(cli, zc_cli_cb);

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);  /* signal READY */

    for (int retry = 0; retry < 50; retry++) {
        if (shmipc_client_connect(cli, "t4_s2c") == SHMIPC_OK) break;
        usleep(100000);
    }

    char cmd;
    for (;;) {
        pipe_rd(s2c_rd, &cmd, 1);
        if (cmd == 'X') break;
        /* cmd == 'G' — read expected payload size */
        uint32_t plen;
        pipe_rd(s2c_rd, &plen, sizeof(plen));

        /* Reset context for this batch */
        ctx.recv = 0; ctx.errs = 0; ctx.stop_seen = 0;
        ctx.timing_started = 0;
        ctx.expected_pay = plen;

        ch = 'A'; pipe_wr(c2s_wr, &ch, 1);  /* ACK */

        /* Wait for STOP sentinel from server */
        int ticks = 0;
        while (!ctx.stop_seen && ticks < 100000) { usleep(100); ticks++; }

        zc_result_t res;
        res.recv = ctx.recv; res.errs = ctx.errs;
        res.elapsed_ms = 0; res.total_bytes = 0;
        pipe_wr(c2s_wr, &res, sizeof(res));
    }

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Section 2: [ZC-C2S Integrity]
 *  Parent = server (receives with on_data_zc), child = client (sends)
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_server_integ_c2s(int s2c_wr, int c2s_rd)
{
    zc_srv_t ctx = { NULL, 0, {0,0,0,0}, 0, 0, 0, 0.0, 0.0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, zc_srv_on_conn);
    shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);
    shmipc_server_register_on_data_zc     (srv, zc_srv_cb);

    if (shmipc_server_start(srv, "t4_c2s") != SHMIPC_OK) {
        fprintf(stderr, "[srv-c2s] start failed\n");
        shmipc_server_destroy(srv); return;
    }

    char ch;
    pipe_rd(c2s_rd, &ch, 1);   /* READY */
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) { fprintf(stderr, "[srv-c2s] no connect\n"); goto done; }

    print_integ_header("C→S");

    for (int pi = 0; pi < N_ZC_PAY; pi++) {
        uint32_t plen = ZC_PAYS[pi];

        /* Reset server receive context */
        ctx.expected_pay = plen;
        ctx.res.recv = 0; ctx.res.errs = 0;
        ctx.stop_seen = 0; ctx.timing_started = 0;

        ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
        pipe_rd(c2s_rd, &ch, 1);   /* ACK */

        /* Wait for STOP from client */
        int ticks = 0;
        while (!ctx.stop_seen && ticks < 200000) { usleep(100); ticks++; }

        print_integ_row(plen, ctx.res.recv, ctx.res.errs, ZC_MSGS);

        ch = 'N'; pipe_wr(s2c_wr, &ch, 1);  /* next */
        pipe_rd(c2s_rd, &ch, 1);             /* client ACK next */
    }

    print_integ_footer();

    ch = 'X'; pipe_wr(s2c_wr, &ch, 1);
done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static void run_client_integ_c2s(int s2c_rd, int c2s_wr)
{
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_config(cli, &(shmipc_config_t){ ZC_SHM, ZC_CAP, ZC_SLICE });

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);

    for (int retry = 0; retry < 50; retry++) {
        if (shmipc_client_connect(cli, "t4_c2s") == SHMIPC_OK) break;
        usleep(100000);
    }

    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + ZC_PAYS[N_ZC_PAY - 1]);
    if (!buf) { perror("malloc"); goto done; }

    char cmd;
    for (int pi = 0; pi < N_ZC_PAY; pi++) {
        uint32_t plen  = ZC_PAYS[pi];
        uint32_t total = HDR_SZ + plen;

        pipe_rd(s2c_rd, &cmd, 1);   /* 'G' */
        ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

        for (int mi = 0; mi < ZC_MSGS; mi++) {
            fill_msg(buf, (uint32_t)mi, plen);
            shmipc_client_write(cli, buf, total, SHMIPC_TIMEOUT_INFINITE);
        }
        fill_stop(buf);
        shmipc_client_write(cli, buf, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);

        pipe_rd(s2c_rd, &cmd, 1);   /* 'N' = next */
        ch = 'K'; pipe_wr(c2s_wr, &ch, 1);
    }

    pipe_rd(s2c_rd, &cmd, 1);   /* 'X' */
    free(buf);
done:
    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Section 3: [ZC vs COPY Throughput]
 *  Server sends TPUT_MSGS messages; client receives with on_data or on_data_zc.
 *  Elapsed time measured on receive side (child → parent via pipe).
 *
 *  use_zc flag is sent from parent to child as a single byte ('Z' or 'C').
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_server_tput(int s2c_wr, int c2s_rd, uint32_t plen)
{
    zc_srv_t ctx = { NULL, 0, {0,0,0,0}, 0, 0, 0, 0.0, 0.0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected   (srv, zc_srv_on_conn);
    shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);

    if (shmipc_server_start(srv, "t4_tput") != SHMIPC_OK) {
        fprintf(stderr, "[srv-tput] start failed\n");
        shmipc_server_destroy(srv); return;
    }

    char ch;
    pipe_rd(c2s_rd, &ch, 1);   /* READY */
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) { fprintf(stderr, "[srv-tput] no connect\n"); goto done; }

    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + plen);
    if (!buf) { perror("malloc"); goto done; }

    /* Signal 'G' + payload length + msg count so child knows what to expect */
    uint32_t nmsg = (uint32_t)tput_msgs_for(plen);
    ch = 'G'; pipe_wr(s2c_wr, &ch, 1);
    pipe_wr(s2c_wr, &plen, sizeof(plen));
    pipe_wr(s2c_wr, &nmsg,  sizeof(nmsg));
    pipe_rd(c2s_rd, &ch, 1);   /* ACK */

    for (uint32_t mi = 0; mi < nmsg; mi++) {
        fill_msg(buf, (uint32_t)mi, plen);
        shmipc_session_write(ctx.sess, buf, HDR_SZ + plen, SHMIPC_TIMEOUT_INFINITE);
    }
    fill_stop(buf);
    shmipc_session_write(ctx.sess, buf, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);

    free(buf);

    /* Read back result from child */
    zc_result_t res;
    pipe_rd(c2s_rd, &res, sizeof(res));
    /* result is consumed by caller via pipe — server just drains here */

    /* Re-pipe to outer scope: caller (main) reads from c2s_rd directly */

done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

/* Shared client body for throughput test — use_zc selects callback */
static void run_client_tput(int s2c_rd, int c2s_wr, int use_zc)
{
    zc_cli_t ctx = { 0, 0, 0, 0, 0, 0.0, 0.0 };

    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_set_config(cli, &(shmipc_config_t){ ZC_SHM, ZC_CAP, ZC_SLICE });

    if (use_zc)
        shmipc_client_register_on_data_zc(cli, zc_cli_cb);
    else
        shmipc_client_register_on_data(cli, copy_cli_cb);

    char ch = 'R'; pipe_wr(c2s_wr, &ch, 1);

    for (int retry = 0; retry < 50; retry++) {
        if (shmipc_client_connect(cli, "t4_tput") == SHMIPC_OK) break;
        usleep(100000);
    }

    /* Read 'G' + expected plen + nmsg */
    pipe_rd(s2c_rd, &ch, 1);
    uint32_t plen, nmsg;
    pipe_rd(s2c_rd, &plen, sizeof(plen));
    pipe_rd(s2c_rd, &nmsg, sizeof(nmsg));
    ctx.expected_pay = plen;

    ch = 'A'; pipe_wr(c2s_wr, &ch, 1);

    int ticks = 0;
    while (!ctx.stop_seen && ticks < 2000000) { usleep(100); ticks++; }

    uint32_t elapsed_ms = 0;
    if (ctx.timing_started && ctx.t_stop_ms > ctx.t_first_ms)
        elapsed_ms = (uint32_t)(ctx.t_stop_ms - ctx.t_first_ms);

    zc_result_t res;
    res.recv        = ctx.recv;
    res.errs        = ctx.errs;
    res.elapsed_ms  = elapsed_ms;
    res.total_bytes = ctx.recv * plen;
    pipe_wr(c2s_wr, &res, sizeof(res));

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Section 3a/3b: [ZC-WBUF] client/server alloc_buf + send_buf (multi-slice)
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_server_wbuf_c2s(int s2c_wr, int c2s_rd)
{
    zc_srv_t ctx = { NULL, 0, {0, 0, 0, 0}, 0, 0, 0, 0.0, 0.0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected(srv, zc_srv_on_conn);
    shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);
    shmipc_server_register_on_data_zc(srv, zc_srv_cb);

    if (shmipc_server_start(srv, "t4_wbuf_c2s") != SHMIPC_OK) {
        fprintf(stderr, "[t4-wbuf-c2s] server start failed\n");
        shmipc_server_destroy(srv);
        return;
    }

    char ch;
    pipe_rd(c2s_rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) {
        fprintf(stderr, "[t4-wbuf-c2s] no session\n");
        goto done;
    }

    for (int pi = 0; pi < N_WBUF_PAY; pi++) {
        uint32_t plen = WBUF_PAYS[pi];
        ctx.expected_pay   = plen;
        ctx.res.recv       = 0;
        ctx.res.errs       = 0;
        ctx.stop_seen      = 0;
        ctx.timing_started = 0;

        ch = 'G';
        pipe_wr(s2c_wr, &ch, 1);
        pipe_rd(c2s_rd, &ch, 1);

        int ticks = 0;
        while (!ctx.stop_seen && ticks < 300000) {
            usleep(100);
            ticks++;
        }

        int ok = (ctx.res.errs == 0 && ctx.res.recv == (uint32_t)WBUF_MSGS);
        ch     = ok ? 'P' : 'F';
        pipe_wr(s2c_wr, &ch, 1);
        pipe_rd(c2s_rd, &ch, 1);
    }

    ch = 'X';
    pipe_wr(s2c_wr, &ch, 1);
done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static int run_client_wbuf_c2s(int s2c_rd, int c2s_wr)
{
    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_config(cli, &(shmipc_config_t){ ZC_SHM, ZC_CAP, ZC_SLICE });

    char ch = 'R';
    pipe_wr(c2s_wr, &ch, 1);

    for (int retry = 0; retry < 50; retry++) {
        if (shmipc_client_connect(cli, "t4_wbuf_c2s") == SHMIPC_OK)
            break;
        usleep(100000);
    }

    uint8_t *tmp = (uint8_t *)malloc(HDR_SZ + WBUF_PAYS[N_WBUF_PAY - 1]);
    if (!tmp) {
        perror("malloc");
        shmipc_client_destroy(cli);
        return 1;
    }

    int failures = 0;

    for (int pi = 0; pi < N_WBUF_PAY; pi++) {
        uint32_t plen = WBUF_PAYS[pi];

        pipe_rd(s2c_rd, &ch, 1);
        ch = 'A';
        pipe_wr(c2s_wr, &ch, 1);

        for (int mi = 0; mi < WBUF_MSGS; mi++) {
            if (send_client_wbuf_zc(cli, tmp, (uint32_t)mi, plen) != SHMIPC_OK) {
                failures++;
                break;
            }
        }
        fill_stop(tmp);
        shmipc_client_write(cli, tmp, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);

        pipe_rd(s2c_rd, &ch, 1);
        pipe_wr(c2s_wr, &ch, 1);
        if (ch == 'F')
            failures++;
    }

    pipe_rd(s2c_rd, &ch, 1);

    shmipc_wbuf_t *wb = shmipc_client_alloc_buf(cli, 64);
    if (wb)
        shmipc_client_discard_buf(cli, wb);

    free(tmp);
    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
    return failures;
}

static void run_server_wbuf_s2c(int s2c_wr, int c2s_rd)
{
    zc_srv_t ctx = { NULL, 0, {0, 0, 0, 0}, 0, 0, 0, 0.0, 0.0 };

    shmipc_server_t *srv = shmipc_server_create();
    shmipc_server_set_context(srv, &ctx);
    shmipc_server_register_on_connected(srv, zc_srv_on_conn);
    shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);

    if (shmipc_server_start(srv, "t4_wbuf_s2c") != SHMIPC_OK) {
        fprintf(stderr, "[t4-wbuf-s2c] server start failed\n");
        shmipc_server_destroy(srv);
        return;
    }

    char ch;
    pipe_rd(c2s_rd, &ch, 1);
    wait_flag(&ctx.connected, 10);
    if (!ctx.connected) {
        fprintf(stderr, "[t4-wbuf-s2c] no session\n");
        goto done;
    }

    uint8_t *tmp = (uint8_t *)malloc(HDR_SZ + WBUF_PAYS[N_WBUF_PAY - 1]);
    if (!tmp) {
        perror("malloc");
        goto done;
    }

    for (int pi = 0; pi < N_WBUF_PAY; pi++) {
        uint32_t plen = WBUF_PAYS[pi];

        ch = 'G';
        pipe_wr(s2c_wr, &ch, 1);
        pipe_wr(s2c_wr, &plen, sizeof(plen));
        pipe_rd(c2s_rd, &ch, 1);

        for (int mi = 0; mi < WBUF_MSGS; mi++) {
            if (send_session_wbuf_zc(ctx.sess, tmp, (uint32_t)mi, plen) != SHMIPC_OK)
                break;
        }
        fill_stop(tmp);
        shmipc_session_write(ctx.sess, tmp, HDR_SZ, SHMIPC_TIMEOUT_INFINITE);

        zc_result_t res;
        pipe_rd(c2s_rd, &res, sizeof(res));
        int ok = (res.errs == 0 && res.recv == (uint32_t)WBUF_MSGS);
        ch     = ok ? 'P' : 'F';
        pipe_wr(s2c_wr, &ch, 1);
        pipe_rd(c2s_rd, &ch, 1);
    }

    free(tmp);
    ch = 'X';
    pipe_wr(s2c_wr, &ch, 1);
done:
    shmipc_server_stop(srv);
    shmipc_server_destroy(srv);
}

static int run_client_wbuf_s2c(int s2c_rd, int c2s_wr)
{
    zc_cli_t ctx = { 0, 0, 0, 0, 0, 0.0, 0.0 };

    shmipc_client_t *cli = shmipc_client_create();
    shmipc_client_set_context(cli, &ctx);
    shmipc_client_set_config(cli, &(shmipc_config_t){ ZC_SHM, ZC_CAP, ZC_SLICE });
    shmipc_client_register_on_data_zc(cli, zc_cli_cb);

    char ch = 'R';
    pipe_wr(c2s_wr, &ch, 1);

    for (int retry = 0; retry < 50; retry++) {
        if (shmipc_client_connect(cli, "t4_wbuf_s2c") == SHMIPC_OK)
            break;
        usleep(100000);
    }

    int failures = 0;

    for (;;) {
        pipe_rd(s2c_rd, &ch, 1);
        if (ch == 'X')
            break;
        uint32_t plen;
        pipe_rd(s2c_rd, &plen, sizeof(plen));
        ctx.recv = ctx.errs = 0;
        ctx.stop_seen        = 0;
        ctx.timing_started   = 0;
        ctx.expected_pay     = plen;
        ch                   = 'A';
        pipe_wr(c2s_wr, &ch, 1);

        int ticks = 0;
        while (!ctx.stop_seen && ticks < 300000) {
            usleep(100);
            ticks++;
        }

        zc_result_t res;
        res.recv = ctx.recv;
        res.errs = ctx.errs;
        pipe_wr(c2s_wr, &res, sizeof(res));

        pipe_rd(s2c_rd, &ch, 1);
        pipe_wr(c2s_wr, &ch, 1);
        if (ch == 'F')
            failures++;
    }

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
    return failures;
}

/* ── Main ───────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    /* Save the real stdout on fd 3, then redirect both stdout (LOGI/LOGD
     * write there) and stderr (LOGW/LOGE) to /dev/null so library noise
     * doesn't interleave with test tables.
     * All printf() calls below use the saved fd via out_fp. */
    int saved_stdout = dup(STDOUT_FILENO);
    out_fp = fdopen(saved_stdout, "w");
    setvbuf(out_fp, NULL, _IONBF, 0);    /* unbuffered: no double-write after fork */

    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);

    int wbuf_fail = 0;

    /* ── Section 1: ZC-S2C Integrity ──────────────────────────────── */
    {
        int p1[2], p2[2];
        pipe(p1); pipe(p2);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(devnull, STDOUT_FILENO);
            close(p1[1]); close(p2[0]);
            run_client_integ_s2c(p1[0], p2[1]);
            exit(0);
        }
        close(p1[0]); close(p2[1]);
        run_server_integ_s2c(p1[1], p2[0]);
        waitpid(pid, NULL, 0);
        close(p1[1]); close(p2[0]);
    }

    /* ── Section 2: ZC-C2S Integrity ──────────────────────────────── */
    {
        int p1[2], p2[2];
        pipe(p1); pipe(p2);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(devnull, STDOUT_FILENO);
            close(p1[1]); close(p2[0]);
            run_client_integ_c2s(p1[0], p2[1]);
            exit(0);
        }
        close(p1[0]); close(p2[1]);
        run_server_integ_c2s(p1[1], p2[0]);
        waitpid(pid, NULL, 0);
        close(p1[1]); close(p2[0]);
    }

    /* ── Section 3: write alloc_buf / send_buf (C2S + S2C) ─────────── */
    {
        int fc2s = 0, fs2c = 0;
        {
            int p1[2], p2[2];
            if (pipe(p1) != 0 || pipe(p2) != 0)
                perror("pipe");
            else {
                pid_t pid = fork();
                if (pid == 0) {
                    dup2(devnull, STDOUT_FILENO);
                    close(p1[1]);
                    close(p2[0]);
                    int f = run_client_wbuf_c2s(p1[0], p2[1]);
                    close(p1[0]);
                    close(p2[1]);
                    exit(f);
                }
                close(p1[0]);
                close(p2[1]);
                run_server_wbuf_c2s(p1[1], p2[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                if (WIFEXITED(st))
                    fc2s = WEXITSTATUS(st);
                else
                    fc2s = 1;
                close(p1[1]);
                close(p2[0]);
            }
        }
        {
            int p1[2], p2[2];
            if (pipe(p1) != 0 || pipe(p2) != 0)
                perror("pipe");
            else {
                pid_t pid = fork();
                if (pid == 0) {
                    dup2(devnull, STDOUT_FILENO);
                    close(p1[1]);
                    close(p2[0]);
                    int f = run_client_wbuf_s2c(p1[0], p2[1]);
                    close(p1[0]);
                    close(p2[1]);
                    exit(f);
                }
                close(p1[0]);
                close(p2[1]);
                run_server_wbuf_s2c(p1[1], p2[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                if (WIFEXITED(st))
                    fs2c = WEXITSTATUS(st);
                else
                    fs2c = 1;
                close(p1[1]);
                close(p2[0]);
            }
        }
        wbuf_fail = fc2s + fs2c;
        tprintf("\n[ZC write alloc_buf] C2S: %s  S2C: %s\n",
                fc2s ? "FAIL" : "OK", fs2c ? "FAIL" : "OK");
    }

    /* ── Section 4: ZC vs COPY Throughput ─────────────────────────── */
    print_tput_header();

    for (int pi = 0; pi < N_TPUT_PAY; pi++) {
        uint32_t plen = TPUT_PAYS[pi];

        if (pi > 0) print_tput_sep();

        /* Run twice: first with on_data (copy), then on_data_zc */
        for (int use_zc = 0; use_zc <= 1; use_zc++) {
            int p1[2], p2[2];
            pipe(p1); pipe(p2);
            pid_t pid = fork();
            if (pid == 0) {
                dup2(devnull, STDOUT_FILENO);
                close(p1[1]); close(p2[0]);
                run_client_tput(p1[0], p2[1], use_zc);
                exit(0);
            }
            close(p1[0]); close(p2[1]);

            /* Run server inline in parent */
            {
                zc_srv_t ctx = { NULL, 0, {0,0,0,0}, 0, 0, 0, 0.0, 0.0 };
                shmipc_server_t *srv = shmipc_server_create();
                shmipc_server_set_context(srv, &ctx);
                shmipc_server_register_on_connected   (srv, zc_srv_on_conn);
                shmipc_server_register_on_disconnected(srv, zc_srv_on_disc);

                if (shmipc_server_start(srv, "t4_tput") == SHMIPC_OK) {
                    char ch;
                    pipe_rd(p2[0], &ch, 1);          /* READY */
                    wait_flag(&ctx.connected, 10);

                    uint32_t nmsg = (uint32_t)tput_msgs_for(plen);
                    uint8_t *buf = (uint8_t *)malloc(HDR_SZ + plen);
                    if (buf && ctx.connected) {
                        ch = 'G'; pipe_wr(p1[1], &ch, 1);
                        pipe_wr(p1[1], &plen, sizeof(plen));
                        pipe_wr(p1[1], &nmsg,  sizeof(nmsg));
                        pipe_rd(p2[0], &ch, 1);      /* ACK */

                        for (uint32_t mi = 0; mi < nmsg; mi++) {
                            fill_msg(buf, mi, plen);
                            shmipc_session_write(ctx.sess, buf,
                                                 HDR_SZ + plen,
                                                 SHMIPC_TIMEOUT_INFINITE);
                        }
                        fill_stop(buf);
                        shmipc_session_write(ctx.sess, buf, HDR_SZ,
                                             SHMIPC_TIMEOUT_INFINITE);
                        free(buf);
                    }
                    shmipc_server_stop(srv);
                }
                shmipc_server_destroy(srv);
            }

            /* Read result from child */
            zc_result_t res;
            pipe_rd(p2[0], &res, sizeof(res));
            waitpid(pid, NULL, 0);
            close(p1[1]); close(p2[0]);

            double mbps = 0.0;
            if (res.elapsed_ms > 0)
                mbps = (double)res.total_bytes / res.elapsed_ms / 1024.0 / 1024.0 * 1000.0;

            const char *method = use_zc
                ? "on_data_zc (zero-copy recv)"
                : "on_data    (copy recv)     ";

            uint32_t total_msgs = (uint32_t)tput_msgs_for(plen);
            print_tput_row(plen, method, res.elapsed_ms, mbps, res.recv, total_msgs);
        }
    }
    print_tput_footer();

    close(devnull);
    tprintf("\ndone.\n");
    return wbuf_fail ? 1 : 0;
}
