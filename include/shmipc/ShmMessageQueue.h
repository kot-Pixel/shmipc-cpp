#ifndef SHMIPC_SHMMESSAGEQUEUE_H
#define SHMIPC_SHMMESSAGEQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "ShmIpcMessage.h"

class ShmMessageQueue {
public:
    void push(ShmIpcMessage&& msg) {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.push(std::move(msg));
        mCondVar.notify_one();
    }

    bool pop(ShmIpcMessage& msg) {
        std::unique_lock<std::mutex> lock(mMutex);
        mCondVar.wait(lock, [this]{ return !mQueue.empty() || mStopped; });

        if (mQueue.empty()) return false;
        msg = std::move(mQueue.front());
        mQueue.pop();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopped = true;
        }
        mCondVar.notify_all();
    }

private:
    std::queue<ShmIpcMessage> mQueue;
    std::mutex mMutex;
    std::condition_variable mCondVar;
    bool mStopped = false;
};

#endif //SHMIPC_SHMMESSAGEQUEUE_H
