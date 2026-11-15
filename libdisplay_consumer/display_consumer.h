#ifndef DISPLAY_CONSUMER_H
#define DISPLAY_CONSUMER_H

#include <stdint.h>
#include "../common/protocol.h"

typedef struct display_ctx display_ctx;
/* Takes ownership even on failure. The socket creator configures its send
 * timeout before transfer; SELinux can deny setopt on a delegated socket. */
int connect_to_deamon_with_fd(display_ctx **out, int ctrl_fd);
int  connect_to_deamon(display_ctx **ctx, const char *socket_path);
/* Offer the optional GPU staging protocol on a fresh connection. The scoped
 * producer must acknowledge before the consumer may hand it a slot. */
int connect_to_deamon_gpu(display_ctx **ctx, const char *socket_path);
int connect_to_deamon_with_fd_gpu(display_ctx **ctx, int ctrl_fd);
/* 0: ACK received; 1: FDS_READY received but ACK pending; -1/EAGAIN:
 * FDS_READY pending. Other errors are terminal. At most timeout_ms per call. */
int wait_for_gpu_staging(display_ctx *ctx, int timeout_ms);
void disconnect(display_ctx *ctx);
/* Terminal for this context: wakes socket users without closing their fds.
 * Callers must stop those users before disconnect(). */
void abort_display_session(display_ctx *ctx);
int  set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh);
/* Export once per context. Fds are borrowed for the duration of the call. */
int  push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count);
/* Zero on handoff; -1/EAGAIN only while waiting for the first FDS_READY. */
int  select_dmabuf(display_ctx *ctx, int idx);
/* GPU mode only: select the initial slot (-1) or return the completed slot
 * with its GPU release dependency (borrowed fd, -1 if already complete). */
int select_dmabuf_fenced(display_ctx *ctx, int idx, int release_fd);
/* >= 0 transfers a fence fd, -1 indicates synchronous completion.
 * A poll timeout leaves the slot outstanding; FAILED never licenses a read. */
#define DISPLAY_FRAME_FAILED (-2)
#define DISPLAY_FRAME_PENDING (-3)
int  refresh_done(display_ctx *ctx, int timeout_ms);
/* Release a completed slot only after the caller has finished reading it. */
int  release_dmabuf(display_ctx *ctx);
int  push_input_event(display_ctx *ctx, const struct InputEvent *event);
/* For advisory input: 1 sent, 0 skipped without writing, -1 session lost. */
int  try_push_input_event(display_ctx *ctx, const struct InputEvent *event);
int  push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size);
int  poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms);
int  poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms);
int  get_data_fd(display_ctx *ctx);
int  get_audio_fd(display_ctx *ctx);
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event);


struct resources {
    uint32_t service_type;
    int32_t type;
    uint32_t num;
    int* fds;
};
struct service_info {
    uint32_t type;
    //资源分配函数指针
    struct resources (*allocate_resource)(uint32_t* args, void* userdata);//only support 3 args
    void (*free_resource)(struct resources res, void* userdata);
    //透传给上面两个回调的实例上下文(多实例时用于区分是哪个连接;单实例可为 NULL)
    void* userdata;
};
typedef struct service_info service_info;
typedef struct resources resources;
void allocate_services(struct display_ctx *ctx, struct service_info *services, int num_services);
void handle_resource_request(struct display_ctx *ctx, struct OutputEvent *event);
void free_resources(struct display_ctx *ctx);//释放资源，保留服务信息
#endif
