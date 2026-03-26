#include "shmipc/ShmServerSession.h"
#include "shmipc/ShmIpcMessage.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <chrono>
#include <climits>
#include <cstring>
#include <thread>

void ShmServerSession::startRunReadThreadLoop() {
    mReadThreadRunning = true;
    mAlive = true;
    mProcessorThread.reset(new std::thread(&ShmServerSession::messageProcessor, this));
    mReadThread.reset(new std::thread(&ShmServerSession::clientUdsReader, this));
}

void ShmServerSession::stopRunReadThreadLoop() {
    mReadThreadRunning = false;
    mMessageQueue.stop();
    if (mReadThread    && mReadThread->joinable())    mReadThread->join();
    if (mProcessorThread && mProcessorThread->joinable()) mProcessorThread->join();
    stopClientWriteConsumer();
}

void ShmServerSession::cleanupSharedMemory() {
    if (mSharedMemoryAddr && mSharedMemoryAddr != MAP_FAILED) {
        munmap(mSharedMemoryAddr, mSharedMemorySize);
        mSharedMemoryAddr = nullptr;
    }
    if (mSharedMemoryFd >= 0) {
        close(mSharedMemoryFd);
        mSharedMemoryFd = -1;
    }
    mServerWriteBuf = nullptr;
    mClientWriteBuf = nullptr;
}

void ShmServerSession::clientUdsReader() {
    LOGI("ShmServerSession reader start fd=%d", mClientFd);

    uint8_t header[SHM_SERVER_PROTOCOL_HEAD_SIZE];
    std::vector<int> received_fds;

    while (mReadThreadRunning) {
        bool ok = mShmProtocolHandler->receiveProtocolHeader(mClientFd, header, received_fds);
        if (ok) {
            ShmIpcMessage msg;
            msg.header = ShmIpcMessageHeader::deserialize(header);
            uint32_t payloadLen = msg.header.length - SHM_SERVER_PROTOCOL_HEAD_SIZE;
            std::vector<char> payload(payloadLen);
            if (payloadLen > 0) {
                if (!mShmProtocolHandler->receiveProtocolPayload(
                        mClientFd, payload.data(), payloadLen)) {
                    LOGE("UDS payload read failure");
                    break;
                }
            }
            msg.payload = std::move(payload);
            msg.fds     = std::move(received_fds);
            mMessageQueue.push(std::move(msg));
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            LOGI("peer disconnected fd=%d", mClientFd);
            break;
        }
    }

    mReadThreadRunning = false;
    mAlive = false;

    /* Stop the consumer thread before munmapping shared memory. */
    stopClientWriteConsumer();
    cleanupSharedMemory();

    if (mCallbacks && mCallbacks->onDisconnected) mCallbacks->onDisconnected(this);

    close(mClientFd);
    mClientFd = -1;

    mMessageQueue.stop();
    LOGI("ShmServerSession reader stop");
}

void ShmServerSession::messageProcessor() {
    LOGI("ShmServerSession processor start");

    ShmIpcMessage msg;
    while (mMessageQueue.pop(msg)) {
        switch (static_cast<ShmProtocolType>(msg.header.type)) {
            case ShmProtocolType::ExchangeMetadata:   handleExchangeMetaDataMessage(msg); break;
            case ShmProtocolType::ShareMemoryByMemfd: handleShareMemoryByMemfd();          break;
            case ShmProtocolType::AckShareMemory:     handleAckShareMemoryMessage(msg);    break;
            /* SyncEventClientWrite is no longer sent over UDS — notifications
             * now arrive via futex on workingFlags inside shared memory. */
            default: break;
        }
    }

    LOGI("ShmServerSession processor stop");
}

void ShmServerSession::handleExchangeMetaDataMessage(const ShmIpcMessage& message) {
    if (message.payload.size() < sizeof(ShmMetadata)) { LOGE("payload too small"); return; }
    ShmMetadata meta{};
    memcpy(&meta, message.payload.data(), sizeof(ShmMetadata));
    if (!metaDataIsValid(meta)) { LOGE("invalid metadata"); return; }
    exchangeMetaData(meta);
}

