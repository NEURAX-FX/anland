#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for recvmsg()/SCM_RIGHTS; DE builds already define it */
#endif
#include "socket_utils.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int send_fds(int sock, const void *data, size_t data_len,
             const int *fds, int fd_count)
{
    struct iovec iov = {
        .iov_base = (void *)data,
        .iov_len  = data_len,
    };

    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);

    ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    return (n == (ssize_t)data_len) ? 0 : -1;
}

int recv_fds(int sock, void *data, size_t data_len,
             int *fds, int fd_count, int *fds_received)
{
    struct iovec iov = {
        .iov_base = data,
        .iov_len  = data_len,
    };

    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0)
        return -1;

    *fds_received = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (count > fd_count)
            count = fd_count;
        memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * count);
        *fds_received = count;
    }

    return (int)n;
}

int send_frame_fence(int sock, unsigned char marker, int fd)
{
    struct iovec iov = {.iov_base = &marker, .iov_len = 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    }
    ssize_t n;
    do {
        n = sendmsg(sock, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

int recv_frame_fence(int sock, unsigned char *marker, int *fd)
{
    struct iovec iov = {.iov_base = marker, .iov_len = 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    *fd = -1;
    const ssize_t n = recvmsg(sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;

    int valid = n == 1 && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) {
            valid = 0;
            continue;
        }
        if (c->cmsg_len != CMSG_LEN(sizeof(int)))
            valid = 0;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int received;
            memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(received));
            if (*fd < 0)
                *fd = received;
            else {
                close(received);
                valid = 0;
            }
        }
    }
    if (valid)
        return 1;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    errno = n == 0 ? EPIPE : EPROTO;
    return -1;
}

int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        got += n;
    }
    return 0;
}