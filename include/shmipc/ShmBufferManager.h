#ifndef SHMIPC_SHMBUFFERMANAGER_H
#define SHMIPC_SHMBUFFERMANAGER_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

#include "ShmConfig.h"

/* ---------------------------------------------------------------
 *  Futex helpers for cross-process shared-memory notification.
 *  SYS_futex is available on Linux and Android (Bionic).
 *
 *  We only use FUTEX_WAIT / FUTEX_WAKE (no process-private flag),
 *  which work across processes sharing the same physical page.
 * --------------------------------------------------------------- */
#ifndef FUTEX_WAIT
#  define FUTEX_WAIT 0
#  define FUTEX_WAKE 1
#endif

/* Wake up to `n` threads sleeping on `addr`. */
inline void shm_futex_wake(std::atomic<uint32_t>* addr, int n = 1)
{
    syscall(SYS_futex,
            reinterpret_cast<uint32_t*>(addr),
            FUTEX_WAKE, n, nullptr, nullptr, 0);
}

/* Sleep while *addr == val.
 * timeout_ms == 0  → wait indefinitely.
 * Returns true if woken by FUTEX_WAKE, false on timeout / EINTR. */
inline bool shm_futex_wait(std::atomic<uint32_t>* addr,
                            uint32_t               val,
                            uint32_t               timeout_ms = 0)
{
    struct timespec ts, *pts = nullptr;
    if (timeout_ms > 0) {
        ts.tv_sec  = timeout_ms / 1000u;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000u) * 1000000L;
        pts = &ts;
    }
    long rc = syscall(SYS_futex,
                      reinterpret_cast<uint32_t*>(addr),
                      FUTEX_WAIT, val, pts, nullptr, 0);
    return rc == 0;
}

/* Ensure std::atomic<uint32_t> uses CPU-native instructions (no process-local
 * lock tables), which is required for safe cross-process shared memory access.
 * ATOMIC_INT_LOCK_FREE == 2 means always lock-free (C++11 macro). */
static_assert(ATOMIC_INT_LOCK_FREE == 2,
    "std::atomic<uint32_t> must be lock-free for cross-process shared memory safety");

#define INVALID_INDEX    0xFFFFFFFFu
#define SLICE_HEADER_SZ  8u   /* sizeof(next) + sizeof(length) */

constexpr uint32_t WORKING_FLAG = 1u;

/* Monotonic nanosecond clock — backed by vDSO on Linux (~10-20 ns/call). */
inline uint64_t shm_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

/* ---------------------------------------------------------------
 *  Event queue  (fixed upper bound, runtime capacity via field)
 * --------------------------------------------------------------- */
struct ShmBufferEvent {
    uint32_t slice_index;
    uint32_t length;
    uint64_t write_ts_ns;  /* CLOCK_MONOTONIC ns, set by producer in tryWriteOnce */
};

struct ShmBufferEventQueue {
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    uint32_t              capacity;       /* runtime slot count (≤ SHMIPC_MAX_EVENT_QUEUE_SIZE) */
    std::atomic<uint32_t> workingFlags;
    ShmBufferEvent        events[SHMIPC_MAX_EVENT_QUEUE_SIZE];
};

/* ---------------------------------------------------------------
 *  Slice  — header only; data bytes follow immediately in memory
 * --------------------------------------------------------------- */
struct ShmBufferSlice {
    uint32_t next;    /* index of next slice in chain, or INVALID_INDEX */
    uint32_t length;  /* number of valid bytes in this slice's data area */
    /* uint8_t data[slice_size]  -- accessed via get_slice_data() */
};

/* ---------------------------------------------------------------
 *  Buffer list  — flat region of variable-size slices
 * --------------------------------------------------------------- */
struct ShmBufferList {
    uint32_t              slice_count;  /* total number of slices */
    uint32_t              slice_size;   /* data bytes per slice (NOT including header) */
    uint32_t              stride;       /* bytes per slice record = SLICE_HEADER_SZ + slice_size */
    std::atomic<uint32_t> free_head;
    /* Slice records follow immediately after this struct in memory */
};

/* ---------------------------------------------------------------
 *  Region layout
 *
 *  [ShmBufferEventQueue]
 *  [ShmBufferList header]
 *  [slice 0 : ShmBufferSlice header (8B) + data (slice_size B)]
 *  [slice 1 : ...]
 *  ...
 * --------------------------------------------------------------- */
struct ShmBufferManager {
    ShmBufferEventQueue io_queue;
    ShmBufferList       buffer_list;
};

/* ---------------------------------------------------------------
 *  Slice access helpers
 * --------------------------------------------------------------- */
inline ShmBufferSlice* get_slice(ShmBufferList* list, uint32_t idx) {
    return reinterpret_cast<ShmBufferSlice*>(
        reinterpret_cast<char*>(list + 1) + static_cast<size_t>(idx) * list->stride);
}

inline uint8_t* get_slice_data(ShmBufferSlice* slice) {
    return reinterpret_cast<uint8_t*>(slice) + SLICE_HEADER_SZ;
}