void ShmServerSession::handleAckShareMemoryMessage(const ShmIpcMessage& message) {
    LOGI("handleAckShareMemory");
    if (message.fds.empty()) { LOGE("no fd received"); return; }

    int shm_fd = dup(message.fds[0]);
    if (shm_fd < 0) { LOGE("dup failed"); return; }

    struct stat st{};
    if (fstat(shm_fd, &st) < 0) { LOGE("fstat failed"); close(shm_fd); return; }
    size_t size = st.st_size;

    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) { LOGE("mmap failed"); close(shm_fd); return; }

    size_t   half = size / 2;
    /* Attach using the negotiated parameters stored in mMetadata */
    ShmBufferManager* serverWriteBuf = attach_shm_buffer_manager(addr);
    ShmBufferManager* clientWriteBuf = attach_shm_buffer_manager((char*)addr + half);

    LOGD("server_write: cap=%u slices=%u slice_size=%u  "
         "client_write: cap=%u slices=%u slice_size=%u",
         serverWriteBuf->io_queue.capacity, serverWriteBuf->buffer_list.slice_count,
         serverWriteBuf->buffer_list.slice_size,
         clientWriteBuf->io_queue.capacity, clientWriteBuf->buffer_list.slice_count,
         clientWriteBuf->buffer_list.slice_size);

    onSharedMemoryReady(addr, size, shm_fd, serverWriteBuf, clientWriteBuf);
}

void ShmServerSession::onSharedMemoryReady(void* addr, size_t size, int fd,
                                            ShmBufferManager* serverWriteBuf,
                                            ShmBufferManager* clientWriteBuf) {
    mSharedMemoryAddr = addr;
    mSharedMemorySize = size;
    mSharedMemoryFd   = fd;
    mServerWriteBuf   = serverWriteBuf;
    mClientWriteBuf   = clientWriteBuf;

    sendShareMemoryReady();
    LOGI("shared memory ready, sent ShareMemoryReady");

    /* Start async dispatch thread (if requested) before the consumer thread,
     * so the queue is ready when the first message arrives. */
    if (mCallbacks && mCallbacks->asyncDispatchDepth > 0)
        startDispatch();

    /* Start the futex-based consumer thread for the client_write region. */
    mConsumerRunning.store(true, std::memory_order_release);
    mClientWriteConsumerThread.reset(
        new std::thread(&ShmServerSession::clientWriteConsumerThread, this));

    if (mCallbacks && mCallbacks->onConnected)
        mCallbacks->onConnected(this);
}

bool ShmServerSession::tryWriteOnce(const uint8_t* msg, uint32_t len) {
    auto*    list          = &mServerWriteBuf->buffer_list;
    auto*    queue         = &mServerWriteBuf->io_queue;
    uint32_t slice_size    = list->slice_size;
    uint32_t slices_needed = (len + slice_size - 1) / slice_size;
    uint32_t first = INVALID_INDEX, prev = INVALID_INDEX, offset = 0;

    for (uint32_t i = 0; i < slices_needed; ++i) {
        uint32_t idx = alloc_slice(list);
        if (idx == INVALID_INDEX) {
            uint32_t cur = first;
            while (cur != INVALID_INDEX) {
                uint32_t n = get_slice(list, cur)->next;
                free_slice(list, cur);
                cur = n;
            }
            return false;
        }
        ShmBufferSlice* s    = get_slice(list, idx);
        uint8_t*        dest = get_slice_data(s);
        uint32_t        copy = std::min(len - offset, slice_size);
        memcpy(dest, msg + offset, copy);
        s->length = copy;
        offset   += copy;
        if (prev != INVALID_INDEX) get_slice(list, prev)->next = idx; else first = idx;
        prev    = idx;
        s->next = INVALID_INDEX;
    }

    if (first == INVALID_INDEX) return false;

    uint32_t cap       = queue->capacity;
    uint32_t tail      = queue->tail.load(std::memory_order_acquire);
    uint32_t head      = queue->head.load(std::memory_order_acquire);
    uint32_t next_tail = (tail + 1) % cap;

    if (next_tail == head) {
        uint32_t cur = first;
        while (cur != INVALID_INDEX) {
            uint32_t n = get_slice(list, cur)->next;
            free_slice(list, cur);
            cur = n;
        }
        return false;
    }

    queue->events[tail].slice_index  = first;
    queue->events[tail].length       = len;
    queue->events[tail].write_ts_ns  = shm_now_ns();
    queue->tail.store(next_tail, std::memory_order_release);

    uint32_t prev_flags = queue->workingFlags.fetch_or(WORKING_FLAG, std::memory_order_acq_rel);
    if ((prev_flags & WORKING_FLAG) == 0) dataSyncServerWrite();

    return true;
}

