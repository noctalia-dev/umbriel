#ifndef UMBRIELFX_TYPES_SCENE_CAPTURE_H
#define UMBRIELFX_TYPES_SCENE_CAPTURE_H

#include <wlr/types/wlr_ext_image_capture_source_v1.h>

struct wlr_scene_node;
struct wlr_scene_buffer;
struct wlr_scene_output;
struct wlr_scene;

/**
 * Immediately reject new copies, then stop sessions and destroy the owned
 * source at a safe event-loop boundary. The scene remains alive. NULL is accepted.
 */
void umbrielfx_scene_capture_source_revoke(struct wlr_ext_image_capture_source_v1* source);

/** Exclude a node's descendants from capture without changing local visibility. */
bool umbrielfx_scene_node_set_capture_excluded(struct wlr_scene_node* node, bool excluded);
bool umbrielfx_scene_node_capture_excluded(struct wlr_scene_node* node);
bool umbrielfx_scene_buffer_captured(struct wlr_scene_buffer* buffer, struct wlr_scene_output* output);
/** Share the owner's policy; retained nodes keep its last state after it dies. */
bool umbrielfx_scene_node_share_capture_policy(struct wlr_scene_node* node, struct wlr_scene_node* owner);
bool umbrielfx_scene_has_capture_exclusion(struct wlr_scene* scene);

/**
 * Set the render scale of a source created with
 * wlr_ext_image_capture_source_v1_create_with_scene_node().
 * The scale must be finite and positive. Active sessions receive updated buffer
 * constraints and a new frame; idle sources do not render until started.
 */
void umbrielfx_scene_capture_source_set_scale(struct wlr_ext_image_capture_source_v1* source, float scale);

#endif
