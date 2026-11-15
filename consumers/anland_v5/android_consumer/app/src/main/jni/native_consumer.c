#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <jni.h>
#include <linux/sync_file.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "camera_service.h"
#include "display_consumer.h"
#include "native_audio.h"
#include "protocol.h"
#include "socket_utils.h"
#include "tracy_zones.h"
#include "window_output.h"

#define TAG "Anland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FRAME_POLL_MS 16
#define GPU_ACK_TIMEOUT_NS 1000000000ULL
#define COMPLETION_TIMEOUT_NS (5ULL * 1000000000ULL)
#define FENCE_TIMEOUT_NS 1000000000ULL
#define PRESENTATION_TIMEOUT_NS 250000000ULL
#define PRESENTATION_CAPACITY 16
#define PACING_REPORT_INTERVAL_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)

struct pacing_sample {
    uint64_t total_ns;
    uint64_t worst_ns;
    uint64_t count;
};

struct consumer_pacing {
    uint64_t interval_start_ns;
    uint64_t producer_waits;
    uint64_t swaps;
    uint64_t swap_failures;
    uint64_t presented;
    uint64_t presentation_unavailable;
    uint64_t imported_fences;
    uint64_t release_fences;
    struct pacing_sample gpu_draw;
    struct pacing_sample producer_wait;
    struct pacing_sample upload;
    struct pacing_sample draw_swap;
    struct pacing_sample render_fence_wait;
};

struct pending_presentation {
    uint64_t serial;
    uint64_t frame_id;
    uint64_t deadline_ns;
};

struct presentation_queue {
    struct pending_presentation entries[PRESENTATION_CAPACITY];
    unsigned count;
};

/* Saved JVM reference for event-thread JNI callbacks. Process-global (the JVM is);
 * the per-thread env is attached as needed. The activity callback target is
 * per-instance -> consumer_state.clipboard_obj. */
static JavaVM *g_jvm = NULL;

struct consumer_state {
    pthread_mutex_t lock;
    /* Serialises the producer input stream; tablet frames hold it across their
     * four records so the FRAME remains an atomic sample commit. */
    pthread_mutex_t input_lock;
    ANativeWindow *window;
    display_ctx *ctx;
    pthread_t render_thread;
    _Atomic bool running;

    struct consumer_pacing pacing;

    // Latest display refresh rate (milli-Hz) reported from Java. Read on
    // (re)connect to seed the producer; updated live by nativeSetRefreshRate.
    _Atomic uint32_t refresh_mhz;

    // Event (output) thread
    pthread_t event_thread;
    _Atomic bool event_running;
    /* All event-thread start and join operations are owned by the render thread. */
    bool event_thread_joinable;

    /* Connection config, set from Java via nativeConfigure() and read on each
     * (re)connect in do_connect(). Guarded by cfg_lock. Per-instance. */
    pthread_mutex_t cfg_lock;
    char cfg_socket_path[256];
    bool cfg_use_root;
    char cfg_helper_path[512];
    char cfg_bridge_path[512];
    int  cfg_custom_width;
    int  cfg_custom_height;
    /* SurfaceHolder dimensions avoid stale ANativeWindow geometry after rotation. */
    int  cfg_screen_width;
    int  cfg_screen_height;
    bool cfg_topapp_enable;
    char cfg_topapp_path[512];
    int cfg_topapp_mode;
    char cfg_topapp_stops[192];
    /* Snapshot used by the helper lifecycle after do_connect() drops cfg_lock. */
    bool topapp_run_enable;
    char topapp_run_path[512];
    int topapp_run_mode;
    char topapp_run_stops[192];

    /* Foreground-scheduling state, serialized by topapp_lock. topapp_root_pid
     * is the HOST pid of the producer's session tree root (find_tree_root via
     * UNIX_DIAG): mode 2 promotes its whole tree, mode 1 uses it both as the
     * namespace anchor for the O(1) "set" switches and as the restore root --
     * on disconnect/fallback the whole tree goes back to the root cgroups in
     * one sweep, covering every pid mode 1 boosted. Written by the topapp_*
     * helpers / the event thread's scheduling handler from arbitrary
     * threads. */
    pthread_mutex_t topapp_lock;
    pid_t topapp_root_pid;

    /* Pointer-motion delta tracking (per-instance). */
    bool  motion_has_last;
    float motion_last_x, motion_last_y;

    /* Clipboard callback target: the Java object whose nativeSetClipboardText /
     * nativeClipListening / nativeClipboardSync the event thread calls (per-instance). */
    jobject clipboard_obj;

    /* Owning MainActivity: notified when a display session is lost, so Java can
     * reset input and probe whether the daemon is still reachable. */
    jobject activity_obj;

    /* Per-instance audio bridge (own AAudio streams, own producer). */
    audio_bridge *audio;

    /* Per-instance camera service registration; userdata points back at this state
     * so the camera layer can tell instances apart (see camera_service.c). */
    struct service_info camera_svc;
};

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event);
static pid_t topapp_discover_root(struct consumer_state *s, int data_fd, bool promote);

static int send_input_event(struct consumer_state *s, const struct InputEvent *event)
{
    /* Presentation is advisory: contention or backpressure must not hold up
     * the render thread or the staging slot. Take both write locks by trylock. */
    if (event->type == INPUT_TYPE_FRAME_PRESENTATION) {
        if (pthread_mutex_trylock(&s->input_lock) != 0)
            return 0;
        int rc = s->ctx ? try_push_input_event(s->ctx, event) : 0;
        pthread_mutex_unlock(&s->input_lock);
        return rc;
    }
    pthread_mutex_lock(&s->input_lock);
    int rc = s->ctx ? push_input_event(s->ctx, event) : 0;
    pthread_mutex_unlock(&s->input_lock);
    return rc;
}

static int send_input_payload(struct consumer_state *s, const struct InputEvent *event,
                              void *payload, size_t size)
{
    pthread_mutex_lock(&s->input_lock);
    int rc = s->ctx ? push_input_event_with_length(s->ctx, event, payload, size) : 0;
    pthread_mutex_unlock(&s->input_lock);
    return rc;
}

static uint64_t monotonic_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void pacing_add(struct pacing_sample *sample, uint64_t duration_ns)
{
    sample->total_ns += duration_ns;
    if (duration_ns > sample->worst_ns)
        sample->worst_ns = duration_ns;
    sample->count++;
}

static uint64_t pacing_mean_us(const struct pacing_sample *sample)
{
    return sample->count ? sample->total_ns / sample->count / 1000ULL : 0;
}

