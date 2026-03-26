#include "shmipc/ShmServer.h"

int ShmServer::start(const char* name) {
    if (mRunning) return SHMIPC_ERR;

    mName = name ? name : SHM_SERVER_DEFAULT_NAME;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOGE("socket() failed");
        return SHMIPC_ERR;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, mName.c_str(), sizeof(addr.sun_path) - 2);
    int len = offsetof(struct sockaddr_un, sun_path) + 1 + mName.size();

    if (bind(server_fd, (struct sockaddr*)&addr, len) != 0) {
        LOGE("bind() failed: %s", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return SHMIPC_ERR;
    }

    if (listen(server_fd, SHM_SERVER_MAX_PROGRESS_COUNT) != 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return SHMIPC_ERR;
    }

    LOGI("server started on '%s'", mName.c_str());
    mRunning = true;
    mAcceptThread.reset(new std::thread(&ShmServer::acceptLoop, this));
    return SHMIPC_OK;
}

void ShmServer::stop() {
    if (!mRunning) return;
    mRunning = false;

    if (server_fd >= 0) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    if (mAcceptThread && mAcceptThread->joinable()) {
        mAcceptThread->join();
    }

    mShmClientManager->cleanAllShmClient();
    LOGI("server stopped");
}

void ShmServer::acceptLoop() {
    while (mRunning) {
        struct pollfd pfd;
        pfd.fd = server_fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            mShmClientManager->removeDeadSessions();
            continue;
        }

        if (!(pfd.revents & POLLIN)) continue;

        int clientFd = accept(server_fd, nullptr, nullptr);
        if (clientFd < 0) {
            if (mRunning) LOGE("accept() failed: %s", strerror(errno));
            continue;
        }

        LOGI("new client fd=%d", clientFd);
        /* onConnected is fired later, inside ShmServerSession::onSharedMemoryReady(),
         * once the full SHM handshake has completed and both buffers are ready. */
        mShmClientManager->createShmClientSession(clientFd, &mSessionCallbacks);

        mShmClientManager->removeDeadSessions();
    }
}

void ShmServer::setOnConnected(std::function<void(ShmServerSession*)> cb) {
    mSessionCallbacks.onConnected = [cb](void* s) {
        if (cb) cb(static_cast<ShmServerSession*>(s));
    };
}

void ShmServer::setOnData(std::function<void(ShmServerSession*, const void*, uint32_t)> cb) {
    mSessionCallbacks.onData = [cb](void* s, const void* data, uint32_t len) {
        if (cb) cb(static_cast<ShmServerSession*>(s), data, len);
    };
}

void ShmServer::setOnDataZc(std::function<void(ShmServerSession*, shmipc_buf_t*)> cb) {
    mSessionCallbacks.onDataZc = [cb](void* s, shmipc_buf_t* buf) {
        if (cb) cb(static_cast<ShmServerSession*>(s), buf);
    };
}

void ShmServer::setOnDisconnected(std::function<void(ShmServerSession*)> cb) {
    mSessionCallbacks.onDisconnected = [cb](void* s) {
        if (cb) cb(static_cast<ShmServerSession*>(s));
    };
}


std::vector<ShmServerSession*> ShmServer::getAllSessions() {
    return mShmClientManager->getAllSessions();
}
