#include "shmipc/ShmClientSession.h"
#include "shmipc/ShmIpcMessage.h"
#include "shmipc/ShmLogger.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <climits>
#include <thread>

ShmClientSession::ShmClientSession()
    : mProtocolHandler(new ShmProtocolHandler()) {
    /* Apply GENERAL preset as default */
    setConfig(SHMIPC_PRESET_GENERAL_SHM_SIZE,
              SHMIPC_PRESET_GENERAL_EVENT_QUEUE_CAP,
              SHMIPC_PRESET_GENERAL_SLICE_SIZE);
}

void ShmClientSession::setConfig(uint32_t shmSize,
                                  uint32_t eventQueueCapacity,
                                  uint32_t sliceSize) {
    mMetadata.magic               = SHM_MAGIC;
    mMetadata.version             = SHM_VERSION;
    mMetadata.shmSize             = shmSize;
    mMetadata.eventQueueCapacity  = eventQueueCapacity;
    mMetadata.sliceSize           = sliceSize;
}

int ShmClientSession::connect(const char* name) {
    if (mConnected) return -1;

    mName = name ? name : "";
    LOGD("connecting to '%s'", mName.c_str());

    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd < 0) { LOGE("socket() failed"); return -1; }

    struct sockaddr_un addr{};
    addr.sun_family  = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, mName.c_str(), sizeof(addr.sun_path) - 2);
    int len = offsetof(struct sockaddr_un, sun_path) + 1 + (int)mName.size();

    if (::connect(mServerFd, (struct sockaddr*)&addr, len) != 0) {
        LOGE("connect() failed: %s", strerror(errno));
        close(mServerFd);
        mServerFd = -1;
        return -1;
    }

    LOGD("socket connected, starting handshake");

    mHandshakeComplete = false;
    startThreads();
    sendExchangeMetadata();

    {
        std::unique_lock<std::mutex> lock(mHandshakeMutex);
        bool ok = mHandshakeCV.wait_for(lock, std::chrono::seconds(10),
                                        [this] { return mHandshakeComplete; });
        if (!ok) {
            LOGE("handshake timeout");
            stopThreads();
            close(mServerFd);
            mServerFd = -1;
            return -1;
        }
    }

    mConnected = true;
    LOGI("connected — shmSize=%u queueCap=%u sliceSize=%u",
         mMetadata.shmSize, mMetadata.eventQueueCapacity, mMetadata.sliceSize);

    if (mOnConnected) mOnConnected();

    return 0;
}

void ShmClientSession::disconnect() {
    if (!mConnected && !mReadThreadRunning) return;

    mConnected         = false;
    mReadThreadRunning = false;

    if (mServerFd >= 0) shutdown(mServerFd, SHUT_RDWR);

    stopThreads();
    cleanupSharedMemory();

    if (mServerFd >= 0) { close(mServerFd); mServerFd = -1; }
}

void ShmClientSession::cleanupSharedMemory() {
    if (mShareMemManager.shareMemoryAddr != MAP_FAILED &&
        mShareMemManager.shareMemoryAddr != nullptr) {
        munmap(mShareMemManager.shareMemoryAddr, mMetadata.shmSize);
        mShareMemManager.shareMemoryAddr = MAP_FAILED;
    }
    if (mShareMemManager.shareMemoryFd >= 0) {
        close(mShareMemManager.shareMemoryFd);
        mShareMemManager.shareMemoryFd = -1;
    }
    mServerWriteBuf = nullptr;
    mClientWriteBuf = nullptr;
}

int ShmClientSession::sendMessage(const ShmIpcMessage& msg) const {
    if (mServerFd < 0) return -1;
    return mProtocolHandler->sendShmMessage(mServerFd, msg);
}

void ShmClientSession::startThreads() {
    mReadThreadRunning = true;
    mProcessorThread.reset(new std::thread(&ShmClientSession::processorThread, this));
    mReadThread.reset(new std::thread(&ShmClientSession::readerThread, this));
}