static void pacing_report_if_due(struct consumer_state *s, uint64_t now_ns)
{
    if (!s->pacing.interval_start_ns || now_ns < s->pacing.interval_start_ns ||
        now_ns - s->pacing.interval_start_ns < PACING_REPORT_INTERVAL_NS)
        return;

    const uint64_t elapsed_ns = now_ns - s->pacing.interval_start_ns;
    const uint64_t fps = elapsed_ns ? s->pacing.swaps * 1000000000ULL / elapsed_ns : 0;
    const uint64_t present_fps = s->pacing.presented * 1000000000ULL / elapsed_ns;
    LOGI("pacing swap_fps=%llu swapped=%llu swap_fail=%llu producer_wait=%llu "
         "producer_wait_us(avg/max)=%llu/%llu(n=%llu) "
         "display_mhz=%u present_fps=%llu presented=%llu present_unavailable=%llu "
         "upload_us(avg/max)=%llu/%llu draw_swap_us(avg/max)=%llu/%llu "
         "render_fence_us(avg/max)=%llu/%llu(n=%llu) "
         "gpu_draw_us(avg/max)=%llu/%llu(n=%llu) render_sync_fds=%llu release_sync_fds=%llu",
         (unsigned long long)fps,
         (unsigned long long)s->pacing.swaps,
         (unsigned long long)s->pacing.swap_failures,
         (unsigned long long)s->pacing.producer_waits,
         (unsigned long long)pacing_mean_us(&s->pacing.producer_wait),
         (unsigned long long)(s->pacing.producer_wait.worst_ns / 1000ULL),
         (unsigned long long)s->pacing.producer_wait.count,
         (unsigned)s->refresh_mhz,
         (unsigned long long)present_fps,
         (unsigned long long)s->pacing.presented,
         (unsigned long long)s->pacing.presentation_unavailable,
         (unsigned long long)pacing_mean_us(&s->pacing.upload),
         (unsigned long long)(s->pacing.upload.worst_ns / 1000ULL),
         (unsigned long long)pacing_mean_us(&s->pacing.draw_swap),
         (unsigned long long)(s->pacing.draw_swap.worst_ns / 1000ULL),
         (unsigned long long)pacing_mean_us(&s->pacing.render_fence_wait),
         (unsigned long long)(s->pacing.render_fence_wait.worst_ns / 1000ULL),
         (unsigned long long)s->pacing.render_fence_wait.count,
         (unsigned long long)pacing_mean_us(&s->pacing.gpu_draw),
         (unsigned long long)(s->pacing.gpu_draw.worst_ns / 1000ULL),
         (unsigned long long)s->pacing.gpu_draw.count,
         (unsigned long long)s->pacing.imported_fences,
         (unsigned long long)s->pacing.release_fences);
    memset(&s->pacing, 0, sizeof(s->pacing));
    s->pacing.interval_start_ns = now_ns;
}

static void send_presentation(struct consumer_state *s, uint64_t serial, uint64_t ns)
{
    if (ns)
        s->pacing.presented++;
    else
        s->pacing.presentation_unavailable++;
    const struct InputEvent event = {
        .type = INPUT_TYPE_FRAME_PRESENTATION,
        .frame_presentation = { .serial = serial, .presentation_ns = ns },
    };
    send_input_event(s, &event);
}

static void poll_presentations(struct consumer_state *s, struct window_output *output,
                               struct presentation_queue *queue)
{
    const uint64_t now = monotonic_now_ns();
    unsigned keep = 0;
    for (unsigned i = 0; i < queue->count; ++i) {
        const struct pending_presentation p = queue->entries[i];
        uint64_t actual_ns = 0;
        int rc = window_output_presentation_time(output, p.frame_id, &actual_ns);
        if (rc == 0 && now && now < p.deadline_ns) {
            queue->entries[keep++] = p;
            continue;
        }
        /* Report only Android's actual CLOCK_MONOTONIC timestamp. Expired,
         * dropped, and unsupported frames have no timestamp (0 on the wire). */
        send_presentation(s, p.serial, rc == 1 ? actual_ns : 0);
    }
    queue->count = keep;
}

static void track_presentation(struct consumer_state *s, struct presentation_queue *queue,
                                struct window_frame frame, uint64_t serial, uint64_t now)
{
    if (!frame.tracked || !now || queue->count == PRESENTATION_CAPACITY) {
        send_presentation(s, serial, 0);
        return;
    }
    queue->entries[queue->count++] = (struct pending_presentation) {
        .serial = serial, .frame_id = frame.id, .deadline_ns = now + PRESENTATION_TIMEOUT_NS,
    };
}

/* Wait only one slice at a time, both to observe stop and to query Android
 * presentation timestamps even if the producer has no new frame ready. */
static bool wait_render_fence(struct consumer_state *s, struct window_output *output,
                              struct presentation_queue *queue, int fence_fd)
{
    const uint64_t start = monotonic_now_ns();
    struct pollfd fd = { .fd = fence_fd, .events = POLLIN };
    bool signaled = false;
    while (s->running) {
        int rc = poll(&fd, 1, FRAME_POLL_MS);
        if (rc > 0) {
            if (!(fd.revents & POLLIN) || (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                break;
            /* An errored dma_fence is also signaled. POLLIN alone does not
             * establish that the producer's writes completed successfully. */
            struct sync_file_info info = {0};
            if (ioctl(fence_fd, SYNC_IOC_FILE_INFO, &info) == 0) {
                signaled = info.status > 0;
                break;
            }
            if (errno != EINTR)
                break;
        }
        if (rc < 0 && errno != EINTR)
            break;
        const uint64_t now = monotonic_now_ns();
        if (!start || !now || now - start >= FENCE_TIMEOUT_NS)
            break;
        poll_presentations(s, output, queue);
    }
    close(fence_fd);
    const uint64_t end = monotonic_now_ns();
    if (start && end >= start)
        pacing_add(&s->pacing.render_fence_wait, end - start);
    return signaled;
}

/* Report the current display refresh rate to the producer over the data
 * channel, reusing the InputEvent framing (see INPUT_TYPE_DISPLAY_REFRESH).
 * No-op when disconnected or rate unknown. */
static void send_refresh_rate(struct consumer_state *s)
{
    pthread_mutex_lock(&s->input_lock);
    const uint32_t refresh = s->refresh_mhz;
    if (s->ctx && refresh) {
        const struct InputEvent ev = {
            .type = INPUT_TYPE_DISPLAY_REFRESH,
            .display = { .refresh_mhz = refresh },
        };
        push_input_event(s->ctx, &ev);
    }
    pthread_mutex_unlock(&s->input_lock);
}

/*
 * Event thread: listens for output events (clipboard, etc.) from the producer
 * on the data_fd. Runs while s->event_running is true.
 */
static void *event_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("event thread started");

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("event thread: AttachCurrentThread failed");
        return NULL;
    }

    /* Find classes/methods once */
    jclass ctxClass = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID setClipMethod = (*env)->GetMethodID(env, ctxClass, "nativeSetClipboardText", "(Ljava/lang/String;)V");
    if (!setClipMethod) {
        LOGE("event thread: nativeSetClipboardText not found");
        (*g_jvm)->DetachCurrentThread(g_jvm);
        return NULL;
    }

