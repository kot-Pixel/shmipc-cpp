#ifndef SHMIPC_SHMTESTPACKET_H
#define SHMIPC_SHMTESTPACKET_H

#include <cstdint>

struct ShmTestPacket {
    uint32_t seq;
    uint32_t len;
    uint8_t  data[0];
};

#endif //SHMIPC_SHMTESTPACKET_H
