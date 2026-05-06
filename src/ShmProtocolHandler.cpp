#include "shmipc/ShmProtocolHandler.h"

bool ShmProtocolHandler::receiveProtocolHeader(int fd, uint8_t* header, std::vector<int>& received_fds)
{
    received_fds.clear();

    size_t total = 0;

    char cmsgbuf[CMSG_SPACE(sizeof(int) * SHM_SERVER_MAX_UDS_PROTOCOL_FD)];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    while (total < SHM_SERVER_PROTOCOL_HEAD_SIZE)
    {
        struct iovec iov;
        iov.iov_base = header + total;
        iov.iov_len  = SHM_SERVER_PROTOCOL_HEAD_SIZE - total;

        struct msghdr msg{};
        msg.msg_iov    = &iov;
        msg.msg_iovlen = 1;
        /* Provide the control buffer on every call: SCM_RIGHTS may arrive on
         * any recvmsg() iteration when the header spans multiple reads. */
        msg.msg_control    = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        ssize_t n = recvmsg(fd, &msg, 0);

        if (n == 0) {
            LOGI("peer closed");
            return false;
        }

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;

            LOGE("rec msg error: %s", strerror(errno));
            return false;
        }

        /* Always process ancillary data from THIS recvmsg() call. */
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type  == SCM_RIGHTS) {
                int*   fds      = (int*)CMSG_DATA(cmsg);
                size_t fd_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < fd_count; i++)
                    received_fds.push_back(fds[i]);
            }
        }
        /* Zero the buffer so stale data from this call cannot bleed into
         * the next iteration if the kernel writes no control data there. */
        memset(cmsgbuf, 0, sizeof(cmsgbuf));

        total += n;
    }
    return true;
}

bool ShmProtocolHandler::receiveProtocolPayload(int fd, char* buf, size_t len)
{
    size_t total = 0;

    while (total < len)
    {
        ssize_t n = read(fd, buf + total, len - total);

        if (n > 0)
            total += n;
        else if (n == 0)
            return false;
        else
        {
            if (errno == EINTR)
                continue;
            return false;
        }
    }

    return true;
}

int ShmProtocolHandler::sendShmMessage(int socketFd, const ShmIpcMessage& message)
{
    /* Guard: the control-message buffer on the stack is sized for
     * SHM_SERVER_MAX_UDS_PROTOCOL_FD fds.  A larger fd vector would
     * cause a stack buffer overflow when the kernel writes SCM_RIGHTS. */
    if (message.fds.size() > SHM_SERVER_MAX_UDS_PROTOCOL_FD) {
        LOGE("sendShmMessage: too many fds (%zu, max %u)",
             message.fds.size(), SHM_SERVER_MAX_UDS_PROTOCOL_FD);
        return -1;
    }

    struct msghdr msg{};
    struct iovec iov[2];

    auto headerBytes = message.header.serialize();

    iov[0].iov_base = (void*)headerBytes.data();
    iov[0].iov_len  = headerBytes.size();

    iov[1].iov_base = (void*)message.payload.data();
    iov[1].iov_len  = message.payload.size();

    int iovcnt = message.payload.empty() ? 1 : 2;

    msg.msg_iov    = iov;
    msg.msg_iovlen = iovcnt;

    char cmsgbuf[CMSG_SPACE(sizeof(int) * SHM_SERVER_MAX_UDS_PROTOCOL_FD)];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    if (!message.fds.empty()) {
        msg.msg_control    = cmsgbuf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * message.fds.size());

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int) * message.fds.size());

        memcpy(CMSG_DATA(cmsg), message.fds.data(),
               sizeof(int) * message.fds.size());
    } else {
        msg.msg_control    = nullptr;
        msg.msg_controllen = 0;
    }

    ssize_t total    = 0;
    ssize_t expected = 0;

    for (int i = 0; i < iovcnt; ++i) {
        expected += iov[i].iov_len;
    }

    bool control_sent = false;

    while (total < expected) {
        /* MSG_NOSIGNAL: convert SIGPIPE (which would kill the process) to an
         * EPIPE errno that we can handle gracefully. */
        ssize_t n = sendmsg(socketFd, &msg, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Back-pressure: sleep 100 µs to avoid a hot busy-loop. */
                usleep(100);
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                LOGI("connection closed during send (errno=%d)", errno);
                return -1;
            }
            LOGE("send msg failed errno=%d (%s)", errno, strerror(errno));
            return -1;
        }

        if (n == 0) {
            LOGE("send msg returned 0");
            return -1;
        }

        total += n;

        if (!control_sent) {
            msg.msg_control    = nullptr;
            msg.msg_controllen = 0;
            control_sent = true;
        }

        ssize_t remaining = n;
        for (int i = 0; i < iovcnt; ++i) {
            if (remaining >= (ssize_t)iov[i].iov_len) {
                remaining -= iov[i].iov_len;
                iov[i].iov_base = (char*)iov[i].iov_base + iov[i].iov_len;
                iov[i].iov_len  = 0;
            } else {
                iov[i].iov_base = (char*)iov[i].iov_base + remaining;
                iov[i].iov_len -= remaining;
                break;
            }
        }
        msg.msg_iov = iov;
    }

    return 0;
}
