#ifndef UMBRIELFX_RENDER_ANIMATION_HISTORY_H
#define UMBRIELFX_RENDER_ANIMATION_HISTORY_H

#include <wayland-server-core.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_output;
struct fx_renderer;
struct wlr_allocator;

struct fx_animation_history {
  struct wl_list outputs;
};

void fx_animation_history_init(struct fx_animation_history* history);
void fx_animation_history_finish(struct fx_animation_history* history);
void fx_animation_history_reset(struct fx_animation_history* history);
// Destroys the entries of `role` on `output`; a NULL output matches every output.
void fx_animation_history_reset_role(struct fx_animation_history* history, struct wlr_output* output, unsigned role);
void fx_animation_history_move(struct fx_animation_history* destination, struct fx_animation_history* source);

// Internal source acquisition: copy both role histories before any role renders.
// Destination must be initialized and empty. Failure rolls it back atomically.
uint64_t fx_animation_history_bytes(const struct fx_animation_history* history,
    struct wlr_output* output, struct fx_renderer* renderer);
bool fx_animation_history_clone(struct fx_animation_history* destination,
    const struct fx_animation_history* source, struct wlr_output* output,
    struct fx_renderer* renderer, struct wlr_allocator* allocator);
// Finish updates deferred by a source pass only after its final output submit.
void fx_animation_history_finish_updates(struct wl_list* updates, bool success);

#endif
