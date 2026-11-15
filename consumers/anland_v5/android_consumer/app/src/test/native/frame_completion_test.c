#define _GNU_SOURCE
#include "display_consumer.h"
#include "socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "fixture: %s failed\n", what);
        exit(2);
    }
}

static display_ctx *open_session(int ctrl[2], int channels[5])
{
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ctrl) == 0, "control socket");
    display_ctx *ctx;
    require(connect_to_deamon_with_fd(&ctx, ctrl[0]) == 0, "consumer handshake");
    struct ctrl_msg hello;
    int count;
    require(recv_fds(ctrl[1], &hello, sizeof(hello), channels, 5, &count) == sizeof(hello)
            && count == 5 && hello.type == CTRL_MSG_CONSUMER_HELLO, "hello fds");
    return ctx;
}

static const struct ctrl_msg ready = {.type = CTRL_MSG_FDS_READY};
static const struct buf_info layout = {.stride = 4, .width = 1, .height = 1, .format = 1};

static int expect_pool(int data)
{
    struct data_msg pool;
    int exported, count = 0;
    require(recv_fds(data, &pool, sizeof(pool), &exported, 1, &count) == sizeof(pool) &&
            count == 1 && pool.type == DATA_MSG_BUFS_READY && pool.size == sizeof(layout),
            "receive one staging fd");
    struct buf_info info;
    require(recv_all(data, &info, sizeof(info)) == 0, "receive slot layout");
    check(info.stride == layout.stride && info.width == layout.width && info.height == layout.height,
          "exported layout matches the slot");
    return exported;
}

static void close_session(display_ctx *ctx, int daemon, const int channels[5])
{
    disconnect(ctx);
    close(daemon);
    for (int i = 0; i < 5; ++i)
        if (channels[i] >= 0)
            close(channels[i]);
}

static void test_malformed_ready(void)
{
    int ctrl[2], channels[5];
    display_ctx *ctx = open_session(ctrl, channels);
    struct ctrl_msg malformed = {.type = CTRL_MSG_FDS_READY, .size = 1};
    require(send_all(ctrl[1], &malformed, sizeof(malformed)) == 0, "send malformed READY");
    check(select_dmabuf(ctx, 0) == -1 && errno == EPROTO, "malformed READY is terminal");
    check(select_dmabuf(ctx, 0) == -1 && errno == EPIPE, "terminal handshake is not retryable");
    close_session(ctx, ctrl[1], channels);
}

static void test_ready_eof(void)
{
    int ctrl[2], channels[5];
    display_ctx *ctx = open_session(ctrl, channels);
    require(send_all(ctrl[1], &ready, 1) == 0, "send partial READY");
    shutdown(ctrl[1], SHUT_WR);
    check(select_dmabuf(ctx, 0) == -1 && errno == EPIPE, "EOF mid-READY is terminal");
    close_session(ctx, ctrl[1], channels);
}

