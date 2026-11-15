#define _GNU_SOURCE
#include "display_consumer.h"
#include "../common/socket_utils.h"

#include <errno.h>
#include <stdatomic.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum session_state { WAITING_READY, ACTIVE, DEAD };

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;        /* render completion; bidirectional in GPU mode */
    int      shm_fd;
    int      audio_fd;        /* local end of the bidirectional audio socketpair (hello slot 4) */
    volatile uint32_t *shm_ptr;
    _Atomic enum session_state state;

    /* Render-thread only. The fds themselves stay immutable until disconnect,
     * after all users have stopped. Abort only shuts the sockets down. */
    unsigned char ready_bytes[sizeof(struct ctrl_msg)];
    size_t ready_used;
    int buf_count;
    bool gpu_offer, gpu_ready;
    enum { SLOT_IDLE, SLOT_IN_FLIGHT, SLOT_COMPLETE } slot;
    pthread_mutex_t data_lock; /* serializes writes, including service fd packets */
    struct service_info *services;
    int             num_services;
    struct resources *resources;
};

void allocate_services(struct display_ctx *ctx, struct service_info *services, int num_services){
    ctx->services = services;
    ctx->num_services = num_services;
    ctx->resources = (struct resources*)malloc(sizeof(struct resources) * num_services);
    if (!ctx->resources) {
        ctx->num_services = 0;
        return;
    }
    for(int i=0;i<num_services;i++){
        ctx->resources[i].service_type = services[i].type;
        ctx->resources[i].type = -1;//unallocated
        ctx->resources[i].num = 0;
        ctx->resources[i].fds = NULL;
    }
}
void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count);
void handle_resource_request(struct display_ctx *ctx, struct OutputEvent *event){
    uint32_t service_type = event->resources_request.type;
    uint8_t found = 0;
    int i;
    for(i=0;i<ctx->num_services;i++){
        if(ctx->services[i].type == service_type){
            //if failed fds=NULL, num=0
            struct resources res = ctx->services[i].allocate_resource(event->resources_request.args, ctx->services[i].userdata);
            if (ctx->resources[i].type != -1) {
                // free previous resource if it was allocated
                ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            }
            ctx->resources[i] = res;
            found = 1;
            break;
        }
    }
    if(!found)
        return; //do nothing if the service type is not found, the producer will not enable the service if it not received the resources back
    //send resources back to producer
    struct InputEvent input_event;
    input_event.type = INPUT_TYPE_RESOURCE;
    input_event.resource.type = service_type;
    input_event.resource.fdnum = ctx->resources[i].num;
    push_input_event_with_fds(ctx, &input_event, ctx->resources[i].fds, ctx->resources[i].num);

}
void free_resources(struct display_ctx *ctx){//释放资源，保留服务信息
    for(int i=0;i<ctx->num_services;i++){
        if(ctx->resources[i].type != -1){
            ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            ctx->resources[i].type = -1;
            ctx->resources[i].num = 0;
            ctx->resources[i].fds = NULL;
        }
    }
}
static int create_shm(display_ctx *ctx)
{
    ctx->shm_fd = memfd_create("buf_select", MFD_CLOEXEC);
    if (ctx->shm_fd < 0)
        return -1;
    if (ftruncate(ctx->shm_fd, sizeof(uint32_t)) < 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    *ctx->shm_ptr = 0;
    return 0;
}

static int send_hello_fds(display_ctx *ctx)
{
    /* Three dedicated socketpairs:
     *   - data:  consumer->producer input/bufs (reverse direction reserved for future)
     *   - fence: frame completion, and in negotiated GPU mode, slot release.
     *            SOCK_SEQPACKET advertises GPU mode without changing HELLO.
     *   - audio: full-duplex PCM -- producer writes playback, consumer writes mic.
     * We keep one end of each and hand the other to the producer. The fd slot order
     * must match the producer's pickup_fds(): { buf_ready, fence, data, shm, audio }. */
    int sv[2], fv[2], av[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (socketpair(AF_UNIX, ctx->gpu_offer ? SOCK_SEQPACKET : SOCK_STREAM, 0, fv) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    /* SEQPACKET: each PCM/format message is one atomic datagram, so neither end can
     * desync mid-frame the way a byte stream could on a partial send/recv. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, av) < 0) {
        close(sv[0]);
        close(sv[1]);
        close(fv[0]);
        close(fv[1]);
        return -1;
    }
    ctx->data_fd  = sv[0];
    ctx->fence_fd = fv[0];
    ctx->audio_fd = av[0];

    struct ctrl_msg hdr = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    int fds[5] = { ctx->buf_ready_efd, fv[1], sv[1], ctx->shm_fd, av[1] };
    int ret = send_fds(ctx->ctrl_fd, &hdr, sizeof(hdr), fds, 5);
    close(sv[1]);
    close(fv[1]);
    close(av[1]);
    return ret;
}

void abort_display_session(display_ctx *ctx)
{
    if (!ctx || atomic_exchange(&ctx->state, DEAD) == DEAD)
        return;
    shutdown(ctx->ctrl_fd, SHUT_RDWR);
    shutdown(ctx->data_fd, SHUT_RDWR);
    shutdown(ctx->fence_fd, SHUT_RDWR);
    shutdown(ctx->audio_fd, SHUT_RDWR);
}

/* FDS_READY can arrive in fragments on the control stream. Only the initial
 * handshake is retryable. Never send another HELLO or pool from this ctx. */
static int wait_for_ready(display_ctx *ctx)
{
    while (ctx->ready_used < sizeof(ctx->ready_bytes)) {
        struct iovec iov = {
            .iov_base = ctx->ready_bytes + ctx->ready_used,
            .iov_len = sizeof(ctx->ready_bytes) - ctx->ready_used,
        };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t n = recvmsg(ctx->ctrl_fd, &msg, MSG_DONTWAIT);
        if (n >= 0 && (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
            abort_display_session(ctx);
            errno = EPROTO;
            return -1;
        }
        if (n > 0) {
            ctx->ready_used += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            errno = EAGAIN;
            return -1;
        }
        abort_display_session(ctx);
        errno = EPIPE;
        return -1;
    }
    struct ctrl_msg ready;
    memcpy(&ready, ctx->ready_bytes, sizeof(ready));
    if (ready.type != CTRL_MSG_FDS_READY || ready.size != 0) {
        abort_display_session(ctx);
        errno = EPROTO;
        return -1;
    }
    enum session_state expected = WAITING_READY;
    if (!atomic_compare_exchange_strong(&ctx->state, &expected, ACTIVE)) {
        errno = EPIPE;
        return -1;
    }
    return 0;
}

static int active_or_abort(display_ctx *ctx)
{
    /* Once active, even readable control bytes are unexpected: this channel
     * has no further daemon messages for a consumer. */
    struct pollfd ctrl = { .fd = ctx->ctrl_fd, .events = POLLIN };
    int ret = poll(&ctrl, 1, 0);
    if (ctx->state == ACTIVE && (ret == 0 || (ret < 0 && errno == EINTR)))
        return 0;
    abort_display_session(ctx);
    errno = EPIPE;
    return -1;
}

static int set_write_timeout(int fd)
{
    const struct timeval timeout = { .tv_usec = 100000 };
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
static int connect_with_fd(display_ctx **out, int ctrl_fd, bool gpu_offer)
{
    *out = NULL;
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (ctrl_fd >= 0)
            close(ctrl_fd);
        return -1;
    }

    pthread_mutex_init(&ctx->data_lock, NULL);

    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->shm_ptr = NULL;
    atomic_init(&ctx->state, WAITING_READY);
    ctx->gpu_offer = gpu_offer;

    ctx->ctrl_fd = ctrl_fd;
    if (ctx->ctrl_fd < 0)
        goto fail;

    /* buf_ready_efd is the consumer->producer pacing eventfd; fence_fd is created as a
     * socketpair inside send_hello_fds(). */
    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->buf_ready_efd < 0)
        goto fail;

    if (create_shm(ctx) < 0)
        goto fail;

    if (send_hello_fds(ctx) < 0)
        goto fail;
    if (set_write_timeout(ctx->data_fd) < 0)
        goto fail;

    *out = ctx;
    return 0;

fail:
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
    return -1;
}

static int connect_path(display_ctx **out, const char *socket_path, bool gpu_offer)
{
    int fd = connect_unix(socket_path);
    if (fd >= 0 && set_write_timeout(fd) < 0) {
        int error = errno;
        close(fd);
        *out = NULL;
        errno = error;
        return -1;
    }
    return connect_with_fd(out, fd, gpu_offer);
}

int connect_to_deamon(display_ctx **out, const char *socket_path)
{
    return connect_path(out, socket_path, false);
}

int connect_to_deamon_with_fd(display_ctx **out, int ctrl_fd)
{
    return connect_with_fd(out, ctrl_fd, false);
}

int connect_to_deamon_gpu(display_ctx **out, const char *socket_path)
{
    return connect_path(out, socket_path, true);
}

int connect_to_deamon_with_fd_gpu(display_ctx **out, int ctrl_fd)
{
    return connect_with_fd(out, ctrl_fd, true);
}

void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    free_resources(ctx);
    free(ctx->resources);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
}

int set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh)
{
    if (ctx->state == DEAD) {
        errno = EPIPE;
        return -1;
    }

    struct ctrl_msg hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) };
    struct screen_info si = { .width = width, .height = height, .format = format, .refresh = refresh };
    uint8_t msg[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &si, sizeof(si));
    if (send_all(ctx->ctrl_fd, msg, sizeof(msg)) == 0)
        return 0;
    abort_display_session(ctx);
    return -1;
}

