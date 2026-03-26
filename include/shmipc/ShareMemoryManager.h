#ifndef SHMIPC_SHAREMEMORYMANAGER_H
#define SHMIPC_SHAREMEMORYMANAGER_H

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <linux/memfd.h>

#include "ShmLogger.h"

#define SHARE_MEMORY_NAME "shareMemory"

class ShareMemoryManager {
public:
    int   shareMemoryFd   = -1;
    void* shareMemoryAddr = MAP_FAILED;

    bool createShareMemory(int size);

    ShareMemoryManager() = default;
    ~ShareMemoryManager() = default;
};

#endif //SHMIPC_SHAREMEMORYMANAGER_H
