#ifndef SHMIPC_SHMCLIENT_H
#define SHMIPC_SHMCLIENT_H

#include <memory>
#include <functional>
#include <string>

#include "ShmClientSession.h"

class ShmClient {
public:
    ShmClient();
    ~ShmClient();

    int  connect(const char* name);
    void disconnect();
    int  writData(const uint8_t* data, uint32_t len, int32_t timeout_ms = -1);
    bool isConnected() const;

    void setOnConnected(std::function<void()> cb);
    void setOnData(std::function<void(const void*, uint32_t)> cb);
    void setOnDisconnected(std::function<void()> cb);

    ShmClientSession* session() const { return mSession.get(); }

    void getStatus(shmipc_client_status_t* out) const {
        if (mSession) mSession->getStatus(out);
    }

private:
    std::unique_ptr<ShmClientSession> mSession;
};

#endif //SHMIPC_SHMCLIENT_H
