#ifndef UMBRIELFX_ANIMATION_H
#define UMBRIELFX_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct wlr_renderer;
struct wlr_scene_node;
struct wlr_scene_shadow;
struct fx_animation_shader;

#define FX_ANIMATION_SLOTS 9
#define FX_ANIMATION_DEPTH 24

struct fx_animation_parameters {
  float progress;
  float linear_progress;
  float direction;
  // Nonzero and unique for each logical transition, stable while it runs.
  uint64_t transition_id;
  // Stable values in [0, 1) for the lifetime of transition_id.
  float random_seed[4];
};

// Compilation happens with the renderer's context current. Sources provide
// vec4 animation(vec2 uv), not a main function or version declaration.
struct fx_animation_shader*
fx_animation_shader_create(struct wlr_renderer* renderer, const char* source, const char* label);
struct fx_animation_shader* fx_animation_shader_ref(struct fx_animation_shader* shader);
void fx_animation_shader_unref(struct fx_animation_shader* shader);
// A shape-preserving shader only scales its input's alpha uniformly. Shadows
// keep their analytic fast path under it instead of capturing a silhouette.
void fx_animation_shader_set_shape_preserving(struct fx_animation_shader* shader, bool shape_preserving);

// Slots compose in ascending order, then through effect-bearing ancestors.
// A NULL shader removes a slot. Nodes hold their own reference to the program.
void wlr_scene_node_set_animation(
    struct wlr_scene_node* node, unsigned slot, struct fx_animation_shader* shader,
    const struct fx_animation_parameters* parameters
);
void wlr_scene_node_clear_animations(struct wlr_scene_node* node);
// Limit only the final animation composite in node-local coordinates. The
// complete subtree remains available to the shader and feedback history.
// A NULL box removes the clip. An empty box keeps the shader and feedback
// history running without compositing pixels. Returns false when the node has
// no animation composite, so the caller can apply an ordinary scene-tree clip.
bool wlr_scene_node_set_animation_output_clip(struct wlr_scene_node* node, const struct wlr_box* box);
// Freeze current parameters into a snapshot and transfer feedback history from
// the source that is about to be retired. Outer lifecycle effects become inner
// opening effects so the new close transition can use its normal slot.
void wlr_scene_node_copy_animations_for_snapshot(struct wlr_scene_node* destination, struct wlr_scene_node* source);

// Keep the shadow in its stacking layer, but derive its animated silhouette
// from source. Color is the unattenuated shadow color; source alpha supplies
// opacity. The association is automatically cleared when either node dies.
void wlr_scene_shadow_set_animation_source(
    struct wlr_scene_shadow* shadow, struct wlr_scene_node* source, const float color[4]
);

#endif
