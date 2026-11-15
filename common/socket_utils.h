#ifndef DISPLAY_SOCKET_UTILS_H
#define DISPLAY_SOCKET_UTILS_H

#include <stddef.h>

int send_fds(int sock, const void *data, size_t data_len,
             const int *fds, int fd_count);

int recv_fds(int sock, void *data, size_t data_len,
             int *fds, int fd_count, int *fds_received);

/* Single-byte frame records, with at most one owned SCM_RIGHTS fd on receive.
 * Send borrows the fd (-1 means no fd). Receive: 1 record, 0 would block,
 * -1 malformed/closed; closes every received fd on failure. */
int send_frame_fence(int sock, unsigned char marker, int fd);
int recv_frame_fence(int sock, unsigned char *marker, int *fd);

int connect_unix(const char *path);

int send_all(int fd, const void *buf, size_t len);

int recv_all(int fd, void *buf, size_t len);

#endif
