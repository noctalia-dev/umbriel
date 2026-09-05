#ifndef UMBRIELFX_ANIMATION_H
#define UMBRIELFX_ANIMATION_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct wlr_renderer;
struct wlr_scene_node;
struct fx_animation_shader;

#define FX_ANIMATION_SLOTS 9
#define FX_ANIMATION_DEPTH 24

struct fx_animation_parameters {
	float progress;
	float linear_progress;
	float direction;
};

// Compilation happens with the renderer's context current. Sources provide
// vec4 animation(vec2 uv), not a main function or version declaration.
struct fx_animation_shader *fx_animation_shader_create(struct wlr_renderer *renderer,
	const char *source, const char *label);
struct fx_animation_shader *fx_animation_shader_ref(struct fx_animation_shader *shader);
void fx_animation_shader_unref(struct fx_animation_shader *shader);

// Slots compose in ascending order, then through effect-bearing ancestors.
// A NULL shader removes a slot. Nodes hold their own reference to the program.
void wlr_scene_node_set_animation(struct wlr_scene_node *node, unsigned slot,
	struct fx_animation_shader *shader, const struct fx_animation_parameters *parameters);
void wlr_scene_node_clear_animations(struct wlr_scene_node *node);
// Freeze current parameters into a snapshot. Outer lifecycle effects become
// inner opening effects so the new close transition can use its normal slot.
void wlr_scene_node_copy_animations(struct wlr_scene_node *destination, struct wlr_scene_node *source);

#endif
