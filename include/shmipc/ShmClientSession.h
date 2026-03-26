#ifndef SHMIPC_SHMCLIENTSESSION_H
#define SHMIPC_SHMCLIENTSESSION_H

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include <string>

#include "ShmProtocolHandler.h"
#include "ShmMessageQueue.h"
#include "ShmBufferManager.h"
#include "ShmMetadata.h"
#include "ShareMemoryManager.h"
#include "ShmConfig.h"
#include "shmipc/shmipc.h"

class ShmClientSession {
public:
    void* apiHandle = nullptr;

    ShmClientSession();
    ~ShmClientSession() { disconnect(); }

    int  connect(const char* name);
    void disconnect();
    /* timeout_ms: -1 = non-blocking (drop immediately)
     *              0 = block until space is available
     *            > 0 = block up to N milliseconds, return SHMIPC_TIMEOUT on expiry */
    int  writData(const uint8_t* data, uint32_t len, int32_t timeout_ms = -1);

    bool isConnected() const { return mConnected.load(std::memory_order_acquire); }

    void getStatus(shmipc_client_status_t* out) const;

    void setOnConnected(std::function<void()> cb)                      { mOnConnected    = std::move(cb); }
    void setOnData(std::function<void(const void*, uint32_t)> cb)      { mOnData         = std::move(cb); }
    void setOnDisconnected(std::function<void()> cb)                   { mOnDisconnected = std::move(cb); }

    /* Apply preset/custom config before calling connect() */
    void setConfig(uint32_t shmSize, uint32_t eventQueueCapacity, uint32_t sliceSize);

private:
    std::string       mName;
    int               mServerFd = -1;
    ShmMetadata       mMetadata{};
    std::atomic<bool> mConnected{false};

    ShmMessageQueue mMessageQueue;
    std::atomic<bool>             mReadThreadRunning{false};
    std::unique_ptr<std::thread>  mReadThread;
    std::unique_ptr<std::thread>  mProcessorThread;
    std::unique_ptr<ShmProtocolHandler> mProtocolHandler;

    std::mutex              mHandshakeMutex;
    std::condition_variable mHandshakeCV;
    bool                    mHandshakeComplete = false;

    std::function<void()>                      mOnConnected;
    std::function<void(const void*, uint32_t)> mOnData;
    std::function<void()>                      mOnDisconnected;

    std::mutex          mWriteMutex;    /* serialises concurrent calls to writData() */

    /* Traffic counters — incremented atomically, never reset */
    std::atomic<uint64_t> mBytesSent{0};
    std::atomic<uint64_t> mMsgsSent{0};
    std::atomic<uint64_t> mBytesReceived{0};
    std::atomic<uint64_t> mMsgsReceived{0};

    /* Futex-driven consumer thread for the server_write region */
    std::atomic<bool>            mConsumerRunning{false};
    std::unique_ptr<std::thread> mServerWriteConsumerThread;

    ShareMemoryManager  mShareMemManager;
    ShmBufferManager*   mServerWriteBuf = nullptr;  // server writes → client reads
    ShmBufferManager*   mClientWriteBuf = nullptr;  // client writes → server reads

    void startThreads();
    void stopThreads();
    int  sendMessage(const ShmIpcMessage& msg) const;
    void cleanupSharedMemory();

    void readerThread();
    void processorThread();

    void sendExchangeMetadata();
    void handleExchangeMetadata(const ShmIpcMessage& msg);
    void handleAckReadyRecvFd();
    void handleShareMemoryReady();

    /* Returns true if the write succeeded, false if queue/slices were full.
     * Must be called with mWriteMutex held. */
    bool tryWriteOnce(const uint8_t* data, uint32_t len);

    /* Futex-based consumer thread: blocks on workingFlags, drains server_write. */
    void serverWriteConsumerThread();
    /* Stop + join the consumer thread (idempotent). */
    void stopServerWriteConsumer();

    void readFromServerWriteBuffer();
    void notifyServerOfClientWrite();
};

#endif //SHMIPC_SHMCLIENTSESSION_H
