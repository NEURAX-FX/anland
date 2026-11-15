#ifndef VULKAN_OUTPUT_H
#define VULKAN_OUTPUT_H

#include "window_output.h"

struct vulkan_output;

/* Use all operations on the render thread. The window must not already have
 * a buffer producer. The LINEAR RGBA8 staging DMA-BUF fd is borrowed; Vulkan
 * imports its own duplicate. Returns NULL if the output cannot be created. */
struct vulkan_output *vulkan_output_create(ANativeWindow *window, int staging_fd,
                                           const struct buf_info *info);
void vulkan_output_destroy(struct vulkan_output *output);

/* Consume producer_fence_fd on all paths (-1 means already signaled). On
 * success, submit the fullscreen draw and return 0 with an owned release
 * sync-file (or -1 if already signaled) in *release_fence_fd. On error return
 * -1 without writing it; the caller must not return the slot as released. */
int vulkan_output_draw(struct vulkan_output *output, int producer_fence_fd,
                       int *release_fence_fd);

/* Present the image from the preceding successful draw. Call once per draw;
 * a Vulkan draw or present error requires destroying this output. */
int vulkan_output_present(struct vulkan_output *output, struct window_frame *frame);

/* Poll a tracked frame's actual monotonic presentation timestamp: 1 with
 * *ns set, 0 while pending, or -1 if unavailable. */
int vulkan_output_presentation_time(struct vulkan_output *output, uint64_t id,
                                    uint64_t *ns);

#endif
