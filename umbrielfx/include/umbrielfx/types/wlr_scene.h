/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_SCENE_H
#define WLR_TYPES_WLR_SCENE_H

/**
 * The scene-graph API provides a declarative way to display surfaces. The
 * compositor creates a scene, adds surfaces, then renders the scene on
 * outputs.
 *
 * The scene-graph API only supports basic 2D composition operations (like the
 * KMS API or the Wayland protocol does). For anything more complicated,
 * compositors need to implement custom rendering logic.
 */

#include <pixman.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

#include "umbrielfx/types/fx/blur_data.h"
#include "umbrielfx/types/fx/clipped_region.h"
#include "umbrielfx/types/linked_node.h"

struct wlr_output;
struct wlr_output_layout;
struct wlr_output_layout_output;
struct wlr_xdg_surface;
struct wlr_layer_surface_v1;
struct wlr_drag_icon;
struct wlr_surface;

struct wlr_scene_node;
struct wlr_scene_buffer;
struct wlr_scene_output_layout;

struct wlr_presentation;
struct wlr_linux_dmabuf_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_color_manager_v1;
struct wlr_output_state;

typedef bool (*wlr_scene_buffer_point_accepts_input_func_t)(
	struct wlr_scene_buffer *buffer, double *sx, double *sy);

typedef void (*wlr_scene_buffer_iterator_func_t)(
	struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data);

enum wlr_scene_node_type {
	// Keep wlroots node values stable because its separately compiled scene
	// helpers inspect nodes created by SceneFX.
	WLR_SCENE_NODE_TREE = 0,
	WLR_SCENE_NODE_RECT = 1,
	WLR_SCENE_NODE_BUFFER = 2,
	WLR_SCENE_NODE_BORDER,
	WLR_SCENE_NODE_SHADOW,
	WLR_SCENE_NODE_OPTIMIZED_BLUR,
	WLR_SCENE_NODE_BLUR,
};

/** A node is an object in the scene. */
struct wlr_scene_node {
	enum wlr_scene_node_type type;
	struct wlr_scene_tree *parent;

	struct wl_list link; // wlr_scene_tree.children

	bool enabled;
	int x, y; // relative to parent

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct wlr_addon_set addons;

	struct {
		pixman_region32_t visible;
	} WLR_PRIVATE;
};

enum wlr_scene_debug_damage_option {
	WLR_SCENE_DEBUG_DAMAGE_NONE,
	WLR_SCENE_DEBUG_DAMAGE_RERENDER,
	WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT
};

/**
 * A sub-tree in the scene-graph.
 *
 * Do not add members here. struct wlr_scene embeds this struct by value, so its
 * size decides the offset of every following wlr_scene member. Compositors link
 * scene helpers from wlroots proper (wlr_scene_subsurface_tree_create,
 * wlr_scene_xdg_surface_create), and those were compiled against wlroots' own
 * definition. Growing this struct silently repoints their reads of
 * wlr_scene.outputs and friends. Per-tree state goes in a wlr_addon on
 * wlr_scene_tree.node instead, as wlr_scene_tree_set_clip does.
 */
struct wlr_scene_tree {
	struct wlr_scene_node node;

	struct wl_list children; // wlr_scene_node.link
};

/** The root scene-graph node. */
struct wlr_scene {
	struct wlr_scene_tree tree;

	struct wl_list outputs; // wlr_scene_output.link

	// May be NULL
	struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager_v1;
	struct wlr_color_manager_v1 *color_manager_v1;

	bool restack_xwayland_surfaces;

	struct {
		struct wl_listener linux_dmabuf_v1_destroy;
		struct wl_listener gamma_control_manager_v1_destroy;
		struct wl_listener gamma_control_manager_v1_set_gamma;
		struct wl_listener color_manager_v1_destroy;

		enum wlr_scene_debug_damage_option debug_damage_option;
		bool direct_scanout;
		bool calculate_visibility;
		bool highlight_transparent_region;

		struct blur_data blur_data;
	} WLR_PRIVATE;
};

/** A scene-graph node displaying a single surface. */
struct wlr_scene_surface {
	struct wlr_scene_buffer *buffer;
	struct wlr_surface *surface;

	struct {
		struct wlr_box clip;

		struct wlr_addon addon;

		struct wl_listener outputs_update;
		struct wl_listener output_sample;
		struct wl_listener frame_done;
		struct wl_listener surface_destroy;
		struct wl_listener surface_commit;
	} WLR_PRIVATE;
};