int push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count)
{
    if (ctx->state == DEAD) {
        errno = EPIPE;
        return -1;
    }
    if (ctx->buf_count) {
        errno = EALREADY;
        return -1;
    }
    if (count <= 0 || count > MAX_BUFS || !fds || !infos) {
        errno = EINVAL;
        return -1;
    }

    const struct data_msg hdr = {
        .type = DATA_MSG_BUFS_READY,
        .size = count * sizeof(*infos),
    };
    pthread_mutex_lock(&ctx->data_lock);
    bool sent = send_fds(ctx->data_fd, &hdr, sizeof(hdr), fds, count) == 0 &&
                send_all(ctx->data_fd, infos, hdr.size) == 0;
    pthread_mutex_unlock(&ctx->data_lock);
    if (!sent) {
        abort_display_session(ctx);
        return -1;
    }
    ctx->buf_count = count;
    return 0;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    if (ctx->gpu_offer) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    if (ctx->state == WAITING_READY && wait_for_ready(ctx) < 0)
        return -1;
    if (active_or_abort(ctx) < 0)
        return -1;
    if (ctx->slot != SLOT_IDLE || idx < 0 || idx >= ctx->buf_count) {
        errno = ctx->slot != SLOT_IDLE ? EBUSY : EINVAL;
        return -1;
    }
    *ctx->shm_ptr = (uint32_t)idx;
    if (eventfd_write(ctx->buf_ready_efd, 1) != 0) {
        abort_display_session(ctx);
        errno = EPIPE;
        return -1;
    }
    ctx->slot = SLOT_IN_FLIGHT;
    return 0;
}

