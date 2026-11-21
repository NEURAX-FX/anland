#define _GNU_SOURCE
#include "anland_niri_bridge.h"
#include "anland_scene.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Every fragment is released by the main thread only after the preceding
 * reconnect returned EAGAIN. No scheduler-dependent sleeps or mocked APIs. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", \
                __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { IO_TIMEOUT_MS = 3000, BUFFER_COUNT = 2, WIDTH = 64, HEIGHT = 32 };

struct identity {
    dev_t dev;
    ino_t ino;
};

struct session {
    int data[2], fence[2], audio[2];
    int ready, shm, buffers[BUFFER_COUNT];
};

struct peer {
    int listener, commands;
    struct session sessions[2];
};

static char temporary_dir[] = "/tmp/anland-readiness-XXXXXX";
static char endpoint[sizeof(((struct sockaddr_un *)0)->sun_path)];

static void cleanup_path(void)
{
    if (endpoint[0])
        unlink(endpoint);
    rmdir(temporary_dir);
}

static void deadline_expired(int signal_number)
{
    (void)signal_number;
    static const char message[] = "niri readiness regression: bounded deadline exceeded\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(EXIT_FAILURE);
}

static int64_t now_ms(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static short wait_until(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t remaining = deadline - now_ms();
        CHECK(remaining > 0);
        struct pollfd pfd = {.fd = fd, .events = events};
        int rc = poll(&pfd, 1, (int)remaining);
        if (rc < 0 && errno == EINTR)
            continue;
        CHECK(rc == 1);
        CHECK(!(pfd.revents & POLLNVAL));
        CHECK(pfd.revents & events);
        return pfd.revents;
    }
}

static void transfer(int fd, void *buffer, size_t size, bool sending)
{
    unsigned char *bytes = buffer;
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    while (size) {
        wait_until(fd, sending ? POLLOUT : POLLIN, deadline);
        ssize_t n = sending ? send(fd, bytes, size, MSG_DONTWAIT | MSG_NOSIGNAL) :
                              recv(fd, bytes, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(n > 0);
        bytes += n;
        size -= (size_t)n;
    }
}

static void send_bytes(int fd, const void *bytes, size_t size)
{
    transfer(fd, (void *)bytes, size, true);
}

static void expect_byte(int fd, unsigned char expected)
{
    unsigned char actual;
    transfer(fd, &actual, sizeof(actual), false);
    CHECK(actual == expected);
}

static void acknowledge(int fd, unsigned char command)
{
    send_bytes(fd, &command, sizeof(command));
}

static void command(int fd, unsigned char value)
{
    acknowledge(fd, value);
    expect_byte(fd, value);
}

static void send_rights(int fd, const void *bytes, size_t size,
                        const int *fds, size_t count)
{
    CHECK(count > 0 && count <= MAX_BUFS);
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))];
    } control = {0};
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = size};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = CMSG_SPACE(count * sizeof(int))};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    ssize_t n;
    for (;;) {
        wait_until(fd, POLLOUT, deadline);
        n = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(n > 0 && (size_t)n <= size);
        break;
    }
    /* Ancillary ownership transfers on the first successful byte, not again
     * if the stream happened to accept only a prefix of its accompanying data. */
    if ((size_t)n < size)
        send_bytes(fd, (const unsigned char *)bytes + n, size - (size_t)n);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        CHECK(close(*fd) == 0);
        *fd = -1;
    }
}

static struct identity identify(int fd)
{
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    return (struct identity){st.st_dev, st.st_ino};
}

static bool same_identity(struct identity a, struct identity b)
{
    return a.dev == b.dev && a.ino == b.ino;
}

static int make_memory(size_t size)
{
    int fd = memfd_create("anland-readiness", MFD_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)size) == 0);
    return fd;
}

static void make_session(struct session *session)
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, session->data) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, session->fence) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, session->audio) == 0);
    session->ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(session->ready >= 0);
    session->shm = make_memory(sizeof(uint32_t));
    uint32_t selected = 1; /* Detect fabricated slot zero as well as lost signals. */
    CHECK(pwrite(session->shm, &selected, sizeof(selected), 0) == (ssize_t)sizeof(selected));
    for (size_t i = 0; i < BUFFER_COUNT; ++i)
        session->buffers[i] = make_memory(WIDTH * HEIGHT * 4);
}

