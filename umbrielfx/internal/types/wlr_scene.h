#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include <wlr/types/wlr_scene.h>

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height);

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip);

void scene_surface_set_scale(struct wlr_scene_surface *surface, double scale);

void scene_subsurface_tree_set_point_accepts_input(struct wlr_scene_tree *tree,
		wlr_scene_buffer_point_accepts_input_func_t point_accepts_input);

struct fx_effect_shader;
struct fx_animation_parameters;
// Synchronous composition-only uniform rebind. Program and every non-uniform
// field must match the installed slot; otherwise returns false for the normal
// setter. damage marks only this output without scheduling a new frame; the
// caller sets it only when this output has pending input. Geometry/history stay intact.
bool wlr_scene_node_set_animation_uniforms_for_output(struct wlr_scene_node *node,
	unsigned slot, struct fx_effect_shader *shader,
	const struct fx_animation_parameters *parameters, struct wlr_scene_output *output, bool damage);

#endif
