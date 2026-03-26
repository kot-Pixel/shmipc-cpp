#include "shmipc/ShareMemoryManager.h"

bool ShareMemoryManager::createShareMemory(int size) {
    shareMemoryFd = syscall(SYS_memfd_create, SHARE_MEMORY_NAME, MFD_CLOEXEC);
    if (shareMemoryFd < 0) {
        LOGI("memfd_create failed");
        shareMemoryFd = -1;
        return false;
    }

    if (ftruncate(shareMemoryFd, size) < 0) {
        LOGI("ftruncate failed");
        close(shareMemoryFd);
        shareMemoryFd = -1;
        return false;
    }

    shareMemoryAddr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shareMemoryFd, 0);
    if (shareMemoryAddr == MAP_FAILED) {
        LOGE("mmap failed");
        close(shareMemoryFd);
        shareMemoryFd = -1;
        return false;
    }

    memset(shareMemoryAddr, 0, size);
    return true;
}
