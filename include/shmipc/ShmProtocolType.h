#ifndef SHMIPC_SHMPROTOCOLTYPE_H
#define SHMIPC_SHMPROTOCOLTYPE_H

#include <cstdint>

enum class ShmProtocolType : uint8_t {
    ExchangeMetadata        = 1,
    ShareMemoryByFilePath   = 2,
    ShareMemoryByMemfd      = 3,
    AckReadyRecvFD          = 4,
    AckShareMemory          = 5,
    ShareMemoryReady        = 6,
    SyncEventServerWrite    = 7,   // server notifies client: data in server_write region
    SyncEventClientWrite    = 8    // client notifies server: data in client_write region
};

#endif //SHMIPC_SHMPROTOCOLTYPE_H