void ShmClientSession::stopThreads() {
    mReadThreadRunning = false;
    mMessageQueue.stop();
    if (mReadThread    && mReadThread->joinable())    mReadThread->join();
    if (mProcessorThread && mProcessorThread->joinable()) mProcessorThread->join();
    stopServerWriteConsumer();
}

void ShmClientSession::readerThread() {
    LOGI("ShmClientSession reader start");

    uint8_t header[SHM_SERVER_PROTOCOL_HEAD_SIZE];
    std::vector<int> received_fds;

    while (mReadThreadRunning) {
        bool ok = mProtocolHandler->receiveProtocolHeader(mServerFd, header, received_fds);
        if (ok) {
            ShmIpcMessage msg;
            msg.header = ShmIpcMessageHeader::deserialize(header);
            uint32_t payloadLen = msg.header.length - SHM_SERVER_PROTOCOL_HEAD_SIZE;
            std::vector<char> payload(payloadLen);
            if (payloadLen > 0) {
                if (!mProtocolHandler->receiveProtocolPayload(
                        mServerFd, payload.data(), payloadLen)) {
                    LOGE("payload read failure");
                    break;
                }
            }
            msg.payload = std::move(payload);
            msg.fds     = std::move(received_fds);
            mMessageQueue.push(std::move(msg));
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            LOGI("server disconnected");
            break;
        }
    }

    mReadThreadRunning = false;
    bool wasConnected  = mConnected.exchange(false);

    /* Stop the consumer thread before munmapping shared memory. */
    stopServerWriteConsumer();
    cleanupSharedMemory();

    if (wasConnected && mOnDisconnected) mOnDisconnected();

    mMessageQueue.stop();
    LOGI("ShmClientSession reader stop");
}

void ShmClientSession::processorThread() {
    LOGI("ShmClientSession processor start");

    ShmIpcMessage msg;
    while (mMessageQueue.pop(msg)) {
        switch (static_cast<ShmProtocolType>(msg.header.type)) {
            case ShmProtocolType::ExchangeMetadata: handleExchangeMetadata(msg); break;
            case ShmProtocolType::AckReadyRecvFD:   handleAckReadyRecvFd();      break;
            case ShmProtocolType::ShareMemoryReady: handleShareMemoryReady();    break;
            /* SyncEventServerWrite is no longer sent over UDS — notifications
             * now arrive via futex on workingFlags inside shared memory. */
            default: break;
        }
    }

    LOGI("ShmClientSession processor stop");
}

void ShmClientSession::sendExchangeMetadata() {
    ShmIpcMessage msg;
    msg.payload.resize(sizeof(ShmMetadata));
    memcpy(msg.payload.data(), &mMetadata, sizeof(ShmMetadata));
    auto type   = static_cast<uint8_t>(ShmProtocolType::ExchangeMetadata);
    auto length = SHM_SERVER_PROTOCOL_HEAD_SIZE + msg.payload.size();
    msg.header  = ShmIpcMessageHeader(type, length, 0);
    sendMessage(msg);
}

void ShmClientSession::handleExchangeMetadata(const ShmIpcMessage& msg) {
    ShmMetadata meta{};
    if (msg.payload.size() < sizeof(ShmMetadata)) return;
    memcpy(&meta, msg.payload.data(), sizeof(ShmMetadata));
    if (!metaDataIsValid(meta)) return;

    /* Server echoes back; accept the (potentially modified) parameters */
    mMetadata = meta;
    LOGD("metadata confirmed: shmSize=%u queueCap=%u sliceSize=%u",
         mMetadata.shmSize, mMetadata.eventQueueCapacity, mMetadata.sliceSize);

    ShmIpcMessage reply;
    auto type = static_cast<uint8_t>(ShmProtocolType::ShareMemoryByMemfd);
    reply.header = ShmIpcMessageHeader(type, SHM_SERVER_PROTOCOL_HEAD_SIZE, 0);
    sendMessage(reply);
}

