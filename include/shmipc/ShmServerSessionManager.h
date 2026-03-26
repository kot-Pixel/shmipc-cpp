#ifndef SHMIPC_SHMSERVERSESSIONMANAGER_H
#define SHMIPC_SHMSERVERSESSIONMANAGER_H

#include <vector>
#include <mutex>
#include <map>
#include <memory>

#include "ShmServerSession.h"

class ShmServerSessionManager {
private:
    std::mutex mClientMutex;
    std::map<int, std::unique_ptr<ShmServerSession>> mShmClientSessionMap;

public:
    bool createShmClientSession(int fd, ShmSessionCallbacks* callbacks);
    void cleanAllShmClient();
    void removeDeadSessions();

    std::vector<ShmServerSession*> getAllSessions() {
        std::lock_guard<std::mutex> lock(mClientMutex);
        std::vector<ShmServerSession*> result;
        for (auto& kv : mShmClientSessionMap) {
            result.push_back(kv.second.get());
        }
        return result;
    }

    uint32_t getSessionCount() {
        std::lock_guard<std::mutex> lock(mClientMutex);
        return static_cast<uint32_t>(mShmClientSessionMap.size());
    }
};

#endif //SHMIPC_SHMSERVERSESSIONMANAGER_H