/** A scene-graph node displaying a solid-colored rectangle */
struct wlr_scene_rect {
	struct wlr_scene_node node;
	int width, height;
	float color[4];

	struct fx_corner_radii corners;
	bool accepts_input;
	struct clipped_region clipped_region;
};

/** A scene-graph node displaying a two-color rounded border in one pass */
struct wlr_scene_border {
	struct wlr_scene_node node;
	int width, height;
	int inner_width, outer_width;
	float inner_color[4];
	float outer_color[4];
	struct clipped_region clipped_region;
	struct fx_corner_radii seam_corners;
	struct fx_corner_radii outer_corners;
};

/** A scene-graph node displaying a shadow */
struct wlr_scene_shadow {
	struct wlr_scene_node node;
	int width, height;
	int corner_radius;
	float color[4];
	float blur_sigma;

	struct clipped_region clipped_region;
};

struct wlr_scene_blur {
	struct wlr_scene_node node;
	int width, height;

	struct fx_corner_radii corners;
	struct clipped_region clipped_region;

	float strength;
	float alpha;
	float ignore_alpha;

	bool should_only_blur_bottom_layer;

	struct linked_node transparency_mask_source;
};

/** A scene-graph node telling SceneFX to render the optimized blur */
struct wlr_scene_optimized_blur {
	struct wlr_scene_node node;
	int width, height;

	bool dirty;
};

struct wlr_scene_outputs_update_event {
	struct wlr_scene_output **active;
	size_t size;
};

struct wlr_scene_output_sample_event {
	struct wlr_scene_output *output;
	bool direct_scanout;
	struct wlr_drm_syncobj_timeline *release_timeline;
	uint64_t release_point;
};

struct wlr_scene_frame_done_event {
	struct wlr_scene_output *output;
	struct timespec when;
};

/** A scene-graph node displaying a buffer */
struct wlr_scene_buffer {
	struct wlr_scene_node node;

	// May be NULL
	struct wlr_buffer *buffer;

	struct {
		struct wl_signal outputs_update; // struct wlr_scene_outputs_update_event
		struct wl_signal output_enter; // struct wlr_scene_output
		struct wl_signal output_leave; // struct wlr_scene_output
		struct wl_signal output_sample; // struct wlr_scene_output_sample_event
		struct wl_signal frame_done; // struct wlr_scene_frame_done_event
	} events;

	// May be NULL
	wlr_scene_buffer_point_accepts_input_func_t point_accepts_input;

	/**
	 * The output that the largest area of this buffer is displayed on.
	 * This may be NULL if the buffer is not currently displayed on any
	 * outputs.
	 */
	struct wlr_scene_output *primary_output;

	float opacity;
	enum wlr_scale_filter_mode filter_mode;
	struct wlr_fbox src_box;
	int dst_width, dst_height;
	enum wl_output_transform transform;
	pixman_region32_t opaque_region;

	enum wlr_color_transfer_function transfer_function;
	enum wlr_color_named_primaries primaries;
	enum wlr_color_encoding color_encoding;
	enum wlr_color_range color_range;

	struct {
		uint64_t active_outputs;
		struct wlr_texture *texture;
		struct wlr_linux_dmabuf_feedback_v1_init_options prev_feedback_options;

		bool own_buffer;
		int buffer_width, buffer_height;
		bool buffer_is_opaque;

		struct wlr_drm_syncobj_timeline *wait_timeline;
		uint64_t wait_point;

		struct wl_listener buffer_release;
		struct wl_listener renderer_destroy;

		// True if the underlying buffer is a wlr_single_pixel_buffer_v1
		bool is_single_pixel_buffer;
		// If is_single_pixel_buffer is set, contains the color of the buffer
		// as {R, G, B, A} where the max value of each component is UINT32_MAX
		uint32_t single_pixel_buffer_color[4];
	} WLR_PRIVATE;

	struct fx_corner_radii corners;
	/**
	 * Rounded rectangle `corners` describes, in node-relative coordinates. An
	 * empty box rounds the buffer's own destination box. While `corners` has a
	 * radius the draw is also confined to the box, so a buffer that does not
	 * reach a box corner keeps that corner square.
	 */
	struct wlr_box corner_box;
	struct linked_node blur;

	/**
	 * Additional luminance scaling applied while compositing this buffer.
	 * The identity value is 1.0.
	 */
	float luminance_multiplier;
};

/** A viewport for an output in the scene-graph */
struct wlr_scene_output {
	struct wlr_output *output;
	struct wl_list link; // wlr_scene.outputs
	struct wlr_scene *scene;
	struct wlr_addon addon;