int ShmServerSession::writData(const uint8_t* msg, uint32_t len, int32_t timeout_ms) {
    if (!mServerWriteBuf || !msg || len == 0 || !mAlive) return SHMIPC_ERR;

    using clock = std::chrono::steady_clock;
    auto deadline = (timeout_ms > 0)
        ? clock::now() + std::chrono::milliseconds(timeout_ms)
        : clock::time_point::max();

    /* Exponential backoff: 100µs → 200 → 400 → … → 10ms cap.
     * sleep_for() is a real blocking sleep (nanosleep), not a busy-spin,
     * so CPU usage stays negligible even during sustained back-pressure. */
    uint32_t sleep_us = 100;
    while (mAlive.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            if (tryWriteOnce(msg, len)) {
                mBytesSent.fetch_add(len,  std::memory_order_relaxed);
                mMsgsSent .fetch_add(1,    std::memory_order_relaxed);
                return SHMIPC_OK;
            }
        }
        if (timeout_ms < 0)                              return SHMIPC_ERR;
        if (timeout_ms > 0 && clock::now() >= deadline)  return SHMIPC_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        sleep_us = std::min(sleep_us * 2u, 10000u);  /* cap at 10 ms */
    }
    return SHMIPC_ERR;
}

void ShmServerSession::dataSyncServerWrite() {
    /* Replaced UDS sendmsg with a futex wake on the server_write workingFlags.
     * The client's serverWriteConsumerThread sleeps on that same address. */
    if (mServerWriteBuf && mAlive.load(std::memory_order_acquire))
        shm_futex_wake(&mServerWriteBuf->io_queue.workingFlags);
}

void ShmServerSession::clientWriteConsumerThread() {
    LOGI("ShmServerSession client_write consumer start");
    auto* flags = &mClientWriteBuf->io_queue.workingFlags;

    while (mConsumerRunning.load(std::memory_order_acquire)) {
        uint32_t val = flags->load(std::memory_order_acquire);
        if (val == 0) {
            /* Sleep until the client calls shm_futex_wake, or 10 ms elapses
             * (so we can check mConsumerRunning and exit gracefully). */
            shm_futex_wait(flags, 0, 10);
            continue;
        }
        readFromClientWriteBuffer();
    }

    LOGI("ShmServerSession client_write consumer stop");
}

void ShmServerSession::stopClientWriteConsumer() {
    mConsumerRunning.store(false, std::memory_order_release);
    if (mClientWriteBuf)
        shm_futex_wake(&mClientWriteBuf->io_queue.workingFlags, INT_MAX);
    if (mClientWriteConsumerThread && mClientWriteConsumerThread->joinable())
        mClientWriteConsumerThread->join();
    stopDispatch();
}

/* ── Async dispatch ──────────────────────────────────────────────── */

void ShmServerSession::startDispatch() {
    if (!mCallbacks || mCallbacks->asyncDispatchDepth == 0) return;
    mDispatchQueue.reset(new ShmDispatchQueue(mCallbacks->asyncDispatchDepth));
    mDispatchThread.reset(new std::thread(&ShmServerSession::dispatchLoop, this));
    LOGI("ShmServerSession async dispatch started (depth=%u)",
         mCallbacks->asyncDispatchDepth);
}

void ShmServerSession::stopDispatch() {
    if (mDispatchQueue) mDispatchQueue->stop();
    if (mDispatchThread && mDispatchThread->joinable()) mDispatchThread->join();
    mDispatchThread.reset();
    mDispatchQueue.reset();
}

