// Guards the scene struct layout: every field wlroots declares must sit at the
// same offset in umbrielfx's version. umbrielfx allocates every node through its
// own vendored helpers, so this keeps a helper that ever resolves to libwlroots
// from corrupting memory before the startup guard catches it.
//
// Built twice, once against each header, and the two outputs are compared.
// The same field list compiles on both sides, so a field wlroots adds that
// umbrielfx lacks breaks the build rather than the renderer.

#include <stddef.h>
#include <stdio.h>

#include ABI_PROBE_HEADER

#define OFF(s, f) printf("offsetof(struct %s, %s) = %zu\n", #s, #f, offsetof(struct s, f))
#define SIZE(s) printf("sizeof(struct %s) >= %zu\n", #s, sizeof(struct s))

int main(void) {
	// wlr_scene_node
	OFF(wlr_scene_node, type);
	OFF(wlr_scene_node, parent);
	OFF(wlr_scene_node, link);
	OFF(wlr_scene_node, enabled);
	OFF(wlr_scene_node, x);
	OFF(wlr_scene_node, y);
	OFF(wlr_scene_node, events);
	OFF(wlr_scene_node, data);
	OFF(wlr_scene_node, addons);
	OFF(wlr_scene_node, visible);
	SIZE(wlr_scene_node);

	// wlr_scene_tree
	OFF(wlr_scene_tree, node);
	OFF(wlr_scene_tree, children);
	SIZE(wlr_scene_tree);

	// wlr_scene_rect
	OFF(wlr_scene_rect, node);
	OFF(wlr_scene_rect, width);
	OFF(wlr_scene_rect, height);
	OFF(wlr_scene_rect, color);
	SIZE(wlr_scene_rect);

	// wlr_scene_buffer
	OFF(wlr_scene_buffer, node);
	OFF(wlr_scene_buffer, buffer);
	OFF(wlr_scene_buffer, events);
	OFF(wlr_scene_buffer, point_accepts_input);
	OFF(wlr_scene_buffer, primary_output);
	OFF(wlr_scene_buffer, opacity);
	OFF(wlr_scene_buffer, filter_mode);
	OFF(wlr_scene_buffer, src_box);
	OFF(wlr_scene_buffer, dst_width);
	OFF(wlr_scene_buffer, dst_height);
	OFF(wlr_scene_buffer, transform);
	OFF(wlr_scene_buffer, opaque_region);
	OFF(wlr_scene_buffer, transfer_function);
	OFF(wlr_scene_buffer, primaries);
	OFF(wlr_scene_buffer, color_encoding);
	OFF(wlr_scene_buffer, color_range);
	OFF(wlr_scene_buffer, active_outputs);
	OFF(wlr_scene_buffer, texture);
	OFF(wlr_scene_buffer, prev_feedback_options);
	OFF(wlr_scene_buffer, own_buffer);
	OFF(wlr_scene_buffer, buffer_width);
	OFF(wlr_scene_buffer, buffer_height);
	OFF(wlr_scene_buffer, buffer_is_opaque);
	OFF(wlr_scene_buffer, wait_timeline);
	OFF(wlr_scene_buffer, wait_point);
	OFF(wlr_scene_buffer, buffer_release);
	OFF(wlr_scene_buffer, renderer_destroy);
	OFF(wlr_scene_buffer, is_single_pixel_buffer);
	OFF(wlr_scene_buffer, single_pixel_buffer_color);
	SIZE(wlr_scene_buffer);

	// wlr_scene_output
	OFF(wlr_scene_output, output);
	OFF(wlr_scene_output, link);
	OFF(wlr_scene_output, scene);
	OFF(wlr_scene_output, addon);
	OFF(wlr_scene_output, damage_ring);
	OFF(wlr_scene_output, x);
	OFF(wlr_scene_output, y);
	OFF(wlr_scene_output, events);
	OFF(wlr_scene_output, pending_commit_damage);
	OFF(wlr_scene_output, index);
	OFF(wlr_scene_output, dmabuf_feedback_debounce);
	OFF(wlr_scene_output, prev_scanout);
	OFF(wlr_scene_output, gamma_lut_changed);
	OFF(wlr_scene_output, gamma_lut);
	OFF(wlr_scene_output, gamma_lut_color_transform);
	OFF(wlr_scene_output, prev_gamma_lut_color_transform);
	OFF(wlr_scene_output, prev_supplied_color_transform);
	OFF(wlr_scene_output, combined_color_transform);
	OFF(wlr_scene_output, output_commit);
	OFF(wlr_scene_output, output_damage);
	OFF(wlr_scene_output, output_needs_frame);
	OFF(wlr_scene_output, damage_highlight_regions);
	OFF(wlr_scene_output, render_list);
	OFF(wlr_scene_output, in_timeline);
	OFF(wlr_scene_output, in_point);
	OFF(wlr_scene_output, out_timeline);
	OFF(wlr_scene_output, out_point);
	SIZE(wlr_scene_output);

	// wlr_scene
	OFF(wlr_scene, tree);
	OFF(wlr_scene, outputs);
	OFF(wlr_scene, linux_dmabuf_v1);
	OFF(wlr_scene, gamma_control_manager_v1);
	OFF(wlr_scene, color_manager_v1);
	OFF(wlr_scene, restack_xwayland_surfaces);
	OFF(wlr_scene, linux_dmabuf_v1_destroy);
	OFF(wlr_scene, gamma_control_manager_v1_destroy);
	OFF(wlr_scene, gamma_control_manager_v1_set_gamma);
	OFF(wlr_scene, color_manager_v1_destroy);
	OFF(wlr_scene, debug_damage_option);
	OFF(wlr_scene, direct_scanout);
	OFF(wlr_scene, calculate_visibility);
	OFF(wlr_scene, highlight_transparent_region);
	SIZE(wlr_scene);

	return 0;
}
