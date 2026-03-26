#ifndef SHMIPC_SHMMETADATA_H
#define SHMIPC_SHMMETADATA_H

#include <cstdint>

constexpr uint32_t SHM_MAGIC   = 0x53484D49; // "SHMI"
constexpr int8_t   SHM_VERSION = 1;

#pragma pack(push, 1)
struct ShmMetadata {
    uint32_t magic;
    uint16_t version;

    uint32_t shmSize;             /* total shared memory size (both regions combined) */
    uint32_t eventQueueCapacity;  /* per-region event queue slot count (≤ SHMIPC_MAX_EVENT_QUEUE_SIZE) */
    uint32_t sliceSize;           /* per-slice data area in bytes */
};
#pragma pack(pop)

inline bool metaDataIsValid(const ShmMetadata& meta) {
    if (meta.magic   != SHM_MAGIC)   return false;
    if (meta.version  < SHM_VERSION)  return false;
    if (meta.shmSize == 0)            return false;
    if (meta.eventQueueCapacity == 0) return false;
    if (meta.sliceSize == 0)          return false;
    return true;
}

#endif //SHMIPC_SHMMETADATA_H