	struct wlr_damage_ring damage_ring;

	int x, y;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		pixman_region32_t pending_commit_damage;

		uint8_t index;

		/**
		 * When scanout is applicable, we increment this every time a frame is rendered until
		 * DMABUF_FEEDBACK_DEBOUNCE_FRAMES is hit to debounce the scanout dmabuf feedback. Likewise,
		 * when scanout is no longer applicable, we decrement this until zero is hit to debounce
		 * composition dmabuf feedback.
		 */
		uint8_t dmabuf_feedback_debounce;
		bool prev_scanout;

		bool gamma_lut_changed;
		struct wlr_gamma_control_v1 *gamma_lut;
		struct wlr_color_transform *gamma_lut_color_transform;

		struct wlr_color_transform *prev_gamma_lut_color_transform;
		struct wlr_color_transform *prev_supplied_color_transform;
		struct wlr_color_transform *combined_color_transform;

		struct wl_listener output_commit;
		struct wl_listener output_damage;
		struct wl_listener output_needs_frame;

		struct wl_list damage_highlight_regions;

		struct wl_array render_list;

		struct wlr_drm_syncobj_timeline *in_timeline;
		uint64_t in_point;
		struct wlr_drm_syncobj_timeline *out_timeline;
		uint64_t out_point;

		// Fields Umbriel adds must stay after every wlroots field so this
		// struct remains a strict prefix extension of wlroots'. The scene
		// helpers Umbriel reuses from wlroots (wlr_scene_attach_output_layout,
		// wlr_scene_output_layout_add_output) read this struct at wlroots
		// offsets.
		float sdr_white_level;
		bool color_transform_dirty;
		bool direct_scanout_enabled;
	} WLR_PRIVATE;
};

struct wlr_scene_timer {
	int64_t pre_render_duration;
	struct wlr_render_timer *render_timer;
};

/** A layer shell scene helper */
struct wlr_scene_layer_surface_v1 {
	struct wlr_scene_tree *tree;
	struct wlr_layer_surface_v1 *layer_surface;

	struct {
		struct wl_listener tree_destroy;
		struct wl_listener layer_surface_destroy;
		struct wl_listener layer_surface_map;
		struct wl_listener layer_surface_unmap;
	} WLR_PRIVATE;
};

/**
 * Immediately destroy the scene-graph node.
 */
void wlr_scene_node_destroy(struct wlr_scene_node *node);
/**
 * Enable or disable this node. If a node is disabled, all of its children are
 * implicitly disabled as well.
 */
void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled);
/**
 * Set the position of the node relative to its parent.
 */
void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y);
/**
 * Move the node right above the specified sibling.
 * Asserts that node and sibling are distinct and share the same parent.
 */
void wlr_scene_node_place_above(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
/**
 * Move the node right below the specified sibling.
 * Asserts that node and sibling are distinct and share the same parent.
 */
void wlr_scene_node_place_below(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
/**
 * Move the node above all of its sibling nodes.
 */
void wlr_scene_node_raise_to_top(struct wlr_scene_node *node);
/**
 * Move the node below all of its sibling nodes.
 */
void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node);
/**
 * Move the node to another location in the tree.
 */
void wlr_scene_node_reparent(struct wlr_scene_node *node,
	struct wlr_scene_tree *new_parent);
/**
 * Get the node's layout-local coordinates.
 *
 * True is returned if the node and all of its ancestors are enabled.
 */
bool wlr_scene_node_coords(struct wlr_scene_node *node, int *lx, int *ly);
/**
 * Call `iterator` on each buffer in the scene-graph, with the buffer's
 * position in layout coordinates. The function is called from root to leaves
 * (in rendering order).
 */
void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
	wlr_scene_buffer_iterator_func_t iterator, void *user_data);
/**
 * Find the topmost node in this scene-graph that contains the point at the
 * given layout-local coordinates. (For surface nodes, this means accepting
 * input events at that point.) Returns the node and coordinates relative to the
 * returned node, or NULL if no node is found at that location.
 */
struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
	double lx, double ly, double *nx, double *ny);

/**
 * Check that every scene helper resolves into umbrielfx rather than libwlroots.
 *
 * umbrielfx's scene structs carry trailing fields wlroots knows nothing about,
 * so a node allocated by wlroots is short and umbrielfx reads those fields past
 * the end of it. Call this before building a scene: it returns NULL when the
 * helpers are sound, otherwise a message naming the first one that came from
 * elsewhere. The message points at a static buffer and stays valid.
 */
const char *umbrielfx_scene_check_helpers(void);