    /* These may wait on AudioFlinger, su or Android's clipboard service.
     * Initialize them here, after the render thread has swapped its first frame. */
    audio_set_ctx(s->audio, s->ctx);
    audio_start(s->audio);
    if (s->event_running && s->topapp_run_enable && s->topapp_run_path[0]) {
        pid_t root = topapp_discover_root(s, get_data_fd(s->ctx), s->topapp_run_mode == 2);
        if (root > 0) {
            pthread_mutex_lock(&s->topapp_lock);
            s->topapp_root_pid = root;
            pthread_mutex_unlock(&s->topapp_lock);
        }
    }
    if (s->event_running) {
        jmethodID listen = (*env)->GetMethodID(env, ctxClass, "nativeClipListening", "(Z)V");
        if (listen)
            (*env)->CallVoidMethod(env, s->clipboard_obj, listen, JNI_TRUE);
        jmethodID sync = (*env)->GetMethodID(env, ctxClass, "nativeClipboardSync", "()V");
        if (sync)
            (*env)->CallVoidMethod(env, s->clipboard_obj, sync);
    }

    /* CONSUMER_VAR_* callbacks land on the owning MainActivity (var, value). */
    jmethodID setVarMethod = NULL;
    if (s->activity_obj) {
        jclass actClass = (*env)->GetObjectClass(env, s->activity_obj);
        setVarMethod = (*env)->GetMethodID(env, actClass, "nativeSetConsumerVar", "(II)V");
        (*env)->DeleteLocalRef(env, actClass);
        if (!setVarMethod)
            LOGE("event thread: nativeSetConsumerVar not found");
    }

    while (s->event_running) {
        struct OutputEvent ev;
        TracyCZoneN(zPoll, "poll_output_event", 1);
        int ret = poll_output_event(s->ctx, &ev, 500);
        TracyCZoneEnd(zPoll);
        if (ret < 0)
            break;
        if (ret == 0)
            continue;

        if (ev.type == OUTPUT_TYPE_RESOURCES_REQUEST) {
            /* Producer is asking for a service's fds (e.g. camera). The display lib
             * matches the type against the registered services and sends the
             * pre-created fds back over SCM_RIGHTS. */
            handle_resource_request(s->ctx, &ev);
        } else if (ev.type == OUTPUT_TYPE_CLIPBOARD && ev.clipboard.size > 0) {
            char *buf = malloc(ev.clipboard.size + 1);
            if (!buf)
                continue;

            if (poll_output_event_extend_data(s->ctx, buf, ev.clipboard.size, 5000) == 1) {
                buf[ev.clipboard.size] = '\0';
                jstring jstr = (*env)->NewStringUTF(env, buf);
                if (jstr) {
                    (*env)->CallVoidMethod(env, s->clipboard_obj, setClipMethod, jstr);
                    (*env)->DeleteLocalRef(env, jstr);
                }
            }
            free(buf);
        } else if (ev.type == OUTPUT_TYPE_SET_CONSUMER_VAR) {
            /* Producer asserts a transient runtime override. CONSUMER_VAR_CAPTURE_MOUSE
             * forces pointer capture on for Wayland pointer lock (games); 0 releases.
             * Forwarded to MainActivity, which marshals to the UI thread. */
            if (setVarMethod && s->activity_obj)
                (*env)->CallVoidMethod(env, s->activity_obj, setVarMethod,
                                       (jint)ev.set_consumer_var.var, (jint)ev.set_consumer_var.value);
        } else if (ev.type == OUTPUT_TYPE_SCHEDULING) {
            topapp_handle_scheduling_event(s, &ev);
        } else {
            /* Unknown or zero-length event: drain any trailing data if size > 0 */
            LOGI("event thread: unknown output event type=%u size=%u", ev.type, ev.clipboard.size);
        }
    }

    (*g_jvm)->DetachCurrentThread(g_jvm);
    LOGI("event thread stopped");
    return NULL;
}

static void join_event_thread(struct consumer_state *s)
{
    /* Called from the render thread, after the transport socket was shut down. */
    if (s->event_thread_joinable) {
        pthread_join(s->event_thread, NULL);
        s->event_thread_joinable = false;
    }
}

static void start_event_thread(struct consumer_state *s)
{
    if (s->event_running)
        return;
    if (s->event_thread_joinable)
        return;
    s->event_running = true;
    if (pthread_create(&s->event_thread, NULL, event_thread_func, s) == 0)
        s->event_thread_joinable = true;
    else
        s->event_running = false;
}


/*
 * Appends `in` to `out` as one shell word, separated from whatever is already
 * there: single-quoted, with embedded quotes escaped as '\''. This is what makes
 * the result safe to hand to `su -c` -- without it, a socket path containing a
 * shell metacharacter is a command run as root, and the socket path comes from
 * the saved preference or from the launch Intent, neither of which is ours.
 *
 * Returns false when the result would not fit. The caller must then refuse
 * rather than run it: a truncated command line is a different command.
 */
static bool append_quoted(char *out, size_t out_size, size_t *used, const char *in)
{
    if (*used > 0) {
        if (*used + 1 >= out_size)
            return false;
        out[(*used)++] = ' ';
    }
    if (*used + 1 >= out_size)
        return false;
    out[(*used)++] = '\'';
    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '\'') {
            if (*used + 4 >= out_size)
                return false;
            memcpy(out + *used, "'\\''", 4);
            *used += 4;
        } else {
            if (*used + 1 >= out_size)
                return false;
            out[(*used)++] = *p;
        }
    }
    if (*used + 2 >= out_size)
        return false;
    out[(*used)++] = '\'';
    out[*used] = '\0';
    return true;
}

static void *reap_root_helper(void *arg)
{
    const pid_t pid = (pid_t)(intptr_t)arg;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return NULL;
}

/*
 * "Connect with root" handshake. The app cannot connect() to a root-owned
 * daemon socket directly, so it listens on a bridge socket, launches the bundled
 * helper through `su -c`, and the helper (as root) connects to the daemon and
 * passes the connected fd back over the bridge. Returns the received fd (caller
 * owns it) or -1 on failure.
 */
