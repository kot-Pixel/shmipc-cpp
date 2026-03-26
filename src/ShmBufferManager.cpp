#include "shmipc/ShmBufferManager.h"
#include "shmipc/ShmLogger.h"

uint32_t alloc_slice(ShmBufferList* list) {
    uint32_t head = list->free_head.load(std::memory_order_acquire);
    while (head != INVALID_INDEX) {
        uint32_t next = get_slice(list, head)->next;
        if (list->free_head.compare_exchange_weak(head, next,
                                                  std::memory_order_acq_rel)) {
            return head;
        }
    }
    return INVALID_INDEX;
}

void free_slice(ShmBufferList* list, uint32_t idx) {
    uint32_t head = list->free_head.load(std::memory_order_acquire);
    do {
        get_slice(list, idx)->next = head;
    } while (!list->free_head.compare_exchange_weak(head, idx,
                                                    std::memory_order_acq_rel));
}

ShmBufferManager* init_shm_buffer_manager(void*    addr,
                                           size_t   region_size,
                                           uint32_t event_queue_capacity,
                                           uint32_t slice_size) {
    if (!addr) return nullptr;
    if (event_queue_capacity == 0 || event_queue_capacity > SHMIPC_MAX_EVENT_QUEUE_SIZE) {
        LOGE("invalid event_queue_capacity=%u (max=%u)",
             event_queue_capacity, SHMIPC_MAX_EVENT_QUEUE_SIZE);
        return nullptr;
    }
    if (slice_size == 0) {
        LOGE("slice_size must be > 0");
        return nullptr;
    }

    memset(addr, 0, region_size);
    auto* mgr = static_cast<ShmBufferManager*>(addr);

    /* --- event queue --- */
    mgr->io_queue.head.store(0);
    mgr->io_queue.tail.store(0);
    mgr->io_queue.workingFlags.store(0);
    mgr->io_queue.capacity = event_queue_capacity;

    /* --- slice list --- */
    uint32_t stride      = SLICE_HEADER_SZ + slice_size;
    size_t   header_size = sizeof(ShmBufferManager);
    size_t   avail       = (region_size > header_size) ? (region_size - header_size) : 0;
    uint32_t slice_count = static_cast<uint32_t>(avail / stride);

    mgr->buffer_list.slice_count = slice_count;
    mgr->buffer_list.slice_size  = slice_size;
    mgr->buffer_list.stride      = stride;
    mgr->buffer_list.free_head.store(0);

    for (uint32_t i = 0; i < slice_count; ++i) {
        ShmBufferSlice* s = get_slice(&mgr->buffer_list, i);
        s->next   = (i + 1 < slice_count) ? i + 1 : INVALID_INDEX;
        s->length = 0;
    }

    LOGD("init_shm_buffer_manager: queue_cap=%u slice_size=%u stride=%u slices=%u",
         event_queue_capacity, slice_size, stride, slice_count);

    return mgr;
}

ShmBufferManager* attach_shm_buffer_manager(void* addr) {
    if (!addr) return nullptr;
    return static_cast<ShmBufferManager*>(addr);
}
