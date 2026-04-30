#ifndef SHMIPC_SHMIPCMESSAGEHEADER_H
#define SHMIPC_SHMIPCMESSAGEHEADER_H

/**
    length   4 Byte  total message length including header (big-endian)
    version  1 Byte  protocol version
    type     1 Byte  message type
    fd_count 1 Byte  number of file descriptors in the message
 */

#include <cstdint>
#include <vector>
#include <arpa/inet.h>

class ShmIpcMessageHeader
{
public:
    static constexpr uint8_t CURRENT_VERSION = 1;
    static constexpr size_t HEADER_SIZE = 7;

    uint32_t length{0};
    uint8_t version{CURRENT_VERSION};
    uint8_t type{0};
    uint8_t fdCount{0};

public:
    ShmIpcMessageHeader() = default;

    ShmIpcMessageHeader(uint8_t t, uint32_t len, uint8_t fds = 0)
            : length(len), type(t), fdCount(fds)
    {}

    std::vector<uint8_t> serialize() const
    {
        std::vector<uint8_t> buf(HEADER_SIZE);

        uint32_t be_len = htonl(length);

        buf[0] = (be_len >> 24) & 0xFF;
        buf[1] = (be_len >> 16) & 0xFF;
        buf[2] = (be_len >> 8) & 0xFF;
        buf[3] = be_len & 0xFF;

        buf[4] = version;
        buf[5] = type;
        buf[6] = fdCount;

        return buf;
    }

    static ShmIpcMessageHeader deserialize(const uint8_t* data)
    {
        ShmIpcMessageHeader h;

        /* Cast each byte to uint32_t before shifting to avoid signed-integer
         * overflow UB when the high bit of data[0] is set (value >= 0x80). */
        uint32_t be_len =
                ((uint32_t)data[0] << 24) |
                ((uint32_t)data[1] << 16) |
                ((uint32_t)data[2] << 8)  |
                ((uint32_t)data[3]);

        h.length  = ntohl(be_len);
        h.version = data[4];
        h.type    = data[5];
        h.fdCount = data[6];

        return h;
    }
};

#endif //SHMIPC_SHMIPCMESSAGEHEADER_H
