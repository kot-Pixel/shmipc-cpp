#ifndef SHMIPC_SHMPROTOCOLHANDLER_H
#define SHMIPC_SHMPROTOCOLHANDLER_H

#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>

#include "ShmLogger.h"
#include "ShmConfig.h"
#include "ShmIpcMessage.h"
#include "ShmProtocolType.h"

class ShmProtocolHandler {
public:
    bool receiveProtocolHeader(int fd, uint8_t* header, std::vector<int>& received_fds);
    bool receiveProtocolPayload(int fd, char* buf, size_t len);
    int  sendShmMessage(int socketFd, const ShmIpcMessage& message);

    ShmProtocolHandler() = default;
    ~ShmProtocolHandler() = default;
};

#endif //SHMIPC_SHMPROTOCOLHANDLER_H