void ShmClientSession::handleAckReadyRecvFd() {
    LOGD("handleAckReadyRecvFd — creating memfd shmSize=%u", mMetadata.shmSize);

    if (!mShareMemManager.createShareMemory(mMetadata.shmSize)) {
        LOGE("createShareMemory failed");
        return;
    }

    size_t   half = mMetadata.shmSize / 2;
    uint32_t cap  = mMetadata.eventQueueCapacity;
    uint32_t ss   = mMetadata.sliceSize;

    /* [0, half)       = server_write region
     * [half, shmSize) = client_write region */
    init_shm_buffer_manager(mShareMemManager.shareMemoryAddr,          half, cap, ss);
    init_shm_buffer_manager((char*)mShareMemManager.shareMemoryAddr + half, half, cap, ss);

    ShmIpcMessage msg;
    msg.fds.push_back(dup(mShareMemManager.shareMemoryFd));
    auto type = static_cast<uint8_t>(ShmProtocolType::AckShareMemory);
    msg.header = ShmIpcMessageHeader(type, SHM_SERVER_PROTOCOL_HEAD_SIZE, msg.fds.size());
    sendMessage(msg);
}

void ShmClientSession::handleShareMemoryReady() {
    if (mShareMemManager.shareMemoryAddr == MAP_FAILED) return;

    size_t half     = mMetadata.shmSize / 2;
    mServerWriteBuf = attach_shm_buffer_manager(mShareMemManager.shareMemoryAddr);
    mClientWriteBuf = attach_shm_buffer_manager(
            (char*)mShareMemManager.shareMemoryAddr + half);

    LOGD("server_write: cap=%u slices=%u slice_size=%u  "
         "client_write: cap=%u slices=%u slice_size=%u",
         mServerWriteBuf->io_queue.capacity, mServerWriteBuf->buffer_list.slice_count,
         mServerWriteBuf->buffer_list.slice_size,
         mClientWriteBuf->io_queue.capacity, mClientWriteBuf->buffer_list.slice_count,
         mClientWriteBuf->buffer_list.slice_size);

    /* Start the futex-based consumer thread for the server_write region now
     * that mServerWriteBuf is valid. */
    mConsumerRunning.store(true, std::memory_order_release);
    mServerWriteConsumerThread.reset(
        new std::thread(&ShmClientSession::serverWriteConsumerThread, this));

    {
        std::lock_guard<std::mutex> lock(mHandshakeMutex);
        mHandshakeComplete = true;
    }
    mHandshakeCV.notify_one();
}