static void close_session(struct session *session)
{
    for (size_t i = 0; i < 2; ++i) {
        close_fd(&session->data[i]);
        close_fd(&session->fence[i]);
        close_fd(&session->audio[i]);
    }
    close_fd(&session->ready);
    close_fd(&session->shm);
    for (size_t i = 0; i < BUFFER_COUNT; ++i)
        close_fd(&session->buffers[i]);
}

static int accept_hello(struct peer *peer)
{
    wait_until(peer->listener, POLLIN, now_ms() + IO_TIMEOUT_MS);
    int fd = accept4(peer->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    CHECK(fd >= 0);
    struct ctrl_msg hello;
    transfer(fd, &hello, sizeof(hello), false);
    CHECK(hello.type == CTRL_MSG_PRODUCER_HELLO && hello.size == 0);
    return fd;
}

static void send_screen(int fd)
{
    struct ctrl_msg header = {.type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info)};
    struct screen_info screen = {.width = WIDTH, .height = HEIGHT, .format = 1, .refresh = 60};
    send_bytes(fd, &header, sizeof(header));
    send_bytes(fd, &screen, sizeof(screen));
}

static void expect_pickup(int fd, int commands)
{
    struct ctrl_msg request;
    transfer(fd, &request, sizeof(request), false);
    CHECK(request.type == CTRL_MSG_PICKUP_FDS && request.size == 0);
    acknowledge(commands, 'p');
}

static void buffer_record(unsigned char *bytes)
{
    struct data_msg header = {.type = DATA_MSG_BUFS_READY,
                             .size = BUFFER_COUNT * sizeof(struct buf_info)};
    struct buf_info info = {.stride = WIDTH * 4, .width = WIDTH,
                           .height = HEIGHT, .format = 1, .modifier = 0, .offset = 0};
    memcpy(bytes, &header, sizeof(header));
    for (size_t i = 0; i < BUFFER_COUNT; ++i)
        memcpy(bytes + sizeof(header) + i * sizeof(info), &info, sizeof(info));
}

static void *protocol_peer(void *userdata)
{
    struct peer *peer = userdata;
    struct session *session = &peer->sessions[0];
    struct ctrl_msg fds_ready = {.type = CTRL_MSG_FDS_READY, .size = 0};
    const unsigned char *ctrl = (const unsigned char *)&fds_ready;
    unsigned char buffers[sizeof(struct data_msg) + BUFFER_COUNT * sizeof(struct buf_info)];
    buffer_record(buffers);
    int fd = accept_hello(peer);
    send_screen(fd);
    expect_pickup(fd, peer->commands);

    expect_byte(peer->commands, 'a');
    int first[] = {session->ready, session->fence[0]};
    send_rights(fd, ctrl, 3, first, 2);
    close_fd(&session->fence[0]);
    acknowledge(peer->commands, 'a');

    expect_byte(peer->commands, 'b');
    int rest[] = {session->data[0], session->shm, session->audio[0]};
    send_rights(fd, ctrl + 3, 2, rest, 3);
    close_fd(&session->data[0]);
    close_fd(&session->shm);
    close_fd(&session->audio[0]);
    acknowledge(peer->commands, 'b');

    expect_byte(peer->commands, 'c');
    send_bytes(fd, ctrl + 5, sizeof(fds_ready) - 5);
    acknowledge(peer->commands, 'c');

    expect_byte(peer->commands, 'd');
    send_rights(session->data[1], buffers, 3, &session->buffers[0], 1);
    close_fd(&session->buffers[0]);
    acknowledge(peer->commands, 'd');

    expect_byte(peer->commands, 'e');
    /* The second fd arrives on metadata, not either header fragment. Both
     * descriptors must survive the next EAGAIN before metadata completes. */
    send_bytes(session->data[1], buffers + 3, sizeof(struct data_msg) - 3);
    send_rights(session->data[1], buffers + sizeof(struct data_msg), 7,
                &session->buffers[1], 1);
    close_fd(&session->buffers[1]);
    acknowledge(peer->commands, 'e');

    expect_byte(peer->commands, 'f');
    send_bytes(session->data[1], buffers + 15, sizeof(buffers) - 15);
    acknowledge(peer->commands, 'f');

    expect_byte(peer->commands, 's');
    CHECK(eventfd_write(session->ready, 1) == 0);
    acknowledge(peer->commands, 's');

    expect_byte(peer->commands, 'x');
    close_fd(&session->data[1]);
    close_fd(&fd);
    acknowledge(peer->commands, 'x');

    expect_byte(peer->commands, 'o');
    fd = accept_hello(peer);
    /* Deliberately withhold SCREEN_INFO until reopen has returned and the
     * main thread explicitly asks for it. A blocking reopen cannot pass. */
    acknowledge(peer->commands, 'o');
    expect_byte(peer->commands, 'i');
    send_screen(fd);
    acknowledge(peer->commands, 'i');
    expect_pickup(fd, peer->commands);

    session = &peer->sessions[1];
    expect_byte(peer->commands, 'r');
    int all[] = {session->ready, session->fence[0], session->data[0],
                 session->shm, session->audio[0]};
    send_rights(fd, ctrl, sizeof(fds_ready), all, 5);
    close_fd(&session->fence[0]);
    close_fd(&session->data[0]);
    close_fd(&session->shm);
    close_fd(&session->audio[0]);
    send_rights(session->data[1], buffers, sizeof(buffers), session->buffers, BUFFER_COUNT);
    for (size_t i = 0; i < BUFFER_COUNT; ++i)
        close_fd(&session->buffers[i]);
    acknowledge(peer->commands, 'r');

    expect_byte(peer->commands, 's');
    CHECK(eventfd_write(session->ready, 1) == 0);
    acknowledge(peer->commands, 's');
    expect_byte(peer->commands, 'q');
    close_fd(&fd);
    close_session(&peer->sessions[0]);
    close_session(&peer->sessions[1]);
    close_fd(&peer->listener);
    acknowledge(peer->commands, 'q');
    close_fd(&peer->commands);
    return NULL;
}

