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

/* ---------------------------------------------------------------
 *  Event queue  (fixed upper bound, runtime capacity via field)
 * --------------------------------------------------------------- */
struct ShmBufferEvent {
    uint32_t slice_index;
    uint32_t length;
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
 *  API
 * --------------------------------------------------------------- */
uint32_t alloc_slice(ShmBufferList* list);
void     free_slice (ShmBufferList* list, uint32_t index);

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
