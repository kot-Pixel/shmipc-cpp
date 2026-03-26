#ifndef SHMIPC_SHMBENCHTESTER_H
#define SHMIPC_SHMBENCHTESTER_H

#include <cstdint>
#include <vector>
#include "ShmServerSession.h"

class ShmBenchTester {
public:
    static uint32_t seq;

    static void benchTest(ShmServerSession* serverSession, uint32_t payload_size) {
        if (serverSession != nullptr) {
            std::vector<uint8_t> buffer;
            buffer.resize(sizeof(uint32_t) * 2 + payload_size);

            uint32_t* p = (uint32_t*)buffer.data();
            p[0] = seq++;
            p[1] = payload_size;

            uint8_t* payload = buffer.data() + 8;
            for (uint32_t i = 0; i < payload_size; i++) {
                payload[i] = (uint8_t)(i % 256);
            }

            serverSession->writData(buffer.data(), buffer.size());
        }
    }
};

#endif //SHMIPC_SHMBENCHTESTER_H
