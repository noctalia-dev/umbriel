#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include "umbrielfx/types/wlr_scene.h"

struct wlr_scene* scene_node_get_root(struct wlr_scene_node* node);

void scene_node_get_size(struct wlr_scene_node* node, int* width, int* height);

void scene_presentation_bounds(struct wlr_scene_node* node, struct wlr_box* box);

void scene_surface_set_clip(struct wlr_scene_surface* surface, struct wlr_box* clip);

void scene_surface_set_scale(struct wlr_scene_surface* surface, double scale);

void scene_node_update_surface_opacity(struct wlr_scene_node* node);

void scene_subsurface_tree_set_point_accepts_input(
    struct wlr_scene_tree* tree, wlr_scene_buffer_point_accepts_input_func_t point_accepts_input
);

#endif
