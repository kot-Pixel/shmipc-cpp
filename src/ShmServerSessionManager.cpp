#include "shmipc/ShmServerSessionManager.h"

bool ShmServerSessionManager::createShmClientSession(int clientFd,
                                                      ShmSessionCallbacks* callbacks) {
    try {
        auto session = std::unique_ptr<ShmServerSession>(new ShmServerSession);
        session->mClientFd = clientFd;
        session->setCallbacks(callbacks);
        session->startRunReadThreadLoop();
        {
            std::lock_guard<std::mutex> lock(mClientMutex);
            mShmClientSessionMap[clientFd] = std::move(session);
        }
        return true;
    } catch (const std::exception& e) {
        LOGE("createShmClientSession exception: %s", e.what());
        return false;
    }
}

void ShmServerSessionManager::cleanAllShmClient() {
    std::lock_guard<std::mutex> lock(mClientMutex);
    for (auto& kv : mShmClientSessionMap) {
        kv.second->stopRunReadThreadLoop();
    }
    mShmClientSessionMap.clear();
}

void ShmServerSessionManager::removeDeadSessions() {
    std::lock_guard<std::mutex> lock(mClientMutex);
    for (auto it = mShmClientSessionMap.begin(); it != mShmClientSessionMap.end(); ) {
        if (!it->second->isAlive()) {
            it = mShmClientSessionMap.erase(it);
        } else {
            ++it;
        }
    }
}
