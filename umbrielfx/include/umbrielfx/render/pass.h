#ifndef SCENE_FX_RENDER_PASS_H
#define SCENE_FX_RENDER_PASS_H

#include <stdbool.h>
#include <GLES2/gl2.h>
#include <wlr/render/color.h>
#include <wlr/render/pass.h>
#include <wlr/render/interface.h>
#include <wlr/render/swapchain.h>

#include "render/egl.h"
#include "types/fx/clipped_region.h"

struct fx_gles_render_pass {
	struct wlr_render_pass base;
	struct fx_framebuffer *buffer;
	struct fx_framebuffer *output_buffer;
	float projection_matrix[9];
	struct wlr_egl_context prev_ctx;
	struct fx_render_timer *timer;
	struct wlr_drm_syncobj_timeline *signal_timeline;
	uint64_t signal_point;
	bool has_color_transform;
	struct wlr_color_transform *color_transform;
	struct fx_offscreen_buffers *output_buffers;
	float output_matrix[9];
	enum wlr_color_transfer_function output_tf;
	GLuint output_lut;
	float output_lut_dim;
	pixman_region32_t updated_region;
	// A new or resized shared blend buffer needs every output pixel initialized.
	bool needs_full_damage;
	// Set while an add_* call renders into an offscreen buffer instead of the
	// pass target, so it must not extend updated_region.
	bool suppress_updated;

	// The region where there's blur
	pixman_region32_t blur_padding_region;
	bool has_blur;
	// Contains output-specific framebuffers.
	// NULL when no advanced effects like blur is being used in the current pass.
	// Call `fx_render_pass_init_offscreen_buffers` to use advanced effects.
	struct fx_offscreen_buffers *fx_offscreen_buffers;
};

struct fx_gradient {
	float degree;
	/* The full area the gradient fit too, for borders use the window size */
	struct wlr_box range;
	/* The center of the gradient, {0.5, 0.5} for normal*/
	float origin[2];
	/* 1 = Linear, 2 = Conic */
	int linear;
	/* Whether or not to blend the colors */
	int blend;
	int count;
	float *colors;
};

struct fx_render_texture_options {
	struct wlr_render_texture_options base;
	const struct wlr_box *clip_box; // Used to clip csd. Ignored if NULL
	struct fx_corner_fradii corners;
	float discard_transparent; // discard pixels with alpha < this; 0.0 discards nothing
	struct clipped_fregion clipped_region;
	// Texels the draw may sample, in buffer coordinates. A source box snapped to
	// whole texels can reach past the region a cropped surface actually owns;
	// this keeps sampling inside it by duplicating its edge texel. Empty draws
	// with the regular shader and no sampling clamp at all; only pass a box when
	// the source box actually reaches texels outside it.
	struct wlr_fbox sample_box;
};

struct fx_render_rect_options {
	struct wlr_render_rect_options base;
	struct clipped_fregion clipped_region;
};

struct fx_render_rect_grad_options {
	struct wlr_render_rect_options base;
	struct fx_gradient gradient;
};

struct fx_render_rounded_rect_options {
	struct wlr_render_rect_options base;
	struct fx_corner_fradii corners;
	struct clipped_fregion clipped_region;
};

struct fx_render_border_options {
	struct wlr_box box;
	const pixman_region32_t *clip;
	struct clipped_fregion clipped_region;
	struct fx_corner_fradii seam_corners;
	struct fx_corner_fradii outer_corners;
	float inner_width;
	float outer_width;
	struct wlr_render_color inner_color;
	struct wlr_render_color outer_color;
};

struct fx_render_rounded_rect_grad_options {
	struct wlr_render_rect_options base;
	struct fx_gradient gradient;
	struct fx_corner_fradii corners;
};

struct fx_render_box_shadow_options {
	struct wlr_box box;
	struct clipped_fregion clipped_region;
	/* Clip region, leave NULL to disable clipping */
	const pixman_region32_t *clip;

	float blur_sigma;
	int corner_radius;
	struct wlr_render_color color;
};

struct fx_render_blur_pass_options {
	struct fx_render_texture_options tex_options;
	struct fx_framebuffer *current_buffer;
	struct blur_data *blur_data;
	bool use_optimized_blur;
	float ignore_alpha; // 0.0 = blur whole box; >0 = skip pixels with mask alpha < value
	float blur_strength;
	struct fx_corner_fradii corners;
	struct clipped_fregion clipped_region;
};

struct fx_gles_render_pass *fx_get_render_pass(struct wlr_render_pass *render_pass);

/**
 * Attaches the output's offscreen buffer set to the render pass. The
 * individual buffers are allocated when an effect first needs them.
 */
bool fx_render_pass_init_offscreen_buffers(struct wlr_render_pass *render_pass,
		struct wlr_output *output);

/**
 * Returns the buffer holding the pixels saved around blurred regions,
 * allocating it on first use. NULL without offscreen buffers or when the
 * allocation failed.
 */
struct fx_framebuffer *fx_render_pass_blur_saved_pixels_buffer(
		struct fx_gles_render_pass *pass);

/**
 * Render a fx texture.
 */
void fx_render_pass_add_texture(struct fx_gles_render_pass *render_pass,
	const struct fx_render_texture_options *options);

/**
 * Render a rectangle.
 */
void fx_render_pass_add_rect(struct fx_gles_render_pass *render_pass,
	const struct fx_render_rect_options *options);

/**
 * Render a rectangle with a gradient.
 */
void fx_render_pass_add_rect_grad(struct fx_gles_render_pass *render_pass,
	const struct fx_render_rect_grad_options *options);

/**
 * Render a rounded rectangle.
 */
void fx_render_pass_add_rounded_rect(struct fx_gles_render_pass *render_pass,
	const struct fx_render_rounded_rect_options *options);

/**
 * Render a two-color rounded border.
 */
void fx_render_pass_add_border(struct fx_gles_render_pass *render_pass,
	const struct fx_render_border_options *options);

/**
 * Render a rounded rectangle with a gradient.
 */
void fx_render_pass_add_rounded_rect_grad(struct fx_gles_render_pass *render_pass,
	const struct fx_render_rounded_rect_grad_options *options);

/**
 * Render a box shadow.
 */
void fx_render_pass_add_box_shadow(struct fx_gles_render_pass *pass,
		const struct fx_render_box_shadow_options *options);

/**
 * Render blur.
 */
void fx_render_pass_add_blur(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options);

/**
 * Render optimized blur.
 */
bool fx_render_pass_add_optimized_blur(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options);

/**
 * Render from one buffer to another
 */
void fx_render_pass_read_to_buffer(struct fx_gles_render_pass *pass,
		pixman_region32_t *region, struct fx_framebuffer *dst_buffer,
		struct fx_framebuffer *src_buffer);

#endif