static int recv_fd_via_root_helper(const struct consumer_state *s, const char *daemon_sock,
                                   const char *helper_path, const char *bridge_path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    char dir[sizeof(addr.sun_path) - sizeof("/s") + 1];
    int n = snprintf(dir, sizeof(dir), "%s.XXXXXX", bridge_path);
    if (!helper_path[0] || !bridge_path[0] || n < 0 || (size_t)n >= sizeof(dir)) {
        LOGE("root helper: missing configuration or bridge path too long");
        return -1;
    }
    /* A concurrent window or late helper from a canceled attempt must never
     * replace or connect to this attempt's socket. */
    if (!mkdtemp(dir)) {
        LOGE("root helper: mkdtemp failed: %s", strerror(errno));
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/s", dir);

    int fd = -1, peer = -1;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 1) < 0) {
        LOGE("root helper: listen on %s failed: %s", addr.sun_path, strerror(errno));
        goto out;
    }

    char inner[2048] = {0};
    size_t used = 0;
    if (!append_quoted(inner, sizeof(inner), &used, helper_path) ||
        !append_quoted(inner, sizeof(inner), &used, daemon_sock) ||
        !append_quoted(inner, sizeof(inner), &used, addr.sun_path)) {
        LOGE("root helper: command line too long to quote safely");
        goto out;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("root helper: fork failed: %s", strerror(errno));
        goto out;
    }
    if (pid == 0) {
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);
    }

    /* Preserve time for a root prompt without holding nativeStop's join.
     * After the child exits, still drain an already queued connection/fd. */
    const uint64_t deadline = monotonic_now_ns() + 30ULL * 1000000000;
    pid_t child = 0;
    int status = 0;
    while (s->running && monotonic_now_ns() < deadline) {
        struct pollfd pfd = { .fd = peer < 0 ? listener : peer, .events = POLLIN };
        int ready = poll(&pfd, 1, child == 0 ? 50 : 0);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || !(pfd.revents & POLLIN)) {
            if (ready != 0 || child != 0)
                break;
            child = waitpid(pid, &status, WNOHANG);
            if (child < 0 && errno == EINTR)
                child = 0;
            continue;
        }
        if (peer < 0) {
            peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (peer < 0 && errno != EINTR && errno != EAGAIN)
                break;
        } else {
            char marker;
            int count;
            if (recv_fds(peer, &marker, 1, &fd, 1, &count) == 1 && count == 1) {
                if (marker == 'F')
                    break;
                close(fd);
            }
            fd = -1;
            break;
        }
    }
    if (child == 0) {
        do {
            child = waitpid(pid, &status, WNOHANG);
        } while (child < 0 && errno == EINTR);
        if (child == 0) {
            if (fd < 0)
                kill(pid, SIGKILL);
            /* su may already be running as root, so even after attempting to
             * cancel it, reap independently of the window and render thread. */
            pthread_t reaper;
            int rc = pthread_create(&reaper, NULL, reap_root_helper, (void *)(intptr_t)pid);
            if (rc == 0)
                pthread_detach(reaper);
            else
                LOGE("root helper: could not start child reaper: %s", strerror(rc));
        }
    }
    if (fd < 0 && s->running)
        LOGE("root helper: no daemon fd for %s (su status=%d)", daemon_sock, status);

out:
    if (peer >= 0)
        close(peer);
    if (listener >= 0)
        close(listener);
    unlink(addr.sun_path);
    rmdir(dir);
    return fd;
}

/*
 * Foreground scheduling ("启用前台调度 (root)"). No resident helper: each
 * transition is one `su -c libsettopapp.so ...` invocation.
 *
 * Mode 1 (focused app): at exit-fallback the consumer asks the helper for the
 * HOST pid of the session anchor (UNIX_DIAG lookup, "noset"); that anchor
 * both pins the producer's PID namespace and its /proc/<pid>/root/proc.
 * Every SCHEDULING report from the producer (producer subtree at init, then
 * the focused client subtree on each activation change) becomes an O(1) "set"
 * call that setns()es into the anchor's PID namespace and writes the
 * container pid straight into the cgroup files. The producer orders the
 * reports self-contained (off before on), so no consumer-side tracking is
 * needed.
 *
 * Mode 2 (whole session): unchanged legacy behaviour -- promote the whole
 * producer tree at exit-fallback.
 *
 * Both modes restore identically on fallback/stop: one "restore" sweep over
 * the whole tree rooted at the scanned session root, which covers every pid
 * either mode boosted.
 */

static bool safe_helper_path(const char *path)
{
    if (!path || path[0] != '/')
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (!(isalnum(*p) || *p == '/' || *p == '.' || *p == '_'
              || *p == '-' || *p == '+' || *p == '=' || *p == ':'
              || *p == '~')) {
            return false;
        }
    }
    return true;
}

/* Run `su -c "<cmd>"`, capture its first stdout line. Returns the parsed pid
 * (>0), or -1 on any failure (no su, helper missing, no output). Blocks for
 * the duration of the helper; called from the render/event threads where a
 * short stall at a session boundary is acceptable. */
