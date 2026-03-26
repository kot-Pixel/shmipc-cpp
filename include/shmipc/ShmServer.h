#ifndef SHMIPC_SHMSERVER_H
#define SHMIPC_SHMSERVER_H

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <memory>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <functional>
#include <string>

#include "ShmServerSessionManager.h"
#include "ShmLogger.h"
#include "ShmConfig.h"
#include "shmipc/shmipc.h"

class ShmServer {
    int server_fd = -1;
    std::string mName;
    std::atomic<bool> mRunning{false};
    std::unique_ptr<std::thread> mAcceptThread;
    std::unique_ptr<ShmServerSessionManager> mShmClientManager;
    ShmSessionCallbacks mSessionCallbacks;

    void acceptLoop();

public:
    ShmServer()
        : mShmClientManager(new ShmServerSessionManager())
    {}

    ~ShmServer() {
        stop();
    }

    int  start(const char* name);
    void stop();

    void setOnConnected         (std::function<void(ShmServerSession*)> cb);
    void setOnData              (std::function<void(ShmServerSession*, const void*, uint32_t)> cb);
    void setOnDataZc            (std::function<void(ShmServerSession*, shmipc_buf_t*)> cb);
    void setOnDisconnected      (std::function<void(ShmServerSession*)> cb);
    void setAsyncDispatchDepth  (uint32_t depth)
        { mSessionCallbacks.asyncDispatchDepth = depth; }


    std::vector<ShmServerSession*> getAllSessions();

    bool     isRunning()         const { return mRunning.load(std::memory_order_acquire); }
    uint32_t getConnectedCount()       { return mShmClientManager->getSessionCount(); }
};

#endif //SHMIPC_SHMSERVER_H