/**
 * Create a new scene-graph.
 *
 * The graph is also a struct wlr_scene_node. Associated resources can be
 * destroyed through wlr_scene_node_destroy().
 */
struct wlr_scene *wlr_scene_create(void);


// Sets the global blur parameters
void wlr_scene_set_blur_data(struct wlr_scene *scene, int num_passes,
	int radius, float noise, float brightness, float contrast, float saturation);

// Sets the global blur num_passes parameter
void wlr_scene_set_blur_num_passes(struct wlr_scene *scene, int num_passes);

// Sets the global blur radius parameter
void wlr_scene_set_blur_radius(struct wlr_scene *scene, int radius);

// Sets the global blur noise parameter
void wlr_scene_set_blur_noise(struct wlr_scene *scene, float noise);

// Sets the global blur brightness parameter
void wlr_scene_set_blur_brightness(struct wlr_scene *scene, float brightness);

// Sets the global blur contrast parameter
void wlr_scene_set_blur_contrast(struct wlr_scene *scene, float contrast);

// Sets the global blur saturation parameter
void wlr_scene_set_blur_saturation(struct wlr_scene *scene, float saturation);

/**
 * Handles linux_dmabuf_v1 feedback for all surfaces in the scene.
 *
 * Asserts that a struct wlr_linux_dmabuf_v1 hasn't already been set for the scene.
 */
void wlr_scene_set_linux_dmabuf_v1(struct wlr_scene *scene,
	struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1);

/**
 * Handles gamma_control_v1 for all outputs in the scene.
 *
 * Asserts that a struct wlr_gamma_control_manager_v1 hasn't already been set
 * for the scene.
 */
void wlr_scene_set_gamma_control_manager_v1(struct wlr_scene *scene,
	struct wlr_gamma_control_manager_v1 *gamma_control);

/**
 * Handles color_management_v1 feedback for all surfaces in the scene.
 *
 * Asserts that a struct wlr_color_manager_v1 hasn't already been set for the scene.
 */
void wlr_scene_set_color_manager_v1(struct wlr_scene *scene, struct wlr_color_manager_v1 *manager);

/**
 * Add a node displaying nothing but its children.
 */
struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent);

/**
 * Set the rectangular clip of a scene tree, in coordinates local to the tree.
 *
 * Pass NULL or an empty box to remove the clip. Descendants are constrained to
 * the intersection of every clip above them.
 *
 * The clip is a pure scissor: it constrains rendering, damage, output
 * membership (enter/leave), frame callbacks and hit testing of every
 * descendant, and it never resizes or rescales a buffer, so src_box and
 * dest_size are untouched. It is stored in a wlr_addon on the tree's node.
 */
void wlr_scene_tree_set_clip(struct wlr_scene_tree *tree, const struct wlr_box *clip);

/**
 * Add a node displaying a single surface to the scene-graph.
 *
 * The child sub-surfaces are ignored. See wlr_scene_subsurface_tree_create()
 *
 * Note that this helper does multiple things on behalf of the compositor. Some
 * of these include protocol implementations where compositors just need to enable
 * the protocols:
 *  - wp_viewporter
 *  - wp_presentation_time
 *  - wp_fractional_scale_v1
 *  - wp_alpha_modifier_v1
 *  - wp_linux_drm_syncobj_v1
 *  - zwp_linux_dmabuf_v1 presentation feedback with wlr_scene_set_linux_dmabuf_v1()
 *
 * This helper will also transparently:
 *  - Send preferred buffer scale¹
 *  - Send preferred buffer transform¹
 *  - Restack xwayland surfaces. See wlr_xwayland_surface_restack()²
 *  - Send output enter/leave events.
 *
 * ¹ Note that scale and transform sent to the surface will be based on the output
 * which has the largest visible surface area. Intelligent visibility calculations
 * influence this.
 * ² xwayland stacking order is undefined when the xwayland surfaces do not
 * intersect.
 */
struct wlr_scene_surface *wlr_scene_surface_create(struct wlr_scene_tree *parent,
	struct wlr_surface *surface);

/**
 * If this node represents a wlr_scene_buffer, that buffer will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_buffer.
 */