int wait_for_gpu_staging(display_ctx *ctx, int timeout_ms)
{
    if (!ctx->gpu_offer) {
        errno = EINVAL;
        return -1;
    }
    if (ctx->gpu_ready)
        return active_or_abort(ctx);

    struct pollfd channels[] = {
        {.fd = ctx->ctrl_fd, .events = POLLIN},
        {.fd = ctx->fence_fd, .events = POLLIN},
    };
    if (ctx->state == WAITING_READY) {
        int ready = poll(channels, 1, timeout_ms);
        if (!ready || (ready < 0 && errno == EINTR)) {
            errno = EAGAIN;
            return -1;
        }
        if (ready < 0) {
            abort_display_session(ctx);
            return -1;
        }
        if (wait_for_ready(ctx) < 0)
            return -1;
        /* Do not spend a second full timeout on the ACK in this call. */
        timeout_ms = 0;
    }
    int ready = poll(channels, 2, timeout_ms);
    if (ctx->state != ACTIVE || (ready > 0 && (channels[0].revents ||
        (channels[1].revents & (POLLHUP | POLLERR | POLLNVAL))))) {
        abort_display_session(ctx);
        errno = EPIPE;
        return -1;
    }
    if (!ready || (ready < 0 && errno == EINTR))
        return 1;
    if (ready > 0 && channels[1].revents & POLLIN) {
        unsigned char marker;
        int fd;
        const int received = recv_frame_fence(ctx->fence_fd, &marker, &fd);
        if (received == 1 && marker == DISPLAY_FENCE_GPU_READY && fd < 0) {
            ctx->gpu_ready = true;
            return 0;
        }
        if (!received)
            return 1;
        if (fd >= 0)
            close(fd);
    }
    abort_display_session(ctx);
    errno = EPROTO;
    return -1;
}