static anland_niri_watch_fds snapshot(anland_niri_bridge *bridge)
{
    anland_niri_watch_fds watch;
    CHECK(anland_niri_get_watch_fds(bridge, 0, &watch) == 0);
    CHECK(watch.generation != 0);
    CHECK(watch.socket_fd >= 0);
    int flags = fcntl(watch.socket_fd, F_GETFD);
    CHECK(flags >= 0 && (flags & FD_CLOEXEC));
    if (watch.ready_fd >= 0) {
        flags = fcntl(watch.ready_fd, F_GETFD);
        CHECK(flags >= 0 && (flags & FD_CLOEXEC));
    }
    anland_niri_watch_fds unchanged;
    CHECK(anland_niri_get_watch_fds(bridge, watch.generation, &unchanged) == 0);
    CHECK(unchanged.generation == watch.generation);
    CHECK(unchanged.socket_fd == -1 && unchanged.ready_fd == -1);
    CHECK(unchanged.socket_writable == watch.socket_writable);
    return watch;
}

static void close_snapshot(anland_niri_watch_fds *watch)
{
    close_fd(&watch->socket_fd);
    close_fd(&watch->ready_fd);
}

static void idle_watch(const anland_niri_watch_fds *watch)
{
    struct pollfd fds[] = {
        {.fd = watch->socket_fd, .events = watch->socket_writable ? POLLOUT : POLLIN},
        {.fd = watch->ready_fd, .events = POLLIN},
    };
    /* Assert event-loop quiescence, not a timing-sensitive CPU percentage. */
    CHECK(poll(fds, 2, 20) == 0);
}

static void no_target(anland_niri_bridge *bridge)
{
    anland_niri_event events[ANLAND_SCENE_EVENT_QUEUE];
    size_t count;
    CHECK(anland_niri_dispatch(bridge, events, ANLAND_SCENE_EVENT_QUEUE, &count) == 0);
    for (size_t i = 0; i < count; ++i) {
        CHECK(events[i].type != ANLAND_SCENE_EVENT_RENDER_TARGET_READY);
        if (events[i].release_fence_fd >= 0)
            close_fd(&events[i].release_fence_fd);
    }
    anland_niri_target target;
    CHECK(anland_niri_get_target(bridge, &target) < 0);
    CHECK(target.fd == -1);
}

static void pending(anland_niri_bridge *bridge)
{
    for (unsigned i = 0; i < 2; ++i) {
        errno = 0;
        CHECK(anland_niri_reconnect(bridge) == -1);
        CHECK(errno == EAGAIN);
        CHECK(anland_niri_daemon_alive(bridge));
        CHECK(!anland_niri_connected(bridge));
        no_target(bridge);
    }
}

static void check_pending_watch(anland_niri_bridge *bridge,
                                const anland_niri_watch_fds *previous)
{
    anland_niri_watch_fds watch = snapshot(bridge);
    CHECK(watch.generation == previous->generation);
    CHECK(same_identity(identify(watch.socket_fd), identify(previous->socket_fd)));
    CHECK(watch.socket_writable == 0 && watch.ready_fd == -1);
    idle_watch(&watch);
    /* Closing caller-owned duplicates must not close or alter the originals. */
    close_snapshot(&watch);
    pending(bridge);
}