struct wlr_scene_buffer *wlr_scene_buffer_from_node(struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_tree, that tree will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_tree.
 */
struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_rect, that rect will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_rect.
 */
struct wlr_scene_rect *wlr_scene_rect_from_node(struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_border, that border will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_border.
 */
struct wlr_scene_border *wlr_scene_border_from_node(struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_shadow, that shadow will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_shadow.
 */
struct wlr_scene_shadow *wlr_scene_shadow_from_node(struct wlr_scene_node *node);

struct wlr_scene_blur *wlr_scene_blur_from_node(struct wlr_scene_node *node);

/**
 * If this buffer is backed by a surface, then the struct wlr_scene_surface is
 * returned. If not, NULL will be returned.
 */
struct wlr_scene_surface *wlr_scene_surface_try_from_buffer(
	struct wlr_scene_buffer *scene_buffer);

/**
 * Call wlr_surface_send_frame_done() if the surface is visible.
 */
void wlr_scene_surface_send_frame_done(struct wlr_scene_surface *scene_surface,
	const struct timespec *when);

/**
 * Add a node displaying a solid-colored rectangle to the scene-graph.
 *
 * The color argument must be a premultiplied color value.
 */
struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
		int width, int height, const float color[static 4]);

/**
 * Change the width and height of an existing rectangle node.
 */
void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height);

/**
 * Change the corner radius of all corners of an existing rectangle node.
 */
void wlr_scene_rect_set_corner_radius(struct wlr_scene_rect *rect, int corner_radius);

/**
 * Change the rounded corners of an existing rectangle node.
 */
void wlr_scene_rect_set_corner_radii(struct wlr_scene_rect *rect, struct fx_corner_radii);

/**
 * Sets the region where to clip the rect.
 *
 * For there to be corner rounding of the clipped region, the corner radius and
 * corners must be non-zero.
 *
 * NOTE: The positioning is node-relative.
 */
void wlr_scene_rect_set_clipped_region(struct wlr_scene_rect *rect,
		struct clipped_region clipped_region);

/**
 * Change the color of an existing rectangle node.
 *
 * The color argument must be a premultiplied color value.
 */
void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]);

/**
 * Add a node displaying a two-color rounded border.
 *
 * Colors must be premultiplied. Widths are logical pixels. Geometry is set
 * separately so callers can update every shape field atomically.
 */
struct wlr_scene_border *wlr_scene_border_create(struct wlr_scene_tree *parent,
		const float inner_color[static 4], const float outer_color[static 4]);

/**
 * The clipped region is the transparent content box and its inner corner radii.
 * Seam and outer radii describe the other two nested contours explicitly.
 */
void wlr_scene_border_set_geometry(struct wlr_scene_border *border,
		int width, int height, int inner_width, int outer_width,
		struct clipped_region clipped_region,
		struct fx_corner_radii seam_corners,
		struct fx_corner_radii outer_corners);

void wlr_scene_border_set_colors(struct wlr_scene_border *border,
		const float inner_color[static 4], const float outer_color[static 4]);

/**
 * Add a node displaying a shadow to the scene-graph.
 */
struct wlr_scene_shadow *wlr_scene_shadow_create(struct wlr_scene_tree *parent,
		int width, int height, int corner_radius, float blur_sigma,
		const float color[static 4]);

/**
 * Change the width and height of an existing shadow node.
 */
void wlr_scene_shadow_set_size(struct wlr_scene_shadow *shadow, int width, int height);

/**
 * Change the corner radius of an existing shadow node.
 */
void wlr_scene_shadow_set_corner_radius(struct wlr_scene_shadow *shadow, int corner_radius);

/**
 * Change the blur_sigma of an existing shadow node.
 */
void wlr_scene_shadow_set_blur_sigma(struct wlr_scene_shadow *shadow, float blur_sigma);

/**
 * Change the color of an existing shadow node.
 */
void wlr_scene_shadow_set_color(struct wlr_scene_shadow *shadow, const float color[static 4]);

/**
 * Sets the region where to clip the shadow.
 *
 * For there to be corner rounding of the clipped region, the corner radius and
 * corners must be non-zero.
 *
 * NOTE: The positioning is node-relative.
 */
void wlr_scene_shadow_set_clipped_region(struct wlr_scene_shadow *shadow,
		struct clipped_region clipped_region);

/**
 * Add a node displaying a blur to the scene-graph.
 */
struct wlr_scene_blur *wlr_scene_blur_create(struct wlr_scene_tree *parent,
	   int width, int height);

/**
 * Change the width and height of an existing blur node.
 */
void wlr_scene_blur_set_size(struct wlr_scene_blur *blur, int width, int height);

/**
 * Change the corner radius of all corners of an existing blur node.
 */
void wlr_scene_blur_set_corner_radius(struct wlr_scene_blur *blur, int corner_radius);

/**
 * Change the corners of an existing blur node.
 */
void wlr_scene_blur_set_corner_radii(struct wlr_scene_blur *blur, struct fx_corner_radii);

