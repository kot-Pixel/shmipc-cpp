/*
 * test_common.h — shared utilities for shmipc test suite
 *
 * Include this file once in each test .c file.
 * All definitions are static; each translation unit gets its own copy.
 */
#pragma once
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

/* ── Message wire format ────────────────────────────────────────────────
 *   [0..3]  uint32_t  seq      monotone sequence number
 *   [4..7]  uint32_t  pay_len  payload byte count (0 for STOP)
 *   [8..]   uint8_t   payload[]
 * STOP sentinel: seq = STOP_SEQ, pay_len = 0, total len = HDR_SZ
 * ────────────────────────────────────────────────────────────────────── */
#define HDR_SZ    8u
#define STOP_SEQ  0xFFFFFFFFu

/* ── Write modes ──────────────────────────────────────────────────────── */
static const int32_t MODES[]    = {
    SHMIPC_TIMEOUT_INFINITE, SHMIPC_TIMEOUT_NONBLOCKING, 100
};
static const char* MODE_STR[] = { "BLOCK ", "NONBLK", " 100ms" };
#define N_MOD 3

/* ── Per-preset payload arrays ────────────────────────────────────────── */

/* LOW_FREQ: 256 B → 1 MB   (13 steps) */
static const uint32_t PAYS_LF[] = {
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    65536, 131072, 262144, 524288, 1048576
};
#define N_LF  13

/* GENERAL: 256 B → 4 MB   (15 steps) */
static const uint32_t PAYS_GEN[] = {
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    65536, 131072, 262144, 524288, 1048576, 2097152, 4194304
};
#define N_GEN 15

/* HI_THRU: 256 B → 8 MB   (16 steps) */
static const uint32_t PAYS_HI[] = {
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608
};
#define N_HI  16

/* ── Preset struct (used by test1 and test2) ──────────────────────────── */
typedef struct {
    const char     *name;
    const char     *chan;
    uint32_t        shm, cap, slice;
    const uint32_t *pays;
    int             n_pay;
    uint32_t        max_pay;
} preset_t;

/* ── Messages per test — scaled by payload size ───────────────────────── */
static int msgs_for(uint32_t plen) {
    if (plen >= 4u<<20) return 10;
    if (plen >= 2u<<20) return 15;
    if (plen >= 1u<<20) return 20;
    if (plen >= 512<<10) return 30;
    if (plen >= 128<<10) return 50;
    if (plen >= 32<<10)  return 100;
    return 200;
}

/* ── Message fill helpers ─────────────────────────────────────────────── */
static void fill_msg(uint8_t *buf, uint32_t seq, uint32_t pay_len) {
    memcpy(buf,     &seq,     4);
    memcpy(buf + 4, &pay_len, 4);
    for (uint32_t i = 0; i < pay_len; i++)
        buf[HDR_SZ + i] = (uint8_t)((seq * 31u + i) % 251u);
}

static void fill_stop(uint8_t *buf) {
    uint32_t seq = STOP_SEQ, len = 0;
    memcpy(buf, &seq, 4);
    memcpy(buf + 4, &len, 4);
}

/* ── Timing ───────────────────────────────────────────────────────────── */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

/* ── Size formatter — rotating 4-buffer; safe for up to 4 calls/printf ── */
static const char *fmtsz(uint32_t n) {
    static char bufs[4][12];
    static int idx = 0;
    idx = (idx + 1) & 3;
    char *b = bufs[idx];
    if      (n >= 1u<<20) snprintf(b, 12, "%4uMB", n >> 20);
    else if (n >= 1u<<10) snprintf(b, 12, "%4uKB", n >> 10);
    else                  snprintf(b, 12, "%4u B", n);
    return b;
}

/* ── Pipe I/O (handles EINTR) ─────────────────────────────────────────── */
static void pipe_wr(int fd, const void *p, size_t n) {
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r > 0) { p = (const char *)p + r; n -= (size_t)r; }
        else if (errno != EINTR) break;
    }
}

static void pipe_rd(int fd, void *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r > 0) { p = (char *)p + r; n -= (size_t)r; }
        else if (r == 0 || errno != EINTR) break;
    }
}

/* ── Spin-wait for a flag with a second-based timeout ─────────────────── */
static void wait_flag(volatile int *flag, int timeout_s) {
    int steps = timeout_s * 10000; /* 100 µs per step */
    for (int i = 0; !*flag && i < steps; i++)
        usleep(100);
}
