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
    /* BUG-10 fix: move all sessions out of the map while holding the lock,
     * then stop them WITHOUT the lock.  This prevents a deadlock where the
     * onDisconnected callback (invoked during join) tries to acquire
     * mClientMutex via getAllSessions() / getConnectedCount(). */
    std::map<int, std::unique_ptr<ShmServerSession>> tmp;
    {
        std::lock_guard<std::mutex> lock(mClientMutex);
        tmp = std::move(mShmClientSessionMap);
    }
    for (auto& kv : tmp) {
        kv.second->stopRunReadThreadLoop();
    }
    /* Sessions are destroyed here (unique_ptr DTOR) outside the lock. */
}

void ShmServerSessionManager::removeDeadSessions() {
    /* BUG-10 fix: collect dead session unique_ptrs outside the lock so their
     * destructors (which join threads) do not run while mClientMutex is held. */
    std::vector<std::unique_ptr<ShmServerSession>> dead;
    {
        std::lock_guard<std::mutex> lock(mClientMutex);
        for (auto it = mShmClientSessionMap.begin();
             it != mShmClientSessionMap.end(); ) {
            if (!it->second->isAlive()) {
                dead.push_back(std::move(it->second));
                it = mShmClientSessionMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    /* dead goes out of scope here: sessions are destroyed outside the lock. */
}
