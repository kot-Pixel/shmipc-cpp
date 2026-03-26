#ifndef SHMIPC_SHMIPCMESSAGE_H
#define SHMIPC_SHMIPCMESSAGE_H

#include <vector>
#include <unistd.h>

#include "ShmIpcMessageHeader.h"

class ShmIpcMessage {
public:
    ShmIpcMessageHeader header;
    std::vector<char> payload;
    std::vector<int> fds;

public:
    ShmIpcMessage() = default;

    explicit ShmIpcMessage(uint8_t type)
    {
        header.type = type;
    }

    ~ShmIpcMessage()
    {
        closeFds();
    }

    ShmIpcMessage(const ShmIpcMessage&) = delete;
    ShmIpcMessage& operator=(const ShmIpcMessage&) = delete;

    ShmIpcMessage(ShmIpcMessage&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    ShmIpcMessage& operator=(ShmIpcMessage&& other) noexcept
    {
        if (this != &other)
        {
            closeFds();
            moveFrom(std::move(other));
        }
        return *this;
    }

private:
    void closeFds()
    {
        for (auto fd : fds)
        {
            if (fd >= 0)
                close(fd);
        }
        fds.clear();
    }

    void moveFrom(ShmIpcMessage&& other)
    {
        header  = other.header;
        payload = std::move(other.payload);
        fds     = std::move(other.fds);

        other.fds.clear();
    }
};

#endif //SHMIPC_SHMIPCMESSAGE_H
