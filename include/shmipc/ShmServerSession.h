#ifndef SHMIPC_SHMSERVERSESSION_H
#define SHMIPC_SHMSERVERSESSION_H

#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>

#include "ShmProtocolHandler.h"
#include "ShmMessageQueue.h"
#include "ShmBufferManager.h"
#include "ShmMetadata.h"
#include "shmipc/shmipc.h"

struct ShmSessionCallbacks {
    std::function<void(void* /*session*/)> onConnected;
    std::function<void(void* /*session*/, const void* /*data*/, uint32_t /*len*/)> onData;
    std::function<void(void* /*session*/)> onDisconnected;
};

class ShmServerSession {
public:
    int   mClientFd  = -1;
    void* apiHandle  = nullptr;

    void startRunReadThreadLoop();
    void stopRunReadThreadLoop();

    /* timeout_ms: -1 = non-blocking (drop immediately)
     *              0 = block until space is available
     *            > 0 = block up to N milliseconds, return SHMIPC_TIMEOUT on expiry */
    int writData(const uint8_t* msg, uint32_t len, int32_t timeout_ms = -1);

    void setCallbacks(ShmSessionCallbacks* cb) { mCallbacks = cb; }
    bool isAlive() const { return mAlive.load(std::memory_order_acquire); }

    void getStatus(shmipc_session_status_t* out) const;

    ShmServerSession()
        : mShmProtocolHandler(new ShmProtocolHandler())
    {}

    ~ShmServerSession() {
        stopRunReadThreadLoop();
        cleanupSharedMemory();
    }

    void onSharedMemoryReady(void* addr, size_t size, int fd,
                             ShmBufferManager* serverWriteBuf,
                             ShmBufferManager* clientWriteBuf);
    void exchangeMetaData(ShmMetadata metadata);
    void handleShareMemoryByMemfd();

private:
    ShmMetadata         mMetadata{};
    ShmMessageQueue     mMessageQueue;
    ShmSessionCallbacks* mCallbacks = nullptr;

    std::atomic<bool> mReadThreadRunning{false};
    std::atomic<bool> mAlive{true};
    std::unique_ptr<std::thread> mReadThread;
    std::unique_ptr<std::thread> mProcessorThread;
    std::unique_ptr<ShmProtocolHandler> mShmProtocolHandler;

    std::mutex mWriteMutex;  /* serialises concurrent calls to writData() */

    /* Traffic counters — incremented atomically, never reset */
    std::atomic<uint64_t> mBytesSent{0};
    std::atomic<uint64_t> mMsgsSent{0};
    std::atomic<uint64_t> mBytesReceived{0};
    std::atomic<uint64_t> mMsgsReceived{0};

    /* Futex-driven consumer thread for the client_write region */
    std::atomic<bool>            mConsumerRunning{false};
    std::unique_ptr<std::thread> mClientWriteConsumerThread;

    void* mSharedMemoryAddr  = nullptr;
    size_t mSharedMemorySize = 0;
    int    mSharedMemoryFd   = -1;
    ShmBufferManager* mServerWriteBuf = nullptr;  // server writes → client reads
    ShmBufferManager* mClientWriteBuf = nullptr;  // client writes → server reads

    void clientUdsReader();
    void messageProcessor();
    void cleanupSharedMemory();

    void handleExchangeMetaDataMessage(const ShmIpcMessage& message);
    void handleAckShareMemoryMessage(const ShmIpcMessage& message);
    void readFromClientWriteBuffer();

    /* Returns true if the write succeeded, false if queue/slices were full.
     * Must be called with mWriteMutex held. */
    bool tryWriteOnce(const uint8_t* msg, uint32_t len);

    /* Futex-based consumer thread: blocks on workingFlags, drains client_write. */
    void clientWriteConsumerThread();
    /* Stop + join the consumer thread (idempotent). */
    void stopClientWriteConsumer();

    void dataSyncServerWrite();
    void sendShareMemoryReady();
};

#endif //SHMIPC_SHMSERVERSESSION_H