/**
 * Make the blur node only blur the bottom layer of the scene
 */
void wlr_scene_blur_set_should_only_blur_bottom_layer(struct wlr_scene_blur *blur,
	bool should_only_blur_bottom_layer);

/**
 * Set the transparency mask source for the blur, only rendering blur where the
 * Mask source is actually rendering (e.g. skip transparent spaces)
 */
void wlr_scene_blur_set_transparency_mask_source(struct wlr_scene_blur *blur,
	   struct wlr_scene_buffer *source);

/**
 * get the transparency mask source for the blur
 */
struct wlr_scene_buffer *wlr_scene_blur_get_transparency_mask_source(
	struct wlr_scene_blur *blur);

/**
 * Sets the blur alpha from 1.0f -> 0.0f. This adjusts the actual alpha of the blur.
 * Default is 1.0f.
 *
 * Lower values without also adjusting the strength will look off.
 *
 * Can be used combined with strength to create a good-looking
 * fade-out effect.
 */
void wlr_scene_blur_set_alpha(struct wlr_scene_blur *blur, float alpha);

/**
 * Sets the blur strength from 1.0f -> 0.0f. This adjusts how strong the blur is
 * relative to the base 1.0 value.
 *
 * Can be used combined with alpha to create a good-looking
 * fade-out effect.
 */
void wlr_scene_blur_set_strength(struct wlr_scene_blur *blur, float strength);

/**
 * Skip blurring pixels whose transparency-mask alpha is below this threshold
 * (0.0..1.0). 0.0 blurs the whole node box. Only effective when a
 * transparency mask source is set. Default 0.0.
 */
void wlr_scene_blur_set_ignore_alpha(struct wlr_scene_blur *blur, float ignore_alpha);

/**
 * Sets the region where to clip the blur.
 *
 * For there to be corner rounding of the clipped region, the corner radius and
 * corners must be non-zero.
 *
 * NOTE: The positioning is node-relative.
 */
void wlr_scene_blur_set_clipped_region(struct wlr_scene_blur *blur,
		struct clipped_region clipped_region);

/**
 * If this node represents a wlr_scene_optimized_blur node, that node will
 * be returned. It is not legal to feed a node that does not represent
 * a wlr_scene_optimized_blur.
 */
struct wlr_scene_optimized_blur *wlr_scene_optimized_blur_from_node(
		struct wlr_scene_node *node);

/**
 * Add a node indicating to the renderer to render optimized blur to the scene-graph.
 * NOTE: Has to be positioned where to draw the optimized blur. This allows
 * for unique effects like only blurring half of the output.
 */
struct wlr_scene_optimized_blur *wlr_scene_optimized_blur_create(struct wlr_scene_tree *parent,
		int width, int height);

/**
 * Change the width and height of an existing blur node.
 * This calls `wlr_scene_optimized_blur_mark_dirty` as well
 */
void wlr_scene_optimized_blur_set_size(struct wlr_scene_optimized_blur *blur_node,
		int width, int height);

/**
 * Tells the renderer to re-render the optimized blur.
 * This is very expensive so should only be called when needed.
 *
 * An example use would be to call this when a "static" node changes, like a
 * wallpaper.
 */
void wlr_scene_optimized_blur_mark_dirty(struct wlr_scene_optimized_blur *blur_node);

/**
 * Add a node displaying a buffer to the scene-graph.
 *
 * If the buffer is NULL, this node will not be displayed.
 */
struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
	struct wlr_buffer *buffer);

/**
 * Sets the buffer's backing buffer.
 *
 * If the buffer is NULL, the buffer node will not be displayed.
 */
void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer);

/**
 * Sets the buffer's backing buffer with a custom damage region.
 *
 * The damage region is in buffer-local coordinates. If the region is NULL,
 * the whole buffer node will be damaged.
 */
void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const pixman_region32_t *region);

/**
 * Options for wlr_scene_buffer_set_buffer_with_options().
 */
struct wlr_scene_buffer_set_buffer_options {
	// The damage region is in buffer-local coordinates. If the region is NULL,
	// the whole buffer node will be damaged.
	const pixman_region32_t *damage;

