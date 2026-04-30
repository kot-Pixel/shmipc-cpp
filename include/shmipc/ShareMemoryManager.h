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
    int    shareMemoryFd   = -1;
    void*  shareMemoryAddr = MAP_FAILED;
    size_t shareMemorySize = 0;   /* set by createShareMemory(); used by destructor */

    bool createShareMemory(size_t size);

    ShareMemoryManager() = default;

    /* Destructor releases any resources not already freed by the session's
     * cleanupSharedMemory().  The sentinel checks prevent double-release. */
    ~ShareMemoryManager() {
        if (shareMemoryAddr != MAP_FAILED && shareMemoryAddr != nullptr) {
            munmap(shareMemoryAddr, shareMemorySize);
            shareMemoryAddr = MAP_FAILED;
        }
        if (shareMemoryFd >= 0) {
            close(shareMemoryFd);
            shareMemoryFd = -1;
        }
    }
};

#endif //SHMIPC_SHAREMEMORYMANAGER_H