void ShmServerSession::dispatchLoop() {
    while (auto* buf = mDispatchQueue->pop()) {
        if (!mCallbacks) { shmipc_buf_free(buf); continue; }
        if (mCallbacks->onDataZc) {
            mCallbacks->onDataZc(this, buf);           /* app owns buf */
        } else if (mCallbacks->onData) {
            mCallbacks->onData(this, buf->data, buf->len);
            shmipc_buf_free(buf);
        } else {
            shmipc_buf_free(buf);
        }
    }
}

/* ── Write-side zero-copy ────────────────────────────────────────── */

shmipc_wbuf_t* ShmServerSession::allocWriteBuf(uint32_t len) {
    if (!mServerWriteBuf || !mAlive) return nullptr;
    auto* list = &mServerWriteBuf->buffer_list;
    if (len == 0 || len > list->slice_size) return nullptr;

    uint32_t idx = alloc_slice(list);
    if (idx == INVALID_INDEX) return nullptr;

    auto* wb       = new shmipc_wbuf_t;
    wb->data       = get_slice_data(get_slice(list, idx));
    wb->capacity   = list->slice_size;
    wb->slice_idx  = idx;
    wb->manager    = mServerWriteBuf;
    return wb;
}

int ShmServerSession::sendWriteBuf(shmipc_wbuf_t* buf, uint32_t len) {
    if (!buf) return SHMIPC_ERR;
    if (!mServerWriteBuf || !mAlive || len == 0 || len > buf->capacity) {
        discardWriteBuf(buf);
        return SHMIPC_ERR;
    }

    auto* list = &mServerWriteBuf->buffer_list;
    ShmBufferSlice* s = get_slice(list, buf->slice_idx);
    s->length = len;
    s->next   = INVALID_INDEX;

    bool ok;
    {
        std::lock_guard<std::mutex> g(mWriteMutex);
        ok = shm_queue_commit(&mServerWriteBuf->io_queue, buf->slice_idx, len);
    }

    if (!ok) {
        /* Queue full — free the slice before destroying the handle. */
        free_slice(list, buf->slice_idx);
    } else {
        mBytesSent.fetch_add(len, std::memory_order_relaxed);
        mMsgsSent .fetch_add(1,   std::memory_order_relaxed);
    }
    delete buf;

    if (ok) { dataSyncServerWrite(); return SHMIPC_OK; }
    return SHMIPC_ERR;
}

void ShmServerSession::discardWriteBuf(shmipc_wbuf_t* buf) {
    if (!buf) return;
    if (buf->manager)
        free_slice(&buf->manager->buffer_list, buf->slice_idx);
    delete buf;
}

void ShmServerSession::sendShareMemoryReady() {
    if (mClientFd >= 0) {
        ShmIpcMessage msg;
        auto type = static_cast<uint8_t>(ShmProtocolType::ShareMemoryReady);
        msg.header = ShmIpcMessageHeader(type, SHM_SERVER_PROTOCOL_HEAD_SIZE, 0);
        mShmProtocolHandler->sendShmMessage(mClientFd, msg);
    }
}

void ShmServerSession::exchangeMetaData(ShmMetadata meta) {
    if (metaDataIsValid(meta)) {
        LOGI("recv ExchangeMetadata: shmSize=%u queueCap=%u sliceSize=%u",
             meta.shmSize, meta.eventQueueCapacity, meta.sliceSize);
        mMetadata = meta;
    }
    if (metaDataIsValid(mMetadata)) {
        ShmIpcMessage msg;
        msg.payload.resize(sizeof(ShmMetadata));
        memcpy(msg.payload.data(), &mMetadata, sizeof(ShmMetadata));
        auto type   = static_cast<uint8_t>(ShmProtocolType::ExchangeMetadata);
        auto length = SHM_SERVER_PROTOCOL_HEAD_SIZE + msg.payload.size();
        msg.header  = ShmIpcMessageHeader(type, length, 0);
        mShmProtocolHandler->sendShmMessage(mClientFd, msg);
    }
}