int main(void)
{
    test_malformed_ready();
    test_ready_eof();
    int ctrl[2], channels[5];
    display_ctx *ctx = open_session(ctrl, channels);
    const int buf = memfd_create("staging fixture", MFD_CLOEXEC);
    require(buf >= 0 && push_dmabufs(ctx, &buf, &layout, 1) == 0, "register one staging slot");
    int old_export = expect_pool(channels[2]);
    check(select_dmabuf(ctx, 0) == -1 && errno == EAGAIN, "first handshake has not completed");
    check(refresh_done(ctx, 0) == DISPLAY_FRAME_FAILED, "no slot is not a no-fence success");
    require(send_all(ctrl[1], &ready, 1) == 0, "partial READY");
    check(select_dmabuf(ctx, 0) == -1 && errno == EAGAIN, "partial READY is retryable");
    require(send_all(ctrl[1], (const char *)&ready + 1, sizeof(ready) - 1) == 0, "rest of READY");
    require(select_dmabuf(ctx, 0) == 0, "select slot 0");
    eventfd_t seq;
    require(eventfd_read(channels[0], &seq) == 0 && seq == 1, "first slot notification");
    check(refresh_done(ctx, 1) == DISPLAY_FRAME_PENDING, "wait slice timeout keeps frame outstanding");
    check(release_dmabuf(ctx) < 0, "timed-out receive does not release the slot");
    check(select_dmabuf(ctx, 0) == -1 && errno == EBUSY, "cannot overwrite an in-flight frame");
    struct pollfd pending = {.fd = channels[0], .events = POLLIN};
    check(poll(&pending, 1, 0) == 0, "no second slot notification until release");
    check(push_dmabufs(ctx, &buf, &layout, 1) == -1 && errno == EALREADY,
          "cannot replace an active pool");
    pending.fd = channels[2];
    check(poll(&pending, 1, 0) == 0, "activation did not re-export the pool");
    const char marker = 0;
    require(send_all(channels[1], &marker, 1) == 0, "no-fence completion message");
    check(refresh_done(ctx, 0) == -1, "received completion without a fence remains valid");
    check(select_dmabuf(ctx, 0) == -1 && errno == EBUSY, "completion alone does not release slot");
    require(release_dmabuf(ctx) == 0, "release after simulated draw");
    require(select_dmabuf(ctx, 0) == 0, "reuse same slot after release");
    require(eventfd_read(channels[0], &seq) == 0 && seq == 1, "second slot notification");
    int done = eventfd(0, EFD_CLOEXEC);
    require(done >= 0 && send_fds(channels[1], &marker, 1, &done, 1) == 0, "completion with fence fd");
    int fence = refresh_done(ctx, 0);
    check(fence >= 0, "received completion transfers its fence fd");
    if (fence >= 0) {
        struct pollfd signal = {.fd = fence, .events = POLLIN};
        check(poll(&signal, 1, 1) == 0, "received but unsignaled fence is not ready");
        check(select_dmabuf(ctx, 0) == -1 && errno == EBUSY, "fence timeout does not reuse the slot");
        require(eventfd_write(done, 1) == 0, "signal returned fence fixture");
        check(poll(&signal, 1, 0) == 1 && (signal.revents & POLLIN),
              "returned fd refers to the supplied fence");
        close(fence);
    }
    close(done);
    require(release_dmabuf(ctx) == 0, "release after fence and simulated draw");
    require(select_dmabuf(ctx, 0) == 0, "select before producer disconnect");
    close(channels[1]);
    channels[1] = -1;
    check(refresh_done(ctx, 0) == DISPLAY_FRAME_FAILED, "producer EOF does not complete an unfinished frame");
    check(release_dmabuf(ctx) < 0, "a lost frame may not be released for reuse");
    check(select_dmabuf(ctx, 0) == -1 && errno == EPIPE, "terminal loss is not first-handshake EAGAIN");
    check(push_dmabufs(ctx, &buf, &layout, 1) == -1 && errno == EPIPE, "dead context cannot export its pool");
    struct ctrl_msg unexpected;
    check(recv(ctrl[1], &unexpected, sizeof(unexpected), MSG_DONTWAIT) == 0,
          "terminal loss shuts down control without re-depositing fds");
    close_session(ctx, ctrl[1], channels);
    close(buf);

    ctx = open_session(ctrl, channels);
    const int new_buf = memfd_create("fresh staging fixture", MFD_CLOEXEC);
    require(new_buf >= 0 && push_dmabufs(ctx, &new_buf, &layout, 1) == 0, "register fresh staging slot");
    int new_export = expect_pool(channels[2]);
    struct stat old_stat, new_stat;
    require(fstat(old_export, &old_stat) == 0 && fstat(new_export, &new_stat) == 0, "compare slots");
    check(old_stat.st_ino != new_stat.st_ino, "new session uses different storage with old fd still held");
    require(send_all(ctrl[1], &ready, sizeof(ready)) == 0 && select_dmabuf(ctx, 0) == 0,
            "activate fresh session");
    check(refresh_done(ctx, 1) == DISPLAY_FRAME_PENDING, "fresh completion still pending");
    abort_display_session(ctx); /* owner decides the total timeout has elapsed */
    check(refresh_done(ctx, 0) == DISPLAY_FRAME_FAILED && select_dmabuf(ctx, 0) < 0,
          "abandoning timed-out completion permanently retires the session");
    close_session(ctx, ctrl[1], channels);
    close(old_export);
    close(new_export);
    close(new_buf);
    return failures ? 1 : 0;
}