static pid_t run_su_capture_pid(const char *cmd)
{
    int link[2];
    if (pipe(link) < 0) {
        LOGE("settopapp: pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        close(link[0]);
        close(link[1]);
        return -1;
    }
    if (pid == 0) {
        /* su prompt/progress output must not pollute the pid line. */
        close(link[0]);
        dup2(link[1], STDOUT_FILENO);
        close(link[1]);
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(link[1]);

    char line[32];
    ssize_t n = read(link[0], line, sizeof(line) - 1);
    close(link[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("settopapp: helper failed (status=%d, read=%zd)", status, n);
        return -1;
    }
    line[n] = '\0';
    int got = atoi(line);
    return got > 0 ? (pid_t)got : -1;
}

/* Run `su -c "<cmd>"` ignoring its output; returns the su exit status, or -1
 * on fork/exec failure. Used for the mode-1 "set" switches, which report
 * success through logcat rather than stdout. */
static int run_su_status(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Resolve the producer (holder of the peer of data socket `data_fd`) to its
 * session-tree-root HOST pid via the helper. promote=false only discovers
 * ("noset"); promote=true additionally moves the whole tree into top-app
 * (mode 2), honouring the optional custom stop-name list. Returns the root
 * pid, or -1. Called at exit-fallback, i.e. the producer is connected right
 * now, so the diag lookup has a live peer; data_fd is live for this whole
 * call (enter_fallback closes it only after this returns). In mode 1 the
 * returned anchor pins the producer's PID namespace (/proc/<pid>/ns/pid) and
 * its /proc/<pid>/root/proc for the later "set" calls. */
static pid_t topapp_discover_root(struct consumer_state *s, int data_fd,
                                  bool promote)
{
    struct stat st;
    if (data_fd < 0 || fstat(data_fd, &st) < 0 || st.st_ino == 0
        || !safe_helper_path(s->topapp_run_path)) {
        LOGE("settopapp: data fd not resolvable");
        return -1;
    }

    /* The inode identifies our end of the data socketpair; the helper resolves
     * it to the peer the producer holds. The optional stop-name list bounds
     * the upward PPID scan in both modes: mode 2 uses the scanned root as the
     * promote tree root, mode 1 as the namespace anchor (and restore root). */
    char cmd[sizeof(s->topapp_run_path) + sizeof(s->topapp_run_stops) + 32];
    if (s->topapp_run_stops[0] != '\0' && promote)
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (s->topapp_run_stops[0] != '\0')
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (promote)
        snprintf(cmd, sizeof(cmd), "%s %llu", s->topapp_run_path,
                 (unsigned long long)st.st_ino);
    else
        snprintf(cmd, sizeof(cmd), "%s %llu noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino);

    pid_t root = run_su_capture_pid(cmd);
    if (root > 0)
        LOGI("settopapp: tree root %d %s", (int)root,
             promote ? "promoted to top-app" : "resolved (anchor)");
    return root;
}

/* Mode 1: O(1) switch of one container pid (or its subtree) between top-app
 * and the root groups, via the anchor's PID namespace. */
static int topapp_set(struct consumer_state *s, pid_t container_pid,
                      bool on, bool settree)
{
    pthread_mutex_lock(&s->topapp_lock);
    const pid_t anchor = s->topapp_root_pid;
    pthread_mutex_unlock(&s->topapp_lock);
    if (anchor <= 0)
        return -1;

    char cmd[sizeof(s->topapp_run_path) + 96];
    const int length = snprintf(cmd, sizeof(cmd), "%s set %d %d opt=%s settree=%s",
                                s->topapp_run_path, (int)anchor, (int)container_pid,
                                on ? "on" : "off", settree ? "true" : "false");
    if (length <= 0 || (size_t)length >= sizeof(cmd))
        return -1;
    return run_su_status(cmd);
}

/* Undo every mode-1 boost of this connection: the focused pid (if any) and
 * KWin's own subtree back to the root groups. */
/* Undo every boost of this connection: restore the whole scanned tree rooted
 * at topapp_root_pid back to the root cgroups. Shared by both modes -- the
 * anchor tree covers KWin's subtree and every focused client subtree mode 1
 * boosted, and mode 2 promoted exactly that tree in the first place. Safe to
 * call with the producer already gone: restore walks the root's tree as it
 * exists NOW (leftover processes still move back; dead ones skip). */
static void topapp_restore_tree(struct consumer_state *s)
{
    pthread_mutex_lock(&s->topapp_lock);
    pid_t root = s->topapp_root_pid;
    s->topapp_root_pid = 0;
    pthread_mutex_unlock(&s->topapp_lock);

    if (root <= 0)
        return;

    char cmd[sizeof(s->topapp_run_path) + 32];
    snprintf(cmd, sizeof(cmd), "%s %d restore", s->topapp_run_path, (int)root);
    if (run_su_capture_pid(cmd) > 0)
        LOGI("settopapp: tree root %d restored", (int)root);
}

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event)
{
    /* Only mode 1 consumes per-pid reports (mode 2 promoted the whole tree
     * once at exit-fallback), and only when the feature is enabled for this
     * connection -- with foreground scheduling off the producer may still
     * send events (it cannot know the Android-side setting), and they must
     * not spawn su invocations. */
    if (s->topapp_run_mode != 1 || !s->topapp_run_enable)
        return;

    pthread_mutex_lock(&s->topapp_lock);
    const bool have_anchor = s->topapp_root_pid > 0;
    pthread_mutex_unlock(&s->topapp_lock);
    if (!have_anchor)
        return;

    /* Self-contained switch: the producer already ordered the events so an
     * "off" for the previous client arrives before the "on" for the next.
     * Every event maps 1:1 onto one O(1) helper "set" invocation. */
    const bool on = event->scheduling.flags & SCHEDULING_FLAG_ON;
    const bool settree = event->scheduling.flags & SCHEDULING_FLAG_SETTREE;

    if (event->scheduling.pid == 0 && !on)
        return;   /* nothing to restore; no focused client reported */

    if (topapp_set(s, event->scheduling.pid, on, settree) != 0)
        LOGE("settopapp: set pid=%d on=%d tree=%d failed",
             (int)event->scheduling.pid, on, settree);
    else
        LOGI("settopapp: pid %d %s (%s)", (int)event->scheduling.pid,
             on ? "promoted" : "restored",
             settree ? "tree" : "process");
}

static int do_connect(struct consumer_state *s, struct window_output **output, bool gpu_staging)
{
    memset(&s->pacing, 0, sizeof(s->pacing));
    /* Snapshot the connection config for this attempt. */
    pthread_mutex_lock(&s->cfg_lock);
    bool use_root = s->cfg_use_root;
    char sock_path[sizeof(s->cfg_socket_path)];
    char helper_path[sizeof(s->cfg_helper_path)];
    char bridge_path[sizeof(s->cfg_bridge_path)];
    memcpy(sock_path, s->cfg_socket_path, sizeof(sock_path));
    memcpy(helper_path, s->cfg_helper_path, sizeof(helper_path));
    memcpy(bridge_path, s->cfg_bridge_path, sizeof(bridge_path));
    s->topapp_run_enable = s->cfg_topapp_enable;
    memcpy(s->topapp_run_path, s->cfg_topapp_path, sizeof(s->topapp_run_path));
    s->topapp_run_mode = s->cfg_topapp_mode == 2 ? 2 : 1;
    memcpy(s->topapp_run_stops, s->cfg_topapp_stops,
           sizeof(s->topapp_run_stops));
    int cw = s->cfg_custom_width;
    int ch = s->cfg_custom_height;
    int sw = s->cfg_screen_width;
    int sh = s->cfg_screen_height;
    pthread_mutex_unlock(&s->cfg_lock);

    const char *sock = sock_path;

    int width, height;
    if (cw > 0 && ch > 0) {
        width = cw;
        height = ch;
    } else if (sw > 0 && sh > 0) {
        width = sw;
        height = sh;
    } else {
        width = ANativeWindow_getWidth(s->window);
        height = ANativeWindow_getHeight(s->window);
    }

    LOGI("connecting to %s (%dx%d, one staging slot, root=%d)", sock, width, height, use_root);
    display_ctx *ctx = NULL;
    if (use_root) {
        int ctrl_fd = recv_fd_via_root_helper(s, sock, helper_path, bridge_path);
        if (ctrl_fd < 0)
            return -1;
        int rc = gpu_staging ? connect_to_deamon_with_fd_gpu(&ctx, ctrl_fd) :
                               connect_to_deamon_with_fd(&ctx, ctrl_fd);
        if (rc < 0) {
            LOGE("daemon HELLO via root helper failed for %s: %s", sock, strerror(errno));
            return -1;
        }
    } else {
        int rc = gpu_staging ? connect_to_deamon_gpu(&ctx, sock) : connect_to_deamon(&ctx, sock);
        if (rc < 0) {
            LOGE("daemon connect/HELLO failed for %s: %s", sock, strerror(errno));
            return -1;
        }
    }
    if (!s->running)
        goto fail;
    LOGI("daemon HELLO sent for %s; initializing %s window and staging slot",
         sock, gpu_staging ? "Vulkan" : "EGL");
    *output = gpu_staging ? window_output_create_gpu(s->window, width, height) :
                            window_output_create(s->window, width, height);
    if (!*output) {
        LOGE("cannot create %s output and staging slot", gpu_staging ? "Vulkan" : "EGL");
        abort_display_session(ctx);
        disconnect(ctx);
        return gpu_staging ? -2 : -1;
    }
    struct buf_info info;
    const int fd = window_output_get_buffer(*output, &info);
    if (fd < 0 || set_screen_info(ctx, width, height, info.format, s->refresh_mhz) < 0 ||
        push_dmabufs(ctx, &fd, &info, 1) < 0) {
        LOGE("cannot advertise display and staging slot");
        goto fail;
    }
    LOGI("staging slot advertised for %s (%ux%u stride=%u); waiting for FDS_READY",
         sock, info.width, info.height, info.stride);

    /* Register the camera service only when it was initialised (i.e. the user
     * enabled it in settings and granted CAMERA). The service_info lives in this
     * per-instance state (outlives the ctx) and carries userdata=s so the camera
     * layer knows which instance's client to serve. The producer drives it via
     * RESOURCES_REQUEST (handled on the event thread). */
    if (camera_service_is_ready()) {
        s->camera_svc.type = SERVICE_TYPE_CAMERA;
        s->camera_svc.allocate_resource = camera_allocate_resource;
        s->camera_svc.free_resource = camera_free_resource;
        s->camera_svc.userdata = s;
        allocate_services(ctx, &s->camera_svc, 1);
    }

    pthread_mutex_lock(&s->input_lock);
    s->ctx = ctx;
    pthread_mutex_unlock(&s->input_lock);
    return 0;

fail:
    abort_display_session(ctx);
    disconnect(ctx);
    window_output_destroy(*output);
    *output = NULL;
    return -1;
}

static void notify_session_lost(struct consumer_state *s)
{
    LOGI("display session lost");

    /* Let the owning MainActivity probe the daemon socket and close the window if the
     * daemon is gone. onFallback() marshals itself to the UI thread on the Java side. */
    if (g_jvm && s->activity_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->activity_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "onFallback", "()V");
            if (mid)
                (*env)->CallVoidMethod(env, s->activity_obj, mid);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    /* Disable the clipboard listener for the lost session. */
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env)->CallVoidMethod(env, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}


static void destroy_session(struct consumer_state *s, struct window_output *output)
{
    /* The render thread is the only lifetime owner. Wake the event thread even
     * if it is reading a partial message, and stop audio before freeing ctx. */
    display_ctx *ctx = s->ctx;
    s->event_running = false;
    abort_display_session(ctx);
    join_event_thread(s);
    audio_stop(s->audio);
    topapp_restore_tree(s);
    pthread_mutex_lock(&s->input_lock);
    s->ctx = NULL;
    pthread_mutex_unlock(&s->input_lock);
    disconnect(ctx);
    /* A producer may retain a reference and finish writing this allocation.
     * Drop ours without reading or exporting it again; reconnect allocates anew. */
    window_output_destroy(output);
}

/* True requests the EGL fallback on a new connection with a fresh slot. */
static bool render_session(struct consumer_state *s, struct window_output *output, bool gpu_staging)
{
    struct presentation_queue queue = {0};
    uint64_t serial = 0;

    if (gpu_staging) {
        uint64_t deadline = 0;
        while (s->running) {
            int rc = wait_for_gpu_staging(s->ctx, FRAME_POLL_MS);
            if (rc == 0)
                break;
            if (rc < 0 && errno != EAGAIN)
                return true;
            /* Wait indefinitely for a producer to arrive. An older producer
             * can pick up FDs but never acknowledge the GPU staging offer. */
            if (rc == 1) {
                const uint64_t now = monotonic_now_ns();
                if (!now || (deadline && now >= deadline))
                    return true;
                if (!deadline)
                    deadline = now + GPU_ACK_TIMEOUT_NS;
            }
        }
        if (!s->running)
            return false;
        if (select_dmabuf_fenced(s->ctx, 0, -1) < 0) {
            LOGE("initial GPU staging handoff failed: %s", strerror(errno));
            return true;
        }
    } else {
        while (s->running && select_dmabuf(s->ctx, 0) < 0) {
            if (errno != EAGAIN) {
                LOGE("initial staging slot handoff failed: %s", strerror(errno));
                return false;
            }
            usleep(FRAME_POLL_MS * 1000);
        }
    }
    if (!s->running)
        return false;
    /* SCREEN seeds the connection. Resend the latest rate now that the data
     * channel is active, in case Android switched during the handshake. */
    send_refresh_rate(s);
    LOGI("display session active (%s); waiting for first producer completion",
         gpu_staging ? "Vulkan DMA-BUF + bidirectional SYNC_FD" : "EGL CPU upload");
    s->pacing.interval_start_ns = monotonic_now_ns();

    while (s->running) {
        poll_presentations(s, output, &queue);
        TracyCZoneN(zRefresh, "refresh_done (producer render)", 1);
        const uint64_t start = monotonic_now_ns();
        int fence = DISPLAY_FRAME_PENDING;
        while (s->running) {
            fence = refresh_done(s->ctx, FRAME_POLL_MS);
            if (fence != DISPLAY_FRAME_PENDING)
                break;
            const uint64_t now = monotonic_now_ns();
            if (!start || !now || now - start >= COMPLETION_TIMEOUT_NS)
                break;
            poll_presentations(s, output, &queue);
        }
        const uint64_t completion = monotonic_now_ns();
        TracyCZoneEnd(zRefresh);
        if (fence < -1) {
            if (s->running)
                LOGE("no safe render completion; retiring staging slot (%d)", fence);
            return gpu_staging && serial == 0;
        }
        if (start && completion >= start) {
            s->pacing.producer_waits++;
            pacing_add(&s->pacing.producer_wait, completion - start);
        }

        /* Serial counts producer completions, independently of presentation. */
        if (!s->running) {
            if (fence >= 0)
                close(fence);
            return false;
        }
        ++serial;
        uint64_t prepared;
        if (gpu_staging) {
            const uint64_t draw_start = monotonic_now_ns();
            int release = -1;
            const bool has_render_fence = fence >= 0;
            TracyCZoneN(zGpu, "Vulkan submit staging draw", 1);
            const int drawn = window_output_draw_gpu(output, fence, &release);
            TracyCZoneEnd(zGpu);
            prepared = monotonic_now_ns();
            if (drawn != 0) {
                LOGE("Vulkan staging draw failed; retiring slot");
                return true;
            }
            /* The next Linux write depends on this fence, not on the CPU or
             * BufferQueue. The WSI image has its own present semaphore. */
            const int selected = s->running ? select_dmabuf_fenced(s->ctx, 0, release) : -1;
            if (release >= 0)
                close(release);
            if (selected != 0) {
                if (s->running)
                    LOGE("cannot return the GPU staging slot: %s", strerror(errno));
                return false;
            }
            s->pacing.imported_fences += has_render_fence;
            s->pacing.release_fences += release >= 0;
            if (draw_start && prepared >= draw_start)
                pacing_add(&s->pacing.gpu_draw, prepared - draw_start);
        } else {
            if (fence >= 0 && !wait_render_fence(s, output, &queue, fence)) {
                if (s->running)
                    LOGE("render fence did not complete; retiring staging slot");
                return false;
            }
            if (!s->running)
                return false;

            const uint64_t upload_start = monotonic_now_ns();
            TracyCZoneN(zUpload, "window_output_upload", 1);
            const int uploaded = window_output_upload(output);
            TracyCZoneEnd(zUpload);
            prepared = monotonic_now_ns();
            if (uploaded != 0) {
                LOGE("staging texture upload failed; retiring staging slot");
                return false;
            }
            if (!s->running)
                return false;
            if (upload_start && prepared >= upload_start)
                pacing_add(&s->pacing.upload, prepared - upload_start);

            /* Draw/swap only reads the private GLES texture after upload. */
            if (release_dmabuf(s->ctx) != 0 || select_dmabuf(s->ctx, 0) != 0) {
                LOGE("cannot hand off the uploaded staging slot");
                return false;
            }
        }

        struct window_frame frame;
        TracyCZoneN(zSwap, "window_output_present", 1);
        int drawn = window_output_present(output, &frame);
        TracyCZoneEnd(zSwap);
        const uint64_t swapped = monotonic_now_ns();
        if (drawn != 0) {
            s->pacing.swap_failures++;
            pacing_report_if_due(s, swapped);
            LOGE("%s present failed; retiring display session", gpu_staging ? "Vulkan" : "EGL");
            /* A surface resize/out-of-date error gets a new swapchain while
             * the window is still usable. Failures before first present can
             * also indicate unsupported Vulkan WSI on this device. */
            return gpu_staging && serial == 1;
        }
        s->pacing.swaps++;
        if (serial == 1)
            LOGI("first %s present succeeded (presentation tracking=%d)",
                 gpu_staging ? "Vulkan" : "EGL", frame.tracked);
        if (prepared && swapped >= prepared)
            pacing_add(&s->pacing.draw_swap, swapped - prepared);
        pacing_report_if_due(s, swapped);
        track_presentation(s, &queue, frame, serial, swapped);
        if (serial == 1 && s->running)
            start_event_thread(s);
        TracyCFrameMark;
    }
    return false;
}

static void *render_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("render thread started");
    bool gpu_staging = true;

    while (s->running) {
        struct window_output *output = NULL;
        TracyCZoneN(zConnect, "do_connect", 1);
        int rc = do_connect(s, &output, gpu_staging);
        TracyCZoneEnd(zConnect);
        bool use_legacy = rc == -2;
        if (rc == 0) {
            use_legacy = render_session(s, output, gpu_staging);
            destroy_session(s, output);
            if (s->running && !use_legacy)
                notify_session_lost(s);
        }
        if (s->running && gpu_staging && use_legacy) {
            LOGI("GPU staging unavailable for this window; reconnecting with EGL and a fresh slot");
            gpu_staging = false;
            continue;
        }
        for (int i = 0; s->running && i < 25; ++i)
            usleep(20000);
    }

    ANativeWindow_release(s->window);
    s->window = NULL;
    LOGI("render thread stopped");
    return NULL;
}

/* s->lock serializes nativeStart/Stop/Destroy. The render thread never takes it. */
static void stop_render_thread(struct consumer_state *s)
{
    if (s->running) {
        s->running = false;
        pthread_join(s->render_thread, NULL);
    }
}

/* ---------- JNI ---------- */

static void copy_jstring(JNIEnv *env, jstring js, char *dst, size_t dstsz)
{
    if (!js) {
        dst[0] = '\0';
        return;
    }
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    if (s) {
        strncpy(dst, s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        (*env)->ReleaseStringUTFChars(env, js, s);
    } else {
        dst[0] = '\0';
    }
}

/* Every JNI entry point below takes a jlong handle -- the consumer_state* returned
 * by nativeCreate -- so multiple instances (windows) coexist in one process. */
#define STATE(h) ((struct consumer_state *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_anland_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer_state *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->input_lock, NULL);
    pthread_mutex_init(&s->cfg_lock, NULL);
    pthread_mutex_init(&s->topapp_lock, NULL);
    atomic_init(&s->running, false);
    atomic_init(&s->event_running, false);
    atomic_init(&s->refresh_mhz, 0);
    s->cfg_topapp_mode = 1;
    strncpy(s->cfg_socket_path, "/data/local/tmp/display_daemon.sock",
            sizeof(s->cfg_socket_path) - 1);
    s->audio = audio_create();
    LOGI("instance %p created", (void *)s);
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    /* Stop the transport (mirrors nativeStop), release the camera client + audio
     * bridge, then free. */
    pthread_mutex_lock(&s->lock);
    stop_render_thread(s);
    pthread_mutex_unlock(&s->lock);

    audio_destroy(s->audio);
    s->audio = NULL;
    camera_release_client(s);   /* window gone: tear down its camera channels */

    if (s->clipboard_obj && g_jvm) {
        JNIEnv *e = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL) == 0);
        if (e) {
            (*e)->DeleteGlobalRef(e, s->clipboard_obj);
            if (s->activity_obj)
                (*e)->DeleteGlobalRef(e, s->activity_obj);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    s->activity_obj = NULL;
    pthread_mutex_destroy(&s->topapp_lock);
    pthread_mutex_destroy(&s->cfg_lock);
    pthread_mutex_destroy(&s->input_lock);
    pthread_mutex_destroy(&s->lock);
    LOGI("instance %p destroyed", (void *)s);
    free(s);
}

/* Mark this instance focused (real camera frames go to the focused instance; others
 * get blank frames). Called from Java on window focus gain. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetFocused(
    JNIEnv *env, jclass clazz, jlong handle, jboolean focused)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s && focused)
        camera_set_focus(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeConfigure(
    JNIEnv *env, jclass clazz, jlong handle, jstring socketPath, jboolean useRoot,
    jstring helperPath, jstring bridgePath, jboolean topappEnable, jstring topappPath,
    jint topappMode, jstring topappStops)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    char tmp[sizeof(s->cfg_socket_path)];
    copy_jstring(env, socketPath, tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        memcpy(s->cfg_socket_path, tmp, sizeof(s->cfg_socket_path));
    s->cfg_use_root = (useRoot == JNI_TRUE);
    copy_jstring(env, helperPath, s->cfg_helper_path, sizeof(s->cfg_helper_path));
    copy_jstring(env, bridgePath, s->cfg_bridge_path, sizeof(s->cfg_bridge_path));
    s->cfg_topapp_enable = (topappEnable == JNI_TRUE);
    copy_jstring(env, topappPath, s->cfg_topapp_path, sizeof(s->cfg_topapp_path));
    s->cfg_topapp_mode = topappMode == 2 ? 2 : 1;
    copy_jstring(env, topappStops, s->cfg_topapp_stops,
                 sizeof(s->cfg_topapp_stops));
    for (char *p = s->cfg_topapp_stops; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            s->cfg_topapp_stops[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("configured: socket=%s root=%d helper=%s bridge=%s topapp=%d mode=%d stops=%s",
         s->cfg_socket_path, s->cfg_use_root, s->cfg_helper_path, s->cfg_bridge_path,
         s->cfg_topapp_enable, s->cfg_topapp_mode, s->cfg_topapp_stops);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetCustomResolution(
    JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    s->cfg_custom_width = width;
    s->cfg_custom_height = height;
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("custom resolution: %dx%d", width, height);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetScreenSize(
    JNIEnv *env, jclass clazz, jlong handle, jint width, jint height)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    s->cfg_screen_width = width;
    s->cfg_screen_height = height;
    pthread_mutex_unlock(&s->cfg_lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle, jobject surface, jobject clipboardTarget,
    jobject activityTarget)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->lock);

    stop_render_thread(s);
    s->motion_has_last = false;

    s->window = ANativeWindow_fromSurface(env, surface);
    if (!s->window) {
        LOGE("ANativeWindow_fromSurface failed");
        pthread_mutex_unlock(&s->lock);
        return;
    }

    /* Save JVM (process-global) and this instance's clipboard callback target. */
    if (!g_jvm) {
        (*env)->GetJavaVM(env, &g_jvm);
    }
    if (s->clipboard_obj) {
        (*env)->DeleteGlobalRef(env, s->clipboard_obj);
    }
    /* Static natives have no `thiz`; the Java layer passes the object whose
     * nativeSetClipboardText / nativeClipListening / nativeClipboardSync the
     * event thread calls back into (the Clipboard instance). */
    s->clipboard_obj = (*env)->NewGlobalRef(env, clipboardTarget);

    /* Owning MainActivity for display-session loss notification. */
    if (s->activity_obj) {
        (*env)->DeleteGlobalRef(env, s->activity_obj);
    }
    s->activity_obj = activityTarget ? (*env)->NewGlobalRef(env, activityTarget) : NULL;

    s->running = true;
    if (pthread_create(&s->render_thread, NULL, render_thread_func, s) != 0) {
        LOGE("could not start render thread");
        s->running = false;
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->lock);

    stop_render_thread(s);

    // Disable clip listener on Java side
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env2 = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env2, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env2, NULL) == 0)
                attached = true;
        }
        if (env2) {
            jclass cls = (*env2)->GetObjectClass(env2, s->clipboard_obj);
            jmethodID mid = (*env2)->GetMethodID(env2, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env2)->CallVoidMethod(env2, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetRefreshRate(
    JNIEnv *env, jclass clazz, jlong handle, jfloat hz)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !(hz >= 1.0f && hz <= 1000.0f))
        return;
    const uint32_t mhz = (uint32_t)(hz * 1000.0f + 0.5f);
    if (atomic_exchange(&s->refresh_mhz, mhz) != mhz)
        send_refresh_rate(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouch(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jfloat x, jfloat y, jint pointer_id)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action, .x = x, .y = y, .pointer_id = pointer_id },
    };
    send_input_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH_FRAME,
    };
    send_input_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendKey(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jint keycode)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = { .action = action, .keycode = keycode },
    };
    send_input_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseMotion(
    JNIEnv *env, jclass clazz, jlong handle, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->input_lock);
    if (!s->ctx) {
        pthread_mutex_unlock(&s->input_lock);
        return;
    }

    if (dx == 0.0f && dy == 0.0f && s->motion_has_last) {
        dx = x - s->motion_last_x;
        dy = y - s->motion_last_y;
    }

    s->motion_last_x = x;
    s->motion_last_y = y;
    s->motion_has_last = true;

    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy },
    };
    push_input_event(s->ctx, &ev);
    pthread_mutex_unlock(&s->input_lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseButton(
    JNIEnv *env, jclass clazz, jlong handle, jint button, jboolean pressed)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed ? 1 : 0 },
    };
    send_input_event(s, &ev);
}



JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseScroll(
    JNIEnv *env, jclass clazz, jlong handle, jint axis, jfloat value)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    send_input_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTabletSample(
    JNIEnv *env, jclass clazz, jlong handle, jint tool_id, jint tool_type,
    jint capabilities, jint device_id, jfloat x, jfloat y, jfloat pressure,
    jfloat tilt_x, jfloat tilt_y, jfloat distance, jint flags, jint buttons,
    jlong time_msec)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    uint32_t frame_flags = (uint32_t)flags;
    uint32_t frame_buttons = (uint32_t)buttons;
    if (frame_flags & TABLET_FLAG_CANCELED) {
        frame_flags = TABLET_FLAG_CANCELED;
        frame_buttons = 0;
        pressure = 0.0f;
    } else if (!(frame_flags & TABLET_FLAG_PROXIMITY)) {
        frame_flags = 0;
        frame_buttons = 0;
        pressure = 0.0f;
    }

    struct InputEvent tool = {
        .type = INPUT_TYPE_TABLET_TOOL,
        .tablet_tool = {
            .tool_id = (uint32_t)tool_id,
            .tool_type = (uint32_t)tool_type,
            .capabilities = (uint32_t)capabilities,
            .device_id = (uint32_t)device_id,
        },
    };
    struct InputEvent position = {
        .type = INPUT_TYPE_TABLET_POSITION,
        .tablet_position = {
            .tool_id = (uint32_t)tool_id,
            .x = x,
            .y = y,
            .pressure = pressure,
        },
    };
    struct InputEvent tilt = {
        .type = INPUT_TYPE_TABLET_TILT,
        .tablet_tilt = {
            .tool_id = (uint32_t)tool_id,
            .tilt_x = tilt_x,
            .tilt_y = tilt_y,
            .distance = distance,
        },
    };
    struct InputEvent frame = {
        .type = INPUT_TYPE_TABLET_FRAME,
        .tablet_frame = {
            .tool_id = (uint32_t)tool_id,
            .flags = frame_flags,
            .buttons = frame_buttons,
            .time_msec = (uint32_t)time_msec,
        },
    };

    pthread_mutex_lock(&s->input_lock);
    if (s->ctx) {
        /* Repeat TOOL metadata so a new connection always sees it before axes. */
        if (push_input_event(s->ctx, &tool) == 0
                && push_input_event(s->ctx, &position) == 0
                && push_input_event(s->ctx, &tilt) == 0)
            push_input_event(s->ctx, &frame); /* FRAME commits this sample. */
    }
    pthread_mutex_unlock(&s->input_lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendClipboard(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = (uint32_t)len },
    };
    send_input_payload(s, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTextInput(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_TEXT_INPUT,
        .text_input = { .size = (uint32_t)len },
    };
    send_input_payload(s, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetMicEnabled(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_mic_enabled(s->audio, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioLatency(
    JNIEnv *env, jclass clazz, jlong handle, jint speakerMs, jint micMs)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_latency(s->audio, speakerMs, micMs);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioKeepalive(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_keepalive(s->audio, enabled == JNI_TRUE);
}
