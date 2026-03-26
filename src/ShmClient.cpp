#include "shmipc/ShmClient.h"

ShmClient::ShmClient()
    : mSession(new ShmClientSession())
{}

ShmClient::~ShmClient() = default;

int ShmClient::connect(const char* name) {
    return mSession->connect(name);
}

void ShmClient::disconnect() {
    mSession->disconnect();
}

int ShmClient::writData(const uint8_t* data, uint32_t len, int32_t timeout_ms) {
    return mSession->writData(data, len, timeout_ms);
}

bool ShmClient::isConnected() const {
    return mSession->isConnected();
}

void ShmClient::setOnConnected(std::function<void()> cb) {
    mSession->setOnConnected(std::move(cb));
}

void ShmClient::setOnData(std::function<void(const void*, uint32_t)> cb) {
    mSession->setOnData(std::move(cb));
}

void ShmClient::setOnDisconnected(std::function<void()> cb) {
    mSession->setOnDisconnected(std::move(cb));
}