/* ---------------------------------------------------------------
 *  Receive-buffer handle for the zero-copy API
 *
 *  Heap-allocated by the consumer thread; the application receives
 *  a pointer and MUST call shmipc_buf_release() exactly once.
 *
 *  borrowed == true
 *    data points directly into the shared-memory slice.
 *    release() walks slice_head chain and calls free_slice(),
 *    then deletes the struct.
 *
 *  borrowed == false  (multi-slice fallback: data was fragmented)
 *    data points to a heap copy allocated with new uint8_t[].
 *    release() calls delete[] data, then deletes the struct.
 * --------------------------------------------------------------- */
struct shmipc_buf {
    const uint8_t* data;
    uint32_t       len;
    bool           borrowed;
    ShmBufferList* list;        /* valid when borrowed == true  */
    uint32_t       slice_head;  /* first slice index in chain   */
};
typedef struct shmipc_buf shmipc_buf_t;

/* ---------------------------------------------------------------
 *  Write-side zero-copy handle
 *
 *  Returned by shmipc_session_alloc_buf / shmipc_client_alloc_buf.
 *  The caller writes directly into `data` (up to `capacity` bytes)
 *  then calls *_send_buf(buf, actual_len) to enqueue it without
 *  any additional memcpy.
 *
 *  Ownership: *_send_buf and *_discard_buf always consume the handle.
 * --------------------------------------------------------------- */
struct shmipc_wbuf {
    uint8_t*          data;       /* writable pointer into the SHM slice        */
    uint32_t          capacity;   /* usable bytes (== slice_size)               */
    uint32_t          slice_idx;  /* pre-allocated slice index in the SHM list  */
    ShmBufferManager* manager;    /* owning buffer manager (server or client)   */
};
typedef struct shmipc_wbuf shmipc_wbuf_t;

/* ---------------------------------------------------------------
 *  API
 * --------------------------------------------------------------- */
uint32_t alloc_slice(ShmBufferList* list);
void     free_slice (ShmBufferList* list, uint32_t index);

/* Commit a pre-allocated slice as a new SHM event.
 * MUST be called with the caller's write-mutex held.
 * Returns false (queue full) — caller is responsible for freeing the
 * slice and deleting the wbuf in that case. */
inline bool shm_queue_commit(ShmBufferEventQueue* queue,
                              uint32_t slice_idx, uint32_t len) {
    uint32_t cap  = queue->capacity;
    uint32_t tail = queue->tail.load(std::memory_order_acquire);
    uint32_t head = queue->head.load(std::memory_order_acquire);
    uint32_t next = (tail + 1) % cap;
    if (next == head) return false;
    queue->events[tail].slice_index = slice_idx;
    queue->events[tail].length      = len;
    queue->events[tail].write_ts_ns = shm_now_ns();
    queue->tail.store(next, std::memory_order_release);
    return true;
}

/* Release a receive-side zero-copy buffer (frees slice or heap copy). */
inline void shmipc_buf_free(shmipc_buf_t* buf) {
    if (!buf) return;
    if (buf->borrowed) {
        uint32_t idx = buf->slice_head;
        while (idx != INVALID_INDEX) {
            uint32_t next = get_slice(buf->list, idx)->next;
            free_slice(buf->list, idx);
            idx = next;
        }
    } else {
        delete[] buf->data;
    }
    delete buf;
}

/* Decode one SHM event into a heap-allocated shmipc_buf_t.
 * If want_borrow==true AND the message fits in one slice, the returned
 * buf borrows the SHM slice directly (zero-copy receive).
 * Otherwise, data is copied to heap and the slices are freed immediately. */
inline shmipc_buf_t* shmipc_buf_decode(ShmBufferList* list,
                                        uint32_t slice_idx,
                                        uint32_t total_len,
                                        bool want_borrow) {
    auto* buf = new shmipc_buf_t;
    buf->len  = total_len;

    ShmBufferSlice* first = get_slice(list, slice_idx);
    if (want_borrow && first->next == INVALID_INDEX) {
        /* Single-slice: lend the SHM pointer directly. */
        buf->data       = get_slice_data(first);
        buf->borrowed   = true;
        buf->list       = list;
        buf->slice_head = slice_idx;
    } else {
        /* Multi-slice or non-ZC: copy to heap, free slices now. */
        auto* owned    = new uint8_t[total_len];
        uint32_t rem   = total_len, off = 0, idx = slice_idx;
        while (idx != INVALID_INDEX && rem > 0) {
            ShmBufferSlice* s    = get_slice(list, idx);
            uint32_t        copy = std::min(rem, s->length);
            memcpy(owned + off, get_slice_data(s), copy);
            rem -= copy; off += copy; idx = s->next;
        }
        idx = slice_idx;
        while (idx != INVALID_INDEX) {
            uint32_t next = get_slice(list, idx)->next;
            free_slice(list, idx); idx = next;
        }
        buf->data       = owned;
        buf->borrowed   = false;
        buf->list       = nullptr;
        buf->slice_head = INVALID_INDEX;
    }
    return buf;
}

/* Initialize a fresh region (memset + build free list).
 * event_queue_capacity : runtime queue slots (≤ SHMIPC_MAX_EVENT_QUEUE_SIZE)
 * slice_size           : data bytes per slice */
ShmBufferManager* init_shm_buffer_manager(void*    addr,
                                           size_t   region_size,
                                           uint32_t event_queue_capacity,
                                           uint32_t slice_size);

/* Attach to an already-initialized region (no memset / re-init). */
ShmBufferManager* attach_shm_buffer_manager(void* addr);

#endif //SHMIPC_SHMBUFFERMANAGER_H