static uint64_t target_after_signal(anland_niri_bridge *bridge, struct identity buffer)
{
    CHECK(anland_niri_pump(bridge, 0) == 0);
    anland_niri_event events[ANLAND_SCENE_EVENT_QUEUE];
    size_t count;
    unsigned targets = 0;
    CHECK(anland_niri_dispatch(bridge, events, ANLAND_SCENE_EVENT_QUEUE, &count) == 0);
    for (size_t i = 0; i < count; ++i) {
        if (events[i].type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY) {
            CHECK(events[i].index == 1 && events[i].count == BUFFER_COUNT);
            ++targets;
        }
        if (events[i].release_fence_fd >= 0)
            close_fd(&events[i].release_fence_fd);
    }
    CHECK(targets == 1);
    uint64_t generation = 0;
    for (unsigned i = 0; i < 2; ++i) {
        anland_niri_target target;
        CHECK(anland_niri_get_target(bridge, &target) == 0);
        CHECK(target.fd >= 0 && target.index == 1 && target.count == BUFFER_COUNT);
        CHECK(target.width == WIDTH && target.height == HEIGHT && target.format == 1);
        CHECK(target.stride == WIDTH * 4 && target.offset == 0 && target.modifier == 0);
        CHECK(same_identity(identify(target.fd), buffer));
        CHECK(target.generation == anland_niri_generation(bridge));
        generation = target.generation;
        close_fd(&target.fd);
    }
    return generation;
}

