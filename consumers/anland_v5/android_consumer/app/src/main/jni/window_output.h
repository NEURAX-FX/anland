#ifndef WINDOW_OUTPUT_H
#define WINDOW_OUTPUT_H

#include <android/native_window.h>
#include <stdbool.h>
#include <stdint.h>

struct buf_info;
struct window_output;

struct window_frame {
    uint64_t id;
    bool tracked;
};

/* Create, upload, present, query and destroy on the same dedicated render thread.
 * The window must not be connected to another buffer producer. Width and
 * height describe the staging slot; the EGL surface uses the window size. */
struct window_output *window_output_create(ANativeWindow *window, int width, int height);
/* This backend never maps the staging slot. NULL means use a new legacy
 * connection and a new slot if the window or device cannot support it. */
struct window_output *window_output_create_gpu(ANativeWindow *window, int width, int height);
/* GPU output only. Consumes the producer fd on all paths. On success the
 * caller owns *release_fd (-1 if already complete) and can return the slot. */
int window_output_draw_gpu(struct window_output *output, int producer_fd, int *release_fd);
void window_output_destroy(struct window_output *output);

/* Fill the staging slot layout and return its borrowed FD, or -1 on error.
 * The FD remains owned by output until destroy. */
int window_output_get_buffer(const struct window_output *output, struct buf_info *info);

/* The producer's render fence must be complete and the staging slot held
 * exclusively until a successful upload returns 0. On error return -1. */
int window_output_upload(struct window_output *output);

/* Publish the preceding GPU draw, or draw/publish the legacy upload. Neither
 * path needs to retain the slot past its release. 0 on success, -1 on error. */
int window_output_present(struct window_output *output, struct window_frame *frame);

/* Poll an ID from a tracked frame of this output. Returns 1 and writes its
 * actual monotonic DISPLAY_PRESENT time in ns, 0 if pending, or -1 if
 * unavailable/invalid. Leaves *ns unchanged unless it returns 1. */
int window_output_presentation_time(struct window_output *output, uint64_t id, uint64_t *ns);

#endif