	// Wait for a timeline synchronization point before reading from the buffer.
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

/**
 * Sets the buffer's backing buffer.
 *
 * If the buffer is NULL, the buffer node will not be displayed. If options is
 * NULL, empty options are used.
 */
void wlr_scene_buffer_set_buffer_with_options(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const struct wlr_scene_buffer_set_buffer_options *options);

/**
 * Sets the buffer's opaque region. This is an optimization hint used to
 * determine if buffers which reside under this one need to be rendered or not.
 */
void wlr_scene_buffer_set_opaque_region(struct wlr_scene_buffer *scene_buffer,
	const pixman_region32_t *region);

/**
 * Set the source rectangle describing the region of the buffer which will be
 * sampled to render this node. This allows cropping the buffer.
 *
 * If NULL, the whole buffer is sampled. By default, the source box is NULL.
 */
void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
	const struct wlr_fbox *box);

/**
 * Set the destination size describing the region of the scene-graph the buffer
 * will be painted onto. This allows scaling the buffer.
 *
 * If zero, the destination size will be the buffer size. By default, the
 * destination size is zero.
 */
void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
	int width, int height);

/**
 * Set a transform which will be applied to the buffer.
 */
void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
	enum wl_output_transform transform);

/**
* Sets the opacity of this buffer
*/
void wlr_scene_buffer_set_opacity(struct wlr_scene_buffer *scene_buffer,
	float opacity);

/**
* Sets the filter mode to use when scaling the buffer
*/
void wlr_scene_buffer_set_filter_mode(struct wlr_scene_buffer *scene_buffer,
	enum wlr_scale_filter_mode filter_mode);

/**
* Sets the corner radius of all corners to round of this buffer
*/
void wlr_scene_buffer_set_corner_radius(struct wlr_scene_buffer *scene_buffer,
		int radii);

/**
* Sets the corner radii of this buffer
*/
void wlr_scene_buffer_set_corner_radii(struct wlr_scene_buffer *scene_buffer,
	struct fx_corner_radii corner_radii);

/**
* Sets the box the corner radii of this buffer describe, in node-relative
* coordinates. NULL or an empty box rounds the buffer's own destination box.
*/
void wlr_scene_buffer_set_corner_box(struct wlr_scene_buffer *scene_buffer,
	const struct wlr_box *box);

void wlr_scene_buffer_set_transfer_function(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_transfer_function transfer_function);

void wlr_scene_buffer_set_primaries(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_named_primaries primaries);

void wlr_scene_buffer_set_luminance_multiplier(
	struct wlr_scene_buffer *scene_buffer, float multiplier);

void wlr_scene_buffer_set_color_encoding(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_encoding encoding);

void wlr_scene_buffer_set_color_range(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_range range);

/**
 * Calls the buffer's frame_done signal.
 */
void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
	struct wlr_scene_frame_done_event *event);

/**
 * Add a viewport for the specified output to the scene-graph.
 *
 * An output can only be added once to the scene-graph.
 */
struct wlr_scene_output *wlr_scene_output_create(struct wlr_scene *scene,
	struct wlr_output *output);
/**
 * Destroy a scene-graph output.
 */
void wlr_scene_output_destroy(struct wlr_scene_output *scene_output);
/**
 * Set the output's position in the scene-graph.
 */
void wlr_scene_output_set_position(struct wlr_scene_output *scene_output,
	int lx, int ly);

/**
 * Enable or disable direct scanout attempts for this scene output.
 *
 * Changing this fully damages and schedules the output.
 */
void wlr_scene_output_set_direct_scanout_enabled(
	struct wlr_scene_output *scene_output, bool enabled);

/**
 * Set the SDR white level used for PQ input and output normalization.
 *
 * A value of zero selects the transfer function's default reference
 * luminance.
 */
void wlr_scene_output_set_sdr_white_level(
	struct wlr_scene_output *scene_output, float nits);

struct wlr_scene_output_state_options {
	struct wlr_scene_timer *timer;

	/**
	 * Make imports of the rendered output buffer expose an SDR capture view
	 * when an output color transform is active.
	 */
	bool capture_sdr;

	/**
	 * Color transform to apply before the output's color transform. Cannot be
	 * used when the output has a non-NULL image description set.
	 */
	struct wlr_color_transform *color_transform;

	/**
	 * Allows use of a custom swapchain. This can be useful when trying out an
	 * output configuration. The swapchain dimensions must match the respective
	 * wlr_output_state or output size if not specified.
	 */
	struct wlr_swapchain *swapchain;
};

/**
 * Returns true if scene wants to render a new frame. False, if no new frame
 * is needed and an output commit can be skipped for the current frame.
 */
bool wlr_scene_output_needs_frame(struct wlr_scene_output *scene_output);

/**
 * Render and commit an output.
 */
bool wlr_scene_output_commit(struct wlr_scene_output *scene_output,
	const struct wlr_scene_output_state_options *options);