int main(void)
{
    struct sigaction action = {.sa_handler = deadline_expired};
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(15); /* Also bounds unexpectedly blocking production calls and joins. */
    CHECK(mkdtemp(temporary_dir) != NULL);
    CHECK(atexit(cleanup_path) == 0);
    CHECK(snprintf(endpoint, sizeof(endpoint), "%s/socket", temporary_dir) < (int)sizeof(endpoint));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, endpoint, strlen(endpoint) + 1);
    struct peer peer;
    peer.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    CHECK(peer.listener >= 0);
    CHECK(bind(peer.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(peer.listener, 2) == 0);
    int commands[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, commands) == 0);
    peer.commands = commands[1];
    struct identity data_identity[2], buffer_identity[2];
    for (size_t i = 0; i < 2; ++i) {
        make_session(&peer.sessions[i]);
        data_identity[i] = identify(peer.sessions[i].data[0]);
        buffer_identity[i] = identify(peer.sessions[i].buffers[1]);
    }
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, protocol_peer, &peer) == 0);
    anland_niri_bridge *bridge = anland_niri_open(endpoint);
    CHECK(bridge != NULL);
    anland_niri_watch_fds initial = snapshot(bridge);
    CHECK(initial.ready_fd == -1 && initial.socket_writable == 1);
    struct identity control_identity = identify(initial.socket_fd);
    close_snapshot(&initial);

    pending(bridge);
    expect_byte(commands[0], 'p');
    anland_niri_watch_fds ctrl = snapshot(bridge);
    CHECK(ctrl.generation != initial.generation);
    CHECK(same_identity(identify(ctrl.socket_fd), control_identity));
    CHECK(ctrl.ready_fd == -1 && ctrl.socket_writable == 0);
    idle_watch(&ctrl);
    for (unsigned char fragment = 'a'; fragment <= 'b'; ++fragment) {
        command(commands[0], fragment);
        wait_until(ctrl.socket_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
        pending(bridge);
        check_pending_watch(bridge, &ctrl);
    }
    command(commands[0], 'c');
    wait_until(ctrl.socket_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
    pending(bridge);
    anland_niri_watch_fds data = snapshot(bridge);
    CHECK(data.generation != ctrl.generation);
    CHECK(same_identity(identify(data.socket_fd), data_identity[0]));
    CHECK(!same_identity(identify(data.socket_fd), identify(ctrl.socket_fd)));
    CHECK(data.ready_fd == -1 && data.socket_writable == 0);
    idle_watch(&data);
    close_snapshot(&ctrl);
    for (unsigned char fragment = 'd'; fragment <= 'e'; ++fragment) {
        command(commands[0], fragment);
        wait_until(data.socket_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
        pending(bridge);
        check_pending_watch(bridge, &data);
    }
    command(commands[0], 'f');
    wait_until(data.socket_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
    CHECK(anland_niri_reconnect(bridge) == 0);
    CHECK(anland_niri_connected(bridge));
    anland_niri_watch_fds connected = snapshot(bridge);
    CHECK(connected.generation != data.generation);
    CHECK(same_identity(identify(connected.socket_fd), data_identity[0]));
    CHECK(connected.ready_fd >= 0 && connected.socket_writable == 0);
    close_snapshot(&data);
    no_target(bridge);
    idle_watch(&connected);
    anland_niri_watch_fds owned = snapshot(bridge);
    CHECK(owned.socket_fd != connected.socket_fd && owned.ready_fd != connected.ready_fd);
    close_snapshot(&owned);
    command(commands[0], 's');
    wait_until(connected.ready_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
    uint64_t first_session = target_after_signal(bridge, buffer_identity[0]);
    idle_watch(&connected);

    command(commands[0], 'x'); /* Real peer closure, not drop_session(). */
    short disconnected = wait_until(connected.socket_fd, POLLIN | POLLHUP,
                                     now_ms() + IO_TIMEOUT_MS);
    CHECK(disconnected & POLLHUP);
    CHECK(anland_niri_pump(bridge, 0) == 0);
    CHECK(!anland_niri_connected(bridge));
    CHECK(!anland_niri_daemon_alive(bridge));
    no_target(bridge);
    anland_niri_watch_fds lost;
    CHECK(anland_niri_get_watch_fds(bridge, 0, &lost) == 0);
    CHECK(lost.generation != connected.generation);
    CHECK(lost.socket_fd == -1 && lost.ready_fd == -1);
    errno = 0;
    CHECK(anland_niri_reconnect(bridge) == -1);
    CHECK(errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK);

    acknowledge(commands[0], 'o');
    CHECK(anland_niri_reopen(bridge, endpoint) == 0);
    expect_byte(commands[0], 'o'); /* Peer still has NOT sent SCREEN_INFO. */
    anland_niri_watch_fds reopened = snapshot(bridge);
    CHECK(reopened.generation != lost.generation);
    CHECK(!same_identity(identify(reopened.socket_fd), control_identity));
    CHECK(reopened.ready_fd == -1 && reopened.socket_writable == 0);
    close_snapshot(&lost);
    pending(bridge);
    idle_watch(&reopened);
    command(commands[0], 'i');
    wait_until(reopened.socket_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
    pending(bridge);
    expect_byte(commands[0], 'p');
    command(commands[0], 'r');
    CHECK(anland_niri_reconnect(bridge) == 0);
    CHECK(anland_niri_connected(bridge));
    anland_niri_watch_fds replacement = snapshot(bridge);
    CHECK(replacement.generation != reopened.generation);
    CHECK(replacement.ready_fd >= 0 && replacement.socket_writable == 0);
    CHECK(same_identity(identify(replacement.socket_fd), data_identity[1]));
    CHECK(!same_identity(identify(replacement.socket_fd), identify(connected.socket_fd)));
    close_snapshot(&reopened);
    no_target(bridge);
    idle_watch(&replacement);
    command(commands[0], 's');
    wait_until(replacement.ready_fd, POLLIN, now_ms() + IO_TIMEOUT_MS);
    struct pollfd old_ready = {.fd = connected.ready_fd, .events = POLLIN};
    CHECK(poll(&old_ready, 1, 0) == 0); /* No stale eventfd reused for the new session. */
    CHECK(target_after_signal(bridge, buffer_identity[1]) != first_session);
    idle_watch(&replacement);
    anland_niri_close(bridge);
    /* Caller-owned snapshots remain valid even after the bridge is destroyed. */
    CHECK(fcntl(connected.ready_fd, F_GETFD) >= 0);
    CHECK(fcntl(replacement.ready_fd, F_GETFD) >= 0);
    CHECK(same_identity(identify(replacement.socket_fd), data_identity[1]));
    close_snapshot(&connected);
    close_snapshot(&replacement);
    command(commands[0], 'q');
    struct timespec join_deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &join_deadline) == 0);
    join_deadline.tv_sec += IO_TIMEOUT_MS / 1000;
    CHECK(pthread_timedjoin_np(thread, NULL, &join_deadline) == 0);
    close_fd(&commands[0]);
    alarm(0);
    puts("fragmented SCM_RIGHTS handshake, idle readiness, real target signal, FD ownership and deferred reopen: passed");
    return EXIT_SUCCESS;
}