void ShmClientSession::readFromServerWriteBuffer() {
    if (!mServerWriteBuf) return;

    auto*    queue = &mServerWriteBuf->io_queue;
    auto*    list  = &mServerWriteBuf->buffer_list;
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

            if (slice_index != INVALID_INDEX) {
                std::vector<uint8_t> out;
                out.reserve(total_len);
                uint32_t remaining = total_len;
                uint32_t idx       = slice_index;

                while (idx != INVALID_INDEX && remaining > 0) {
                    ShmBufferSlice* s    = get_slice(list, idx);
                    uint8_t*        data = get_slice_data(s);
                    uint32_t        copy = std::min(remaining, s->length);
                    out.insert(out.end(), data, data + copy);
                    remaining -= copy;
                    idx = s->next;
                }

                mBytesReceived.fetch_add(out.size(), std::memory_order_relaxed);
                mMsgsReceived .fetch_add(1,          std::memory_order_relaxed);
                if (mOnData) mOnData(out.data(), out.size());

                idx = slice_index;
                while (idx != INVALID_INDEX) {
                    uint32_t next = get_slice(list, idx)->next;
                    free_slice(list, idx);
                    idx = next;
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
}

bool ShmClientSession::tryWriteOnce(const uint8_t* data, uint32_t len) {
    auto*    list          = &mClientWriteBuf->buffer_list;
    auto*    queue         = &mClientWriteBuf->io_queue;
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
        memcpy(dest, data + offset, copy);
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

    queue->events[tail].slice_index = first;
    queue->events[tail].length      = len;
    queue->tail.store(next_tail, std::memory_order_release);

    uint32_t prev_flags = queue->workingFlags.fetch_or(WORKING_FLAG, std::memory_order_acq_rel);
    if ((prev_flags & WORKING_FLAG) == 0) notifyServerOfClientWrite();

    return true;
}

int ShmClientSession::writData(const uint8_t* data, uint32_t len, int32_t timeout_ms) {
    if (!mClientWriteBuf || !data || len == 0 || !mConnected) return SHMIPC_ERR;

    using clock = std::chrono::steady_clock;
    auto deadline = (timeout_ms > 0)
        ? clock::now() + std::chrono::milliseconds(timeout_ms)
        : clock::time_point::max();

    /* Exponential backoff: 100µs → 200 → 400 → … → 10ms cap.
     * sleep_for() is a real blocking sleep (nanosleep), not a busy-spin,
     * so CPU usage stays negligible even during sustained back-pressure. */
    uint32_t sleep_us = 100;
    while (mConnected.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            if (tryWriteOnce(data, len)) {
                mBytesSent.fetch_add(len, std::memory_order_relaxed);
                mMsgsSent .fetch_add(1,   std::memory_order_relaxed);
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

void ShmClientSession::notifyServerOfClientWrite() {
    /* Replaced UDS sendmsg with a futex wake on the client_write workingFlags.
     * The server's clientWriteConsumerThread sleeps on that same address. */
    if (mClientWriteBuf && mConnected.load(std::memory_order_acquire))
        shm_futex_wake(&mClientWriteBuf->io_queue.workingFlags);
}

void ShmClientSession::serverWriteConsumerThread() {
    LOGI("ShmClientSession server_write consumer start");
    auto* flags = &mServerWriteBuf->io_queue.workingFlags;

    while (mConsumerRunning.load(std::memory_order_acquire)) {
        uint32_t val = flags->load(std::memory_order_acquire);
        if (val == 0) {
            /* Sleep until server calls shm_futex_wake, or 10 ms elapses
             * (so we can check mConsumerRunning and exit gracefully). */
            shm_futex_wait(flags, 0, 10);
            continue;
        }
        readFromServerWriteBuffer();
    }

    LOGI("ShmClientSession server_write consumer stop");
}

void ShmClientSession::stopServerWriteConsumer() {
    mConsumerRunning.store(false, std::memory_order_release);
    /* Wake the sleeping consumer thread so it can observe mConsumerRunning. */
    if (mServerWriteBuf)
        shm_futex_wake(&mServerWriteBuf->io_queue.workingFlags, INT_MAX);
    if (mServerWriteConsumerThread && mServerWriteConsumerThread->joinable())
        mServerWriteConsumerThread->join();
}

void ShmClientSession::getStatus(shmipc_client_status_t* out) const {
    if (!out) return;
    out->is_connected   = mConnected.load(std::memory_order_acquire) ? 1 : 0;
    out->bytes_sent     = mBytesSent    .load(std::memory_order_relaxed);
    out->msgs_sent      = mMsgsSent     .load(std::memory_order_relaxed);
    out->bytes_received = mBytesReceived.load(std::memory_order_relaxed);
    out->msgs_received  = mMsgsReceived .load(std::memory_order_relaxed);

    /* Instantaneous ring-buffer fullness (best-effort, no lock) */
    if (mClientWriteBuf) {
        const auto& q = mClientWriteBuf->io_queue;
        uint32_t used = q.tail.load(std::memory_order_relaxed)
                      - q.head.load(std::memory_order_relaxed);
        uint32_t cap  = q.capacity;
        out->send_buffer_used_pct = (cap > 0 && used <= cap)
                                    ? (used * 100u / cap) : 0u;
    } else {
        out->send_buffer_used_pct = 0;
    }
}