/**
 * Render and populate given output state.
 */
bool wlr_scene_output_build_state(struct wlr_scene_output *scene_output,
	struct wlr_output_state *state, const struct wlr_scene_output_state_options *options);

/**
 * Retrieve the duration in nanoseconds between the last wlr_scene_output_commit() call and the end
 * of its operations, including those on the GPU that may have finished after the call returned.
 *
 * Returns -1 if the duration is unavailable.
 */
int64_t wlr_scene_timer_get_duration_ns(struct wlr_scene_timer *timer);
void wlr_scene_timer_finish(struct wlr_scene_timer *timer);

/**
 * Call wlr_surface_send_frame_done() on all surfaces in the scene rendered by
 * wlr_scene_output_commit() for which wlr_scene_surface.primary_output
 * matches the given scene_output.
 */
void wlr_scene_output_send_frame_done(struct wlr_scene_output *scene_output,
	struct timespec *now);
/**
 * Call `iterator` on each buffer in the scene-graph visible on the output,
 * with the buffer's position in layout coordinates. The function is called
 * from root to leaves (in rendering order).
 */
void wlr_scene_output_for_each_buffer(struct wlr_scene_output *scene_output,
	wlr_scene_buffer_iterator_func_t iterator, void *user_data);
/**
 * Get a scene-graph output from a struct wlr_output.
 *
 * If the output hasn't been added to the scene-graph, returns NULL.
 */
struct wlr_scene_output *wlr_scene_get_scene_output(struct wlr_scene *scene,
	struct wlr_output *output);

/**
 * Attach an output layout to a scene.
 *
 * The resulting scene output layout allows to synchronize the positions of scene
 * outputs with the positions of corresponding layout outputs.
 *
 * It is automatically destroyed when the scene or the output layout is destroyed.
 */
struct wlr_scene_output_layout *wlr_scene_attach_output_layout(struct wlr_scene *scene,
	struct wlr_output_layout *output_layout);

/**
 * Add an output to the scene output layout.
 *
 * When the layout output is repositioned, the scene output will be repositioned
 * accordingly.
 */
void wlr_scene_output_layout_add_output(struct wlr_scene_output_layout *sol,
	struct wlr_output_layout_output *lo, struct wlr_scene_output *so);

/**
 * Add a node displaying a surface and all of its sub-surfaces to the
 * scene-graph.
 */
struct wlr_scene_tree *wlr_scene_subsurface_tree_create(
	struct wlr_scene_tree *parent, struct wlr_surface *surface);

/**
 * Sets a cropping region for any subsurface trees that are children of this
 * scene node. The clip coordinate space will be that of the root surface of
 * the subsurface tree.
 *
 * A NULL or empty clip will disable clipping
 */
void wlr_scene_subsurface_tree_set_clip(struct wlr_scene_node *node,
	const struct wlr_box *clip);

/**
 * Add a node displaying an xdg_surface and all of its sub-surfaces to the
 * scene-graph.
 *
 * The origin of the returned scene-graph node will match the top-left corner
 * of the xdg_surface window geometry.
 */
struct wlr_scene_tree *wlr_scene_xdg_surface_create(
	struct wlr_scene_tree *parent, struct wlr_xdg_surface *xdg_surface);

/**
 * Add a node displaying a layer_surface_v1 and all of its sub-surfaces to the
 * scene-graph.
 *
 * The origin of the returned scene-graph node will match the top-left corner
 * of the layer surface.
 */
struct wlr_scene_layer_surface_v1 *wlr_scene_layer_surface_v1_create(
	struct wlr_scene_tree *parent, struct wlr_layer_surface_v1 *layer_surface);

/**
 * Configure a layer_surface_v1, position its scene node in accordance to its
 * current state, and update the remaining usable area.
 *
 * full_area represents the entire area that may be used by the layer surface
 * if its exclusive_zone is -1, and is usually the output dimensions.
 * usable_area represents what remains of full_area that can be used if
 * exclusive_zone is >= 0. usable_area is updated if the surface has a positive
 * exclusive_zone, so that it can be used for the next layer surface.
 */
void wlr_scene_layer_surface_v1_configure(
	struct wlr_scene_layer_surface_v1 *scene_layer_surface,
	const struct wlr_box *full_area, struct wlr_box *usable_area);

/**
 * Add a node displaying a drag icon and all its sub-surfaces to the
 * scene-graph.
 */
struct wlr_scene_tree *wlr_scene_drag_icon_create(
	struct wlr_scene_tree *parent, struct wlr_drag_icon *drag_icon);

#endif
