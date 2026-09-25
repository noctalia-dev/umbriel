#ifndef UMBRIELFX_RENDER_ANIMATION_HISTORY_H
#define UMBRIELFX_RENDER_ANIMATION_HISTORY_H

#include <pixman.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

struct fx_animation_parameters;
struct fx_animation_shader;
struct fx_gles_render_pass;
struct wlr_box;
struct wlr_output;

struct fx_animation_history {
  struct wl_list outputs;
};

void fx_animation_history_init(struct fx_animation_history* history);
void fx_animation_history_finish(struct fx_animation_history* history);
void fx_animation_history_reset(struct fx_animation_history* history);
void fx_animation_history_move(struct fx_animation_history* destination, struct fx_animation_history* source);

void fx_render_pass_end_animation_with_history(
    struct fx_gles_render_pass* pass, struct fx_animation_shader* shader,
    const struct fx_animation_parameters* parameters, const struct wlr_box* box, const struct wlr_box* logical_box,
    enum wl_output_transform transform, const pixman_region32_t* capture_clip, const pixman_region32_t* output_clip,
    struct fx_animation_history* history, struct wlr_output* output, bool update_history
);

#endif
