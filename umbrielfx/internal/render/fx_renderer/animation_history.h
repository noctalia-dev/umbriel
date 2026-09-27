#ifndef UMBRIELFX_RENDER_ANIMATION_HISTORY_H
#define UMBRIELFX_RENDER_ANIMATION_HISTORY_H

#include <wayland-server-core.h>

struct wlr_output;

struct fx_animation_history {
  struct wl_list outputs;
};

void fx_animation_history_init(struct fx_animation_history* history);
void fx_animation_history_finish(struct fx_animation_history* history);
void fx_animation_history_reset(struct fx_animation_history* history);
// Destroys the entries of `role` on `output`; a NULL output matches every output.
void fx_animation_history_reset_role(struct fx_animation_history* history, struct wlr_output* output, unsigned role);
void fx_animation_history_move(struct fx_animation_history* destination, struct fx_animation_history* source);

#endif