int select_dmabuf_fenced(display_ctx *ctx, int idx, int release_fd)
{
    if (!ctx->gpu_ready || ctx->buf_count != 1 || idx != 0 || release_fd < -1) {
        errno = EINVAL;
        return -1;
    }
    if (active_or_abort(ctx) < 0)
        return -1;
    if (ctx->slot == SLOT_IN_FLIGHT || (ctx->slot == SLOT_IDLE && release_fd >= 0)) {
        errno = EBUSY;
        return -1;
    }
    /* GPU mode uses the fence socket itself as the ready notification. The
     * selected index is always zero and never changes while it is in use. */
    if (send_frame_fence(ctx->fence_fd, release_fd < 0 ? DISPLAY_FENCE_SLOT_READY :
                        DISPLAY_FENCE_SLOT_RELEASE, release_fd) < 0) {
        abort_display_session(ctx);
        errno = EPIPE;
        return -1;
    }
    ctx->slot = SLOT_IN_FLIGHT;
    return 0;
}

int refresh_done(display_ctx *ctx, int timeout_ms)
{
    if (ctx->slot != SLOT_IN_FLIGHT || ctx->state != ACTIVE)
        return DISPLAY_FRAME_FAILED;

    struct pollfd pfd[] = {
        { .fd = ctx->fence_fd, .events = POLLIN },
        { .fd = ctx->ctrl_fd, .events = POLLIN },
    };
    int ready = poll(pfd, 2, timeout_ms < 0 ? 0 : timeout_ms);
    if (ctx->state != ACTIVE)
        return DISPLAY_FRAME_FAILED;
    if (ready == 0 || (ready < 0 && errno == EINTR))
        return DISPLAY_FRAME_PENDING;
    if (ready < 0 || pfd[1].revents || !(pfd[0].revents & POLLIN) ||
        (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
        abort_display_session(ctx);
        return DISPLAY_FRAME_FAILED;
    }
    unsigned char marker;
    int fence;
    const int received = recv_frame_fence(ctx->fence_fd, &marker, &fence);
    if (!received)
        return DISPLAY_FRAME_PENDING;
    const bool complete = received == 1 && marker == DISPLAY_FENCE_RENDER_DONE;
    if (complete && ctx->state == ACTIVE) {
        ctx->slot = SLOT_COMPLETE;
        return fence;
    }

    if (fence >= 0)
        close(fence);
    abort_display_session(ctx);
    return DISPLAY_FRAME_FAILED;
}

int release_dmabuf(display_ctx *ctx)
{
    if (ctx->gpu_offer || ctx->state != ACTIVE || ctx->slot != SLOT_COMPLETE)
        return -1;
    ctx->slot = SLOT_IDLE;
    return 0;
}

static int push_input(display_ctx *ctx, const struct InputEvent *event, bool advisory)
{
    if (ctx->state != ACTIVE)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    if (advisory) {
        if (pthread_mutex_trylock(&ctx->data_lock) != 0)
            return 0;
    } else {
        pthread_mutex_lock(&ctx->data_lock);
    }
    ssize_t n = advisory ? send(ctx->data_fd, msg, sizeof(msg), MSG_DONTWAIT | MSG_NOSIGNAL)
                         : send_all(ctx->data_fd, msg, sizeof(msg));
    int error = errno;
    bool sent = advisory ? n == (ssize_t)sizeof(msg) : n == 0;
    bool dropped = advisory && n < 0 &&
                   (error == EAGAIN || error == EWOULDBLOCK || error == EINTR);
    /* A partial send cannot be dropped: the data stream is no longer framed. */
    if (!sent && !dropped)
        abort_display_session(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    return sent ? 1 : dropped ? 0 : -1;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    return push_input(ctx, event, false) < 0 ? -1 : 0;
}

int try_push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    return push_input(ctx, event, true);
}

int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size)
{
    if (ctx->state != ACTIVE)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    size_t total = sizeof(struct data_msg) + sizeof(struct InputEvent) + size;
    uint8_t *msg = (uint8_t *)malloc(total);
    if (!msg)
        return -1;
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    memcpy(msg + sizeof(hdr) + sizeof(struct InputEvent), payload, size);

    pthread_mutex_lock(&ctx->data_lock);
    int r = send_all(ctx->data_fd, msg, total);
    if (r < 0)
        abort_display_session(ctx);
    pthread_mutex_unlock(&ctx->data_lock);

    free(msg);
    return r;
}
int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    if (ctx->state != ACTIVE)
        return -1;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret == 0 || (ret < 0 && errno == EINTR))
        return 0;

    if (ret < 0 || pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        abort_display_session(ctx);
        return -1;
    }

    /* Consume the message directly -- no MSG_PEEK pre-check. The producer ->
     * consumer direction only ever carries OUTPUT_EVENT (render-done moved to
     * the dedicated fence channel) and both push_output_event* variants frame
     * it at exactly sizeof(struct OutputEvent), so what to read next is fully
     * determined; the peek cost one extra syscall per event. A mismatched
     * header can only mean the stream desynced: fall back rather than leaving
     * bytes wedged in the socket. */
    uint8_t msg_buf[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    if (recv_all(ctx->data_fd, msg_buf, sizeof(msg_buf)) < 0) {
        abort_display_session(ctx);
        return -1;
    }

    struct data_msg hdr;
    memcpy(&hdr, msg_buf, sizeof(hdr));
    if (hdr.type != DATA_MSG_OUTPUT_EVENT || hdr.size != sizeof(struct OutputEvent)) {
        abort_display_session(ctx);
        return -1;
    }

    memcpy(event, msg_buf + sizeof(struct data_msg), sizeof(*event));
    return 1;
}
int poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms)
{
    if (ctx->state != ACTIVE)
        return -1;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0 || pfd.revents & (POLLHUP | POLLERR | POLLNVAL) ||
        recv_all(ctx->data_fd, payload, size) < 0) {
        /* The payload was announced in the previous event; continuing would
         * parse the rest of it as an unrelated event header. */
        abort_display_session(ctx);
        return -1;
    }
    return 1;
}
int get_data_fd(display_ctx *ctx)
{
    return ctx->data_fd;
}
/* The fd belongs to this session; it is not replaced after a failure. */
int get_audio_fd(display_ctx *ctx)
{
    return ctx->state == ACTIVE ? ctx->audio_fd : -1;
}
//用于处理未处理的变长payload事件
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event)
{
    switch (event->type)
    {
    case OUTPUT_TYPE_CLIPBOARD:
        //客户端发送了一个剪贴板事件，后续会有变长数据跟随，但是库调用者没有处理这个事件，所以我们需要把后续的变长数据读掉，避免阻塞
        if (event->clipboard.size > 0) {
            void* payload = malloc(event->clipboard.size);
            if (payload) {
                poll_output_event_extend_data(ctx, payload, event->clipboard.size, 1000);
                free(payload);
            }
        }
        break;
    default:
        break;
    }
}

void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count)
{
    if (ctx->state != ACTIVE)
        return;

    /* Hold data_lock across the RESOURCE event and the EXTEND_FDS packet. */
    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    bool ok = (fd >= 0) && send_all(fd, msg, sizeof(msg)) == 0;
    if (ok && fd_count > 0) {
        struct data_msg fhdr = { .type = DATA_MSG_INPUT_EXTEND_FDS, .size = 0 };
        ok = send_fds(fd, &fhdr, sizeof(fhdr), fds, fd_count) >= 0;
    }
    if (!ok)
        abort_display_session(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
}