void ShmServerSession::handleShareMemoryByMemfd() {
    LOGI("recv ShareMemoryByMemfd, send AckReadyRecvFD");
    ShmIpcMessage msg;
    auto type = static_cast<uint8_t>(ShmProtocolType::AckReadyRecvFD);
    msg.header = ShmIpcMessageHeader(type, SHM_SERVER_PROTOCOL_HEAD_SIZE, 0);
    mShmProtocolHandler->sendShmMessage(mClientFd, msg);
}

void ShmServerSession::readFromClientWriteBuffer() {
    if (!mClientWriteBuf || !mCallbacks) return;
    const bool hasZc       = static_cast<bool>(mCallbacks->onDataZc);
    const bool hasData     = static_cast<bool>(mCallbacks->onData);
    const bool hasDispatch = (mDispatchQueue != nullptr);
    if (!hasZc && !hasData) return;

    auto*    queue = &mClientWriteBuf->io_queue;
    auto*    list  = &mClientWriteBuf->buffer_list;
    uint32_t cap   = queue->capacity;

    uint32_t head = queue->head.load(std::memory_order_acquire);
    uint32_t tail = queue->tail.load(std::memory_order_acquire);

    /* do-while guarantees we re-check the queue after clearing workingFlags,
     * closing the window where a producer sees flag=1 and skips the SyncEvent
     * notification while we are about to stop draining. */
    do {
        while (head != tail) {
            ShmBufferEvent& event = queue->events[head % cap];
            uint32_t slice_index  = event.slice_index;
            uint32_t total_len    = event.length;
            uint64_t latency_ns   = shm_now_ns() - event.write_ts_ns;

            if (slice_index != INVALID_INDEX) {
                mRecvLatency.record(latency_ns);
                mBytesReceived.fetch_add(total_len, std::memory_order_relaxed);
                mMsgsReceived .fetch_add(1,         std::memory_order_relaxed);

                /* Decode into shmipc_buf_t* using the shared helper.
                 * want_borrow=true only when on_data_zc is registered, so
                 * the SHM slice is freed immediately for non-ZC paths. */
                shmipc_buf_t* buf = shmipc_buf_decode(
                    list, slice_index, total_len, /*want_borrow=*/hasZc);

                if (hasDispatch) {
                    /* Async dispatch: push to queue; retry (100 µs) if full. */
                    while (!mDispatchQueue->try_push(buf)) {
                        if (!mConsumerRunning.load(std::memory_order_acquire)) {
                            shmipc_buf_free(buf);
                            goto done;
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                } else if (hasZc) {
                    mCallbacks->onDataZc(this, buf);   /* app owns buf */
                } else {
                    mCallbacks->onData(this, buf->data, buf->len);
                    shmipc_buf_free(buf);
                }
            }

            head = (head + 1) % cap;
            queue->head.store(head, std::memory_order_release);
            tail = queue->tail.load(std::memory_order_acquire);
        }

        /* Clear the working flag, then re-sample to catch any events that
         * arrived between our last tail-check and the flag clear. */
        queue->workingFlags.fetch_and(~WORKING_FLAG, std::memory_order_release);
        head = queue->head.load(std::memory_order_acquire);
        tail = queue->tail.load(std::memory_order_acquire);
    } while (head != tail);
    done:;
}

void ShmServerSession::getStatus(shmipc_session_status_t* out) const {
    if (!out) return;
    out->is_alive       = mAlive.load(std::memory_order_acquire) ? 1 : 0;
    out->bytes_sent     = mBytesSent    .load(std::memory_order_relaxed);
    out->msgs_sent      = mMsgsSent     .load(std::memory_order_relaxed);
    out->bytes_received = mBytesReceived.load(std::memory_order_relaxed);
    out->msgs_received  = mMsgsReceived .load(std::memory_order_relaxed);

    /* Instantaneous ring-buffer fullness (best-effort, no lock) */
    if (mServerWriteBuf) {
        const auto& q = mServerWriteBuf->io_queue;
        uint32_t used = q.tail.load(std::memory_order_relaxed)
                      - q.head.load(std::memory_order_relaxed);
        uint32_t cap  = q.capacity;
        out->send_buffer_used_pct = (cap > 0 && used <= cap)
                                    ? (used * 100u / cap) : 0u;
    } else {
        out->send_buffer_used_pct = 0;
    }
}
