#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/transform.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "render/color.h"
#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/fx_renderer/shaders.h"
#include "render/pass.h"
#include "render/tracy.h"
#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"
#include "umbrielfx/render/fx_renderer/fx_renderer.h"
#include "umbrielfx/types/fx/blur_data.h"
#include "util/matrix.h"

#define MAX_QUADS 86 // 4kb

struct fx_render_texture_options fx_render_texture_options_default(
		const struct wlr_render_texture_options *base) {
	struct fx_render_texture_options options = {
		.corners = {0},
		.discard_transparent = 0.0f,
		.clip_box = NULL,
		.clipped_region = {0},
	};
	memcpy(&options.base, base, sizeof(*base));
	return options;
}

struct fx_render_rect_options fx_render_rect_options_default(
		const struct wlr_render_rect_options *base) {
	struct fx_render_rect_options options = {
		.base = *base,
		.clipped_region = {
			.area = { .0, .0, .0, .0 },
			.corners = {0},
		},
	};
	return options;
}

bool fx_render_pass_init_offscreen_buffers(struct wlr_render_pass *render_pass,
		struct wlr_output *output) {
	struct fx_gles_render_pass *pass = fx_get_render_pass(render_pass);
	if (output == NULL) {
		pass->fx_offscreen_buffers = NULL;
		return false;
	}
	if (pass->fx_offscreen_buffers != NULL) {
		wlr_log(WLR_ERROR, "Extra buffers called twice. Ignoring...");
		return true;
	}

	// For per output framebuffers
	pass->fx_offscreen_buffers = fx_offscreen_buffers_try_get(output);
	if (pass->fx_offscreen_buffers == NULL) {
		wlr_log(WLR_ERROR, "Failed to get/create effect framebuffers for output: %s",
				output->name);
		return false;
	}
	pass->fx_offscreen_buffers->allocator = output->allocator;
	return true;
}

// Allocates the offscreen buffer in *slot on first use, matching the pass
// target's size and its FP16 format under a color transform. Rebinds the pass
// target. Returns NULL when the buffer could not be allocated.
static struct fx_framebuffer *ensure_offscreen_buffer(struct fx_gles_render_pass *pass,
		struct fx_framebuffer **slot, bool alpha) {
	struct fx_offscreen_buffers *fbos = pass->fx_offscreen_buffers;
	if (fbos == NULL) {
		return NULL;
	}
	uint32_t format;
	if (pass->has_color_transform) {
		format = DRM_FORMAT_ABGR16161616F;
	} else {
		format = alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
	}
	bool failed = false;
	fx_framebuffer_get_or_create_custom(pass->buffer->renderer, fbos->allocator,
		pass->buffer->buffer->width, pass->buffer->buffer->height, format,
		slot, &failed);
	fx_framebuffer_bind(pass->buffer);
	if (failed) {
		wlr_log(WLR_ERROR, "Failed to create effect framebuffer");
		return NULL;
	}
	return *slot;
}

struct fx_framebuffer *fx_render_pass_blur_saved_pixels_buffer(
		struct fx_gles_render_pass *pass) {
	if (pass->fx_offscreen_buffers == NULL) {
		return NULL;
	}
	return ensure_offscreen_buffer(pass,
		&pass->fx_offscreen_buffers->blur_saved_pixels_buffer, false);
}

///
/// Base Wlroots pass functions
///

static const struct wlr_render_pass_impl render_pass_impl;

static void render(const struct wlr_box *box, const pixman_region32_t *clip,
	GLint attrib);
static void set_proj_matrix(GLint loc, float proj[9],
	const struct wlr_box *box);
static void set_tex_matrix(GLint loc, enum wl_output_transform trans,
	const struct wlr_fbox *box);

struct fx_gles_render_pass *fx_get_render_pass(struct wlr_render_pass *render_pass) {
	assert(render_pass->impl == &render_pass_impl);
	struct fx_gles_render_pass *pass = wl_container_of(render_pass, pass, base);
	return pass;
}

static bool render_pass_apply_output_transform(struct fx_gles_render_pass *pass) {
	if (!pass->has_color_transform ||
			!pixman_region32_not_empty(&pass->updated_region)) {
		return true;
	}

	struct fx_renderer *renderer = pass->buffer->renderer;
	struct wlr_texture *wlr_texture = fx_texture_from_buffer(
		&renderer->wlr_renderer, pass->buffer->buffer);
	if (wlr_texture == NULL) {
		return false;
	}
	struct fx_texture *texture = fx_get_texture(wlr_texture);
	struct output_shader *shader = &renderer->shaders.output;
	struct wlr_box box = {
		.width = pass->output_buffer->buffer->width,
		.height = pass->output_buffer->buffer->height,
	};
	struct wlr_fbox src_box = {
		.width = 1.0,
		.height = 1.0,
	};

	fx_framebuffer_bind(pass->output_buffer);
	glViewport(0, 0, box.width, box.height);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glUseProgram(shader->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);
	glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(shader->tex, 0);

	glUniformMatrix3fv(shader->matrix, 1, GL_FALSE, pass->output_matrix);
	glUniform1i(shader->inverse_eotf, pass->output_tf);
	glUniform1i(shader->has_lut, pass->output_lut != 0);
	glUniform1f(shader->lut_dim, pass->output_lut_dim);
	if (pass->output_lut != 0) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, pass->output_lut);
		glUniform1i(shader->lut, 1);
	}

	set_proj_matrix(shader->proj, pass->projection_matrix, &box);
	set_tex_matrix(shader->tex_proj, WL_OUTPUT_TRANSFORM_NORMAL, &src_box);
	render(&box, &pass->updated_region, shader->pos_attrib);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, 0);
	wlr_texture_destroy(wlr_texture);
	return true;
}

static bool render_pass_submit(struct wlr_render_pass *wlr_pass) {
	struct fx_gles_render_pass *pass = fx_get_render_pass(wlr_pass);
	struct fx_renderer *renderer = pass->buffer->renderer;
	struct fx_render_timer *timer = pass->timer;
	bool ok = false;

	TRACY_BOTH_ZONES_START(pass->buffer->renderer);
	push_fx_debug(renderer);
	if (pass->output_buffers != NULL && pass->needs_full_damage) {
		const pixman_box32_t output_box = {
			.x1 = 0,
			.y1 = 0,
			.x2 = pass->output_buffer->buffer->width,
			.y2 = pass->output_buffer->buffer->height,
		};
		if (pixman_region32_contains_rectangle(&pass->updated_region,
				&output_box) != PIXMAN_REGION_IN) {
			wlr_log(WLR_ERROR, "Shared HDR blend buffer was not fully initialized");
			goto out;
		}
	}
	if (!render_pass_apply_output_transform(pass)) {
		goto out;
	}

	if (timer) {
		// clear disjoint flag
		GLint64 disjoint;
		renderer->procs.glGetInteger64vEXT(GL_GPU_DISJOINT_EXT, &disjoint);
		// set up the query
		renderer->procs.glQueryCounterEXT(timer->id, GL_TIMESTAMP_EXT);
		// get end-of-CPU-work time in GL time domain
		renderer->procs.glGetInteger64vEXT(GL_TIMESTAMP_EXT, &timer->gl_cpu_end);
		// get end-of-CPU-work time in CPU time domain
		clock_gettime(CLOCK_MONOTONIC, &timer->cpu_end);
	}

	if (pass->signal_timeline != NULL) {
		EGLSyncKHR sync = wlr_egl_create_sync(renderer->egl, -1);
		if (sync == EGL_NO_SYNC_KHR) {
			goto out;
		}

		int sync_file_fd = wlr_egl_dup_fence_fd(renderer->egl, sync);
		wlr_egl_destroy_sync(renderer->egl, sync);
		if (sync_file_fd < 0) {
			goto out;
		}

		ok = wlr_drm_syncobj_timeline_import_sync_file(pass->signal_timeline, pass->signal_point, sync_file_fd);
		close(sync_file_fd);
		if (!ok) {
			goto out;
		}
	} else {
		glFlush();
	}

	ok = true;

out:
	if (pass->output_buffers != NULL) {
		if (ok) {
			pass->output_buffers->blend_valid = true;
			if (pixman_region32_not_empty(&pass->updated_region)) {
				pass->output_buffers->blend_generation++;
				if (pass->output_buffers->blend_generation == 0) {
					pass->output_buffers->blend_generation++;
				}
			}
			pass->output_buffer->output_generation =
				pass->output_buffers->blend_generation;
		} else {
			pass->output_buffers->blend_valid = false;
			pass->output_buffer->output_generation = 0;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
	TRACY_GPU_ZONE_COLLECT(renderer);

	wlr_egl_restore_context(&pass->prev_ctx);

	wlr_drm_syncobj_timeline_unref(pass->signal_timeline);
	wlr_buffer_unlock(pass->output_buffer->buffer);
	wlr_color_transform_unref(pass->color_transform);

	pass->fx_offscreen_buffers = NULL;
	pixman_region32_fini(&pass->blur_padding_region);
	pixman_region32_fini(&pass->updated_region);

	free(pass);

	return ok;
}

static void render_pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct fx_gles_render_pass *pass = fx_get_render_pass(wlr_pass);
	const struct fx_render_texture_options fx_options =
		fx_render_texture_options_default(options);
	// Re-use fx function but with default options
	// TODO: Simplified version?
	fx_render_pass_add_texture(pass, &fx_options);
}

static void render_pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct fx_gles_render_pass *pass = fx_get_render_pass(wlr_pass);
	const struct fx_render_rect_options fx_options =
		fx_render_rect_options_default(options);
	// Re-use fx function but with default options
	// TODO: Simplified version?
	fx_render_pass_add_rect(pass, &fx_options);
}

static const struct wlr_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
};

///
/// FX pass functions
///

// Initialize the stenciling work. Returns false when the pass target has no
// stencil buffer, in which case no mask is active and stencil_mask_close and
// stencil_mask_fini must be skipped.
static bool stencil_mask_init(struct fx_gles_render_pass *pass) {
	if (!fx_framebuffer_ensure_stencil(pass->buffer)) {
		return false;
	}
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glEnable(GL_STENCIL_TEST);

	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	// Disable writing to color buffer
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	return true;
}

// Close the mask
static void stencil_mask_close(bool draw_inside_mask) {
	// Reenable writing to color buffer
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (draw_inside_mask) {
		glStencilFunc(GL_EQUAL, 1, 0xFF);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		return;
	}
	glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

// Finish stenciling and clear the buffer
static void stencil_mask_fini(void) {
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glDisable(GL_STENCIL_TEST);
}

static void render(const struct wlr_box *box, const pixman_region32_t *clip, GLint attrib) {
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, box->width, box->height);

	if (clip) {
		pixman_region32_intersect(&region, &region, clip);
	}

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(&region, &rects_len);
	if (rects_len == 0) {
		pixman_region32_fini(&region);
		return;
	}

	glEnableVertexAttribArray(attrib);

	for (int i = 0; i < rects_len;) {
		int batch = rects_len - i < MAX_QUADS ? rects_len - i : MAX_QUADS;
		int batch_end = batch + i;

		size_t vert_index = 0;
		GLfloat verts[MAX_QUADS * 6 * 2];
		for (; i < batch_end; i++) {
			const pixman_box32_t *rect = &rects[i];

			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
		}

		glVertexAttribPointer(attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
		glDrawArrays(GL_TRIANGLES, 0, batch * 6);
	}

	glDisableVertexAttribArray(attrib);

	pixman_region32_fini(&region);
}

static void set_proj_matrix(GLint loc, float proj[9], const struct wlr_box *box) {
	float gl_matrix[9];
	wlr_matrix_identity(gl_matrix);
	wlr_matrix_translate(gl_matrix, box->x, box->y);
	wlr_matrix_scale(gl_matrix, box->width, box->height);
	wlr_matrix_multiply(gl_matrix, proj, gl_matrix);
	glUniformMatrix3fv(loc, 1, GL_FALSE, gl_matrix);
}

static void set_tex_matrix(GLint loc, enum wl_output_transform trans,
		const struct wlr_fbox *box) {
	float tex_matrix[9];
	wlr_matrix_identity(tex_matrix);
	wlr_matrix_translate(tex_matrix, box->x, box->y);
	wlr_matrix_scale(tex_matrix, box->width, box->height);
	wlr_matrix_translate(tex_matrix, .5, .5);

	// since textures have a different origin point we have to transform
	// differently if we are rotating
	if (trans & WL_OUTPUT_TRANSFORM_90) {
		wlr_matrix_transform(tex_matrix, wlr_output_transform_invert(trans));
	} else {
		wlr_matrix_transform(tex_matrix, trans);
	}
	wlr_matrix_translate(tex_matrix, -.5, -.5);

	glUniformMatrix3fv(loc, 1, GL_FALSE, tex_matrix);
}

static void setup_blending(enum wlr_render_blend_mode mode) {
	switch (mode) {
	case WLR_RENDER_BLEND_MODE_PREMULTIPLIED:
		glEnable(GL_BLEND);
		break;
	case WLR_RENDER_BLEND_MODE_NONE:
		glDisable(GL_BLEND);
		break;
	}
}

static void render_pass_mark_updated(struct fx_gles_render_pass *pass,
		const struct wlr_box *box, const pixman_region32_t *clip) {
	if (!pass->has_color_transform || pass->suppress_updated) {
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, box->width, box->height);
	if (clip != NULL) {
		pixman_region32_intersect(&region, &region, clip);
	}
	pixman_region32_union(&pass->updated_region, &pass->updated_region, &region);
	pixman_region32_fini(&region);
}

static float color_to_linear_premult(float electrical, float alpha) {
	return alpha == 0.0f ? 0.0f : powf(electrical / alpha, 2.2f) * alpha;
}

static struct wlr_render_color pass_color(struct fx_gles_render_pass *pass,
		const struct wlr_render_color *color) {
	if (!pass->has_color_transform) {
		return *color;
	}
	return (struct wlr_render_color) {
		.r = color_to_linear_premult(color->r, color->a),
		.g = color_to_linear_premult(color->g, color->a),
		.b = color_to_linear_premult(color->b, color->a),
		.a = color->a,
	};
}

static float *pass_gradient_colors(struct fx_gles_render_pass *pass,
		const struct fx_gradient *gradient) {
	if (!pass->has_color_transform) {
		return gradient->colors;
	}
	float *colors = calloc(gradient->count * 4, sizeof(float));
	if (colors == NULL) {
		return NULL;
	}
	for (int i = 0; i < gradient->count; i++) {
		const float *input = &gradient->colors[i * 4];
		float *output = &colors[i * 4];
		output[0] = color_to_linear_premult(input[0], input[3]);
		output[1] = color_to_linear_premult(input[1], input[3]);
		output[2] = color_to_linear_premult(input[2], input[3]);
		output[3] = input[3];
	}
	return colors;
}

static bool apply_clip_region(pixman_region32_t *clip_region,
		const struct wlr_box *clipped_region_box, const struct fx_corner_fradii *corners) {
	if (wlr_box_empty(clipped_region_box)) {
		return false;
	}

	// The shader owns the rounded boundary. Remove only the central cross that
	// is certainly inside the hole, leaving every corner pixel for exact GPU
	// coverage. Truncating a diagonal approximation can otherwise cull a pixel
	// that the rounded shader needs to paint.
	const int top = ceilf(fmax(corners->top_left, corners->top_right));
	const int bottom = ceilf(fmax(corners->bottom_left, corners->bottom_right));
	const int left = ceilf(fmax(corners->top_left, corners->bottom_left));
	const int right = ceilf(fmax(corners->top_right, corners->bottom_right));

	const int horizontal_height = clipped_region_box->height - top - bottom;
	if (horizontal_height > 0) {
		pixman_region32_t horizontal;
		pixman_region32_init_rect(&horizontal,
			clipped_region_box->x, clipped_region_box->y + top,
			clipped_region_box->width, horizontal_height);
		pixman_region32_subtract(clip_region, clip_region, &horizontal);
		pixman_region32_fini(&horizontal);
	}

	const int vertical_width = clipped_region_box->width - left - right;
	if (vertical_width > 0) {
		pixman_region32_t vertical;
		pixman_region32_init_rect(&vertical,
			clipped_region_box->x + left, clipped_region_box->y,
			vertical_width, clipped_region_box->height);
		pixman_region32_subtract(clip_region, clip_region, &vertical);
		pixman_region32_fini(&vertical);
	}

	return true;
}

static bool color_primaries_equal(const struct wlr_color_primaries *a,
		const struct wlr_color_primaries *b) {
	return a->red.x == b->red.x && a->red.y == b->red.y &&
		a->green.x == b->green.x && a->green.y == b->green.y &&
		a->blue.x == b->blue.x && a->blue.y == b->blue.y &&
		a->white.x == b->white.x && a->white.y == b->white.y;
}

static void transpose_color_matrix(float out[static 9], const float matrix[static 9]) {
	out[0] = matrix[0];
	out[1] = matrix[3];
	out[2] = matrix[6];
	out[3] = matrix[1];
	out[4] = matrix[4];
	out[5] = matrix[7];
	out[6] = matrix[2];
	out[7] = matrix[5];
	out[8] = matrix[8];
}

struct output_transform_state {
	float matrix[9];
	enum wlr_color_transfer_function tf;
	struct wlr_color_transform_lut_3x1d *lut;
	bool has_matrix;
	bool has_eotf;
};

static bool output_transform_collect(struct output_transform_state *state,
		struct wlr_color_transform *transform) {
	switch (transform->type) {
	case COLOR_TRANSFORM_MATRIX: {
		if (state->has_matrix || state->has_eotf || state->lut != NULL) {
			return false;
		}
		struct wlr_color_transform_matrix *matrix =
			wl_container_of(transform, matrix, base);
		transpose_color_matrix(state->matrix, matrix->matrix);
		state->has_matrix = true;
		return true;
	}
	case COLOR_TRANSFORM_INVERSE_EOTF: {
		if (state->has_eotf || state->lut != NULL) {
			return false;
		}
		struct wlr_color_transform_inverse_eotf *eotf =
			wlr_color_transform_inverse_eotf_from_base(transform);
		state->tf = eotf->tf;
		state->has_eotf = true;
		return true;
	}
	case COLOR_TRANSFORM_LUT_3X1D:
		if (state->lut != NULL) {
			return false;
		}
		state->lut = color_transform_lut_3x1d_from_base(transform);
		return state->lut->dim > 0;
	case COLOR_TRANSFORM_PIPELINE: {
		struct wlr_color_transform_pipeline *pipeline =
			wl_container_of(transform, pipeline, base);
		for (size_t i = 0; i < pipeline->len; i++) {
			if (!output_transform_collect(state, pipeline->transforms[i])) {
				return false;
			}
		}
		return true;
	}
	case COLOR_TRANSFORM_LCMS2:
		return false;
	}
	return false;
}

static void log_unsupported_output_transform(void) {
	static struct timespec last_log;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (last_log.tv_sec != 0 && now.tv_sec - last_log.tv_sec < 5) {
		return;
	}
	last_log = now;
	wlr_log(WLR_ERROR, "Unsupported output color transform");
}

static bool upload_output_lut_texture(
		const struct wlr_color_transform_lut_3x1d *lut, GLuint *texture) {
	GLint max_size;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	if (lut->dim > (size_t)max_size || lut->dim > INT32_MAX / 6) {
		return false;
	}

	uint8_t *data = malloc(lut->dim * 3 * 2);
	if (data == NULL) {
		return false;
	}
	for (size_t i = 0; i < lut->dim * 3; i++) {
		data[i * 2] = lut->lut_3x1d[i] >> 8;
		data[i * 2 + 1] = lut->lut_3x1d[i] & 0xFF;
	}

	while (glGetError() != GL_NO_ERROR) {
	}
	glGenTextures(1, texture);
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, lut->dim, 3, 0,
		GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(data);
	bool upload_failed = false;
	while (glGetError() != GL_NO_ERROR) {
		upload_failed = true;
	}
	if (upload_failed || *texture == 0) {
		glDeleteTextures(1, texture);
		*texture = 0;
		while (glGetError() != GL_NO_ERROR) {
		}
		return false;
	}
	return true;
}

void fx_output_lut_destroy(struct fx_output_lut *lut) {
	wl_list_remove(&lut->link);
	wlr_addon_finish(&lut->addon);

	struct wlr_egl_context prev_ctx;
	if (wlr_egl_make_current(lut->renderer->egl, &prev_ctx)) {
		push_fx_debug(lut->renderer);
		glDeleteTextures(1, &lut->texture);
		pop_fx_debug(lut->renderer);
		wlr_egl_restore_context(&prev_ctx);
	} else {
		wlr_log(WLR_ERROR, "Failed to activate EGL context while destroying output LUT");
	}
	free(lut);
}

static void output_lut_handle_destroy(struct wlr_addon *addon) {
	struct fx_output_lut *lut = wl_container_of(addon, lut, addon);
	fx_output_lut_destroy(lut);
}

static const struct wlr_addon_interface output_lut_addon_impl = {
	.name = "fx_output_lut",
	.destroy = output_lut_handle_destroy,
};

static struct fx_output_lut *get_or_create_output_lut(
		struct fx_renderer *renderer,
		struct wlr_color_transform_lut_3x1d *transform) {
	struct wlr_addon *addon = wlr_addon_find(&transform->base.addons, renderer,
		&output_lut_addon_impl);
	if (addon != NULL) {
		struct fx_output_lut *lut = NULL;
		return wl_container_of(addon, lut, addon);
	}

	struct fx_output_lut *lut = calloc(1, sizeof(*lut));
	if (lut == NULL) {
		return NULL;
	}
	lut->renderer = renderer;
	lut->dim = transform->dim;
	if (!upload_output_lut_texture(transform, &lut->texture)) {
		free(lut);
		return NULL;
	}

	wlr_addon_init(&lut->addon, &transform->base.addons, renderer,
		&output_lut_addon_impl);
	wl_list_insert(&renderer->output_luts, &lut->link);
	return lut;
}

void fx_render_pass_add_texture(struct fx_gles_render_pass *pass,
		const struct fx_render_texture_options *fx_options) {
	const struct wlr_render_texture_options *options = &fx_options->base;
	struct fx_renderer *renderer = pass->buffer->renderer;
	struct fx_texture *texture = fx_get_texture(options->texture);

	struct tex_shader *shader = NULL;

	bool use_effects = !fx_corner_fradii_is_empty(&fx_options->corners)
		|| clipped_fregion_is_valid(&fx_options->clipped_region);
	// A non-empty sample box selects the clamp variant. Everything else runs
	// the same fetch as before the clamp existed, so a driver only ever sees
	// the extra uniform on fractionally cropped surfaces.
	bool use_clamp = !wlr_fbox_empty(&fx_options->sample_box);
	switch (texture->target) {
	case GL_TEXTURE_2D:
		if (texture->has_alpha) {
			shader = use_clamp
				? (use_effects
					? &renderer->shaders.tex_clamp_effects_rgba
					: &renderer->shaders.tex_clamp_rgba)
				: (use_effects
					? &renderer->shaders.tex_effects_rgba
					: &renderer->shaders.tex_rgba);
		} else {
			shader = use_clamp
				? (use_effects
					? &renderer->shaders.tex_clamp_effects_rgbx
					: &renderer->shaders.tex_clamp_rgbx)
				: (use_effects
					? &renderer->shaders.tex_effects_rgbx
					: &renderer->shaders.tex_rgbx);
		}
		break;
	case GL_TEXTURE_EXTERNAL_OES:
		// EGL_EXT_image_dma_buf_import_modifiers requires
		// GL_OES_EGL_image_external
		assert(renderer->exts.OES_egl_image_external);
		shader = use_clamp
			? (use_effects
				? &renderer->shaders.tex_clamp_effects_ext
				: &renderer->shaders.tex_clamp_ext)
			: (use_effects
				? &renderer->shaders.tex_effects_ext
				: &renderer->shaders.tex_ext);
		break;
	default:
		abort();
	}

	struct wlr_box dst_box;
	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	wlr_render_texture_options_get_dst_box(options, &dst_box);
	float alpha = wlr_render_texture_options_get_alpha(options);

	const struct wlr_box *clip_box = &dst_box;
	if (!wlr_box_empty(fx_options->clip_box)) {
		clip_box = fx_options->clip_box;
	}

	src_fbox.x /= options->texture->width;
	src_fbox.y /= options->texture->height;
	src_fbox.width /= options->texture->width;
	src_fbox.height /= options->texture->height;

	// Texel centers of the first and last sampleable texel, so a clamped
	// coordinate lands on a texel rather than between two.
	const struct wlr_fbox sample_box = fx_options->sample_box;
	const float sample_bounds[4] = {
		(sample_box.x + 0.5) / options->texture->width,
		(sample_box.y + 0.5) / options->texture->height,
		(sample_box.x + sample_box.width - 0.5) / options->texture->width,
		(sample_box.y + sample_box.height - 0.5) / options->texture->height,
	};

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("dst_box (WxH, X, Y): %dx%d, %d, %d",
			dst_box.width, dst_box.height, dst_box.x, dst_box.y);
	TRACY_ZONE_TEXT_f("clip_box (WxH, X, Y): %dx%d, %d, %d",
			clip_box->width, clip_box->height, clip_box->x, clip_box->y);
	TRACY_ZONE_TEXT_f("src_box (WxH, X, Y): %lfx%lf, %lf, %lf",
			src_fbox.width, src_fbox.height, src_fbox.x, src_fbox.y);
	TRACY_ZONE_TEXT_f("Shader Type: %s",
			use_effects ? (
			 shader == &renderer->shaders.tex_effects_rgba ? "Effects RGBA"
			 : shader == &renderer->shaders.tex_effects_rgbx ? "Effects RGBX"
			 : "Effects EXT"
			) : (
				shader == &renderer->shaders.tex_rgba ? "RGBA"
				: shader == &renderer->shaders.tex_rgbx ? "RGBX"
				: "EXT"
			)
		);
	push_fx_debug(renderer);

	if (options->wait_timeline != NULL) {
		int sync_file_fd =
			wlr_drm_syncobj_timeline_export_sync_file(options->wait_timeline, options->wait_point);
		if (sync_file_fd < 0) {
			TRACY_BOTH_ZONES_END_FAIL;
			return;
		}

		EGLSyncKHR sync = wlr_egl_create_sync(renderer->egl, sync_file_fd);
		close(sync_file_fd);
		if (sync == EGL_NO_SYNC_KHR) {
			TRACY_BOTH_ZONES_END_FAIL;
			return;
		}

		bool ok = wlr_egl_wait_sync(renderer->egl, sync);
		wlr_egl_destroy_sync(renderer->egl, sync);
		if (!ok) {
			TRACY_BOTH_ZONES_END_FAIL;
			return;
		}
	}

	bool has_alpha = texture->has_alpha || alpha < 1.0 || use_effects;
	TRACY_ZONE_TEXT_f("Has Alpha: %d", has_alpha);
	setup_blending(!has_alpha ? WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);

	pixman_region32_t clip_region;
	if (options->clip) {
		pixman_region32_init(&clip_region);
		pixman_region32_copy(&clip_region, options->clip);
	} else {
		pixman_region32_init_rect(&clip_region, dst_box.x, dst_box.y, dst_box.width, dst_box.height);
	}
	const struct wlr_box clipped_region_box = fx_options->clipped_region.area;
	struct fx_corner_fradii clipped_region_corners = fx_options->clipped_region.corners;
	apply_clip_region(&clip_region, &clipped_region_box, &clipped_region_corners);
	render_pass_mark_updated(pass, &dst_box, &clip_region);

	glUseProgram(shader->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);

	switch (options->filter_mode) {
	case WLR_SCALE_FILTER_BILINEAR:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case WLR_SCALE_FILTER_NEAREST:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	}

	glUniform1i(shader->tex, 0);
	glUniform1f(shader->alpha, alpha);

	struct wlr_color_primaries primaries_srgb;
	wlr_color_primaries_from_named(&primaries_srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	const float lum_multiplier = options->luminance_multiplier != NULL
		? *options->luminance_multiplier
		: 1.0f;
	const bool primaries_passthrough = options->primaries == NULL ||
		color_primaries_equal(options->primaries, &primaries_srgb);
	const enum wlr_color_transfer_function source_tf = options->transfer_function != 0
		? options->transfer_function
		: WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	const enum wlr_color_transfer_function target_tf = pass->has_color_transform
		? WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR
		: WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	const bool color_passthrough = source_tf == target_tf &&
		primaries_passthrough && lum_multiplier == 1.0f;

	float primaries_matrix[9];
	if (primaries_passthrough) {
		wlr_matrix_identity(primaries_matrix);
	} else {
		float matrix[9];
		wlr_color_primaries_transform_absolute_colorimetric(
			options->primaries, &primaries_srgb, matrix);
		transpose_color_matrix(primaries_matrix, matrix);
	}
	glUniform1i(shader->source_tf, color_passthrough ? 0 : source_tf);
	glUniformMatrix3fv(shader->primaries_matrix, 1, GL_FALSE, primaries_matrix);
	glUniform1f(shader->lum_multiplier, lum_multiplier);
	glUniform1i(shader->target_tf, color_passthrough ? 0 : target_tf);

	glUniform1f(shader->discard_transparent, fx_options->discard_transparent);
	if (use_clamp) {
		glUniform4f(shader->sample_bounds, sample_bounds[0], sample_bounds[1],
			sample_bounds[2], sample_bounds[3]);
	}

	if (use_effects) {
		struct fx_corner_fradii corners = fx_options->corners;

		glUniform2f(shader->effects.size, clip_box->width, clip_box->height);
		glUniform2f(shader->effects.position, clip_box->x, clip_box->y);
		uniform_corner_radii_set(&shader->effects.radius, &corners);

		glUniform2f(shader->effects.clip_size, clipped_region_box.width, clipped_region_box.height);
		glUniform2f(shader->effects.clip_position, clipped_region_box.x, clipped_region_box.y);
		uniform_corner_radii_set(&shader->effects.clip_radius, &clipped_region_corners);
	}

	set_proj_matrix(shader->proj, pass->projection_matrix, &dst_box);
	set_tex_matrix(shader->tex_proj, options->transform, &src_fbox);

	render(&dst_box, &clip_region, shader->pos_attrib);
	pixman_region32_fini(&clip_region);

	glBindTexture(texture->target, 0);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_rect(struct fx_gles_render_pass *pass,
		const struct fx_render_rect_options *fx_options) {
	const struct wlr_render_rect_options *options = &fx_options->base;

	struct fx_renderer *renderer = pass->buffer->renderer;

	const struct wlr_render_color converted_color = pass_color(pass, &options->color);
	const struct wlr_render_color *color = &converted_color;
	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = pass->buffer->buffer;
	wlr_render_rect_options_get_box(options, wlr_buffer, &box);

	const bool should_clip = clipped_fregion_is_valid(&fx_options->clipped_region);

	enum wlr_render_blend_mode blend_mode =
		(color->a == 1.0 && !should_clip) ? WLR_RENDER_BLEND_MODE_NONE : options->blend_mode;
	const bool use_fast_clear =
		blend_mode == WLR_RENDER_BLEND_MODE_NONE && // includes check for `should_clip`
		options->clip == NULL &&
		box.x == 0 && box.y == 0 &&
		box.width == wlr_buffer->width &&
		box.height == wlr_buffer->height;

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("Box (WxH, X, Y): %dx%d, %d, %d", box.width, box.height, box.x, box.y);
	TRACY_ZONE_TEXT_f("Color RGBA: %f, %f, %f, %f", color->r, color->g, color->b, color->a);
	TRACY_ZONE_TEXT_f("Use fast clear optimization: %d", use_fast_clear);

	push_fx_debug(renderer);
	if (use_fast_clear) {
		render_pass_mark_updated(pass, &box, NULL);
		glClearColor(color->r, color->g, color->b, color->a);
		glClear(GL_COLOR_BUFFER_BIT);
	} else {
		const struct wlr_box *clipped_region_box = &fx_options->clipped_region.area;
		const struct fx_corner_fradii *clipped_region_corners = &fx_options->clipped_region.corners;

		TRACY_ZONE_TEXT_f("Clip Box (WxH, X, Y): %dx%d, %d, %d",
				clipped_region_box->width, clipped_region_box->height,
				clipped_region_box->x, clipped_region_box->y);
		TRACY_ZONE_TEXT_f("Clip Box Corners (TL, TR, BL, BR): %f, %f, %f, %f",
				clipped_region_corners->top_left,
				clipped_region_corners->top_right,
				clipped_region_corners->bottom_left,
				clipped_region_corners->bottom_right);

		pixman_region32_t clip_region;
		if (options->clip) {
			pixman_region32_init(&clip_region);
			pixman_region32_copy(&clip_region, options->clip);
		} else {
			pixman_region32_init_rect(&clip_region, box.x, box.y, box.width, box.height);
		}

		apply_clip_region(&clip_region, clipped_region_box, clipped_region_corners);
		render_pass_mark_updated(pass, &box, &clip_region);

		setup_blending(blend_mode);
		struct quad_shader *shader = should_clip
			? &renderer->shaders.quad_clip
			: &renderer->shaders.quad;
		glUseProgram(shader->program);
		set_proj_matrix(shader->proj, pass->projection_matrix, &box);
		glUniform4f(shader->color, color->r, color->g, color->b, color->a);
		if (should_clip) {
			glUniform2f(shader->effects.clip_size, clipped_region_box->width, clipped_region_box->height);
			glUniform2f(shader->effects.clip_position, clipped_region_box->x, clipped_region_box->y);
			uniform_corner_radii_set(&shader->effects.clip_radius, clipped_region_corners);
		}
		render(&box, &clip_region, shader->pos_attrib);

		pixman_region32_fini(&clip_region);
	}

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_rect_grad(struct fx_gles_render_pass *pass,
		const struct fx_render_rect_grad_options *fx_options) {
	const struct wlr_render_rect_options *options = &fx_options->base;

	struct fx_renderer *renderer = pass->buffer->renderer;

	if (renderer->shaders.quad_grad.max_len <= fx_options->gradient.count) {
		glDeleteProgram(renderer->shaders.quad_grad.program);
		if (!link_quad_grad_program(&renderer->shaders.quad_grad, fx_options->gradient.count + 1)) {
			wlr_log(WLR_ERROR, "Could not link quad shader after updating max_len to %d. Aborting renderer", fx_options->gradient.count + 1);
			abort();
		}
	}

	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = pass->buffer->buffer;
	wlr_render_rect_options_get_box(options, wlr_buffer, &box);
	float *gradient_colors = pass_gradient_colors(pass, &fx_options->gradient);
	if (gradient_colors == NULL) {
		return;
	}
	render_pass_mark_updated(pass, &box, options->clip);

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("Box (WxH, X, Y): %dx%d, %d, %d", box.width, box.height, box.x, box.y);
	TRACY_ZONE_TEXT_f("Gradient:");
	TRACY_ZONE_TEXT_f("\tNum Colors: %d", fx_options->gradient.count);
	TRACY_ZONE_TEXT_f("\tBlend: %d", fx_options->gradient.blend);
	TRACY_ZONE_TEXT_f("\tDegree: %f", fx_options->gradient.degree);
	TRACY_ZONE_TEXT_f("\tType: %s",
			fx_options->gradient.linear == 1 ? "Linear"
			: fx_options->gradient.linear == 2 ? "Conic"
			: "Unknown");
	TRACY_ZONE_TEXT_f("\tOrigin: %fx%f",
			fx_options->gradient.origin[0], fx_options->gradient.origin[1]);
	TRACY_ZONE_TEXT_f("\tRange (WxH, X, Y): %dx%d, %d, %d",
			fx_options->gradient.range.width, fx_options->gradient.range.height,
			fx_options->gradient.range.x, fx_options->gradient.range.y);
	// TODO: Display Colors (not really sure how it works without a scene example...)
	push_fx_debug(renderer);

	setup_blending(options->blend_mode);

	struct quad_grad_shader shader = renderer->shaders.quad_grad;
	glUseProgram(shader.program);

	set_proj_matrix(shader.proj, pass->projection_matrix, &box);
	glUniform4fv(shader.colors, fx_options->gradient.count, gradient_colors);
	glUniform1i(shader.count, fx_options->gradient.count);
	glUniform2f(shader.size, fx_options->gradient.range.width, fx_options->gradient.range.height);
	glUniform1f(shader.degree, fx_options->gradient.degree);
	glUniform1f(shader.linear, fx_options->gradient.linear);
	glUniform1f(shader.blend, fx_options->gradient.blend);
	glUniform2f(shader.grad_box, fx_options->gradient.range.x, fx_options->gradient.range.y);
	glUniform2f(shader.origin, fx_options->gradient.origin[0], fx_options->gradient.origin[1]);

	render(&box, options->clip, shader.pos_attrib);
	if (gradient_colors != fx_options->gradient.colors) {
		free(gradient_colors);
	}

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_rounded_rect(struct fx_gles_render_pass *pass,
		const struct fx_render_rounded_rect_options *fx_options) {
	const struct wlr_render_rect_options *options = &fx_options->base;

	struct fx_renderer *renderer = pass->buffer->renderer;

	const struct wlr_render_color converted_color = pass_color(pass, &options->color);
	const struct wlr_render_color *color = &converted_color;
	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = pass->buffer->buffer;
	wlr_render_rect_options_get_box(options, wlr_buffer, &box);

	pixman_region32_t clip_region;
	if (options->clip) {
		pixman_region32_init(&clip_region);
		pixman_region32_copy(&clip_region, options->clip);
	} else {
		pixman_region32_init_rect(&clip_region, box.x, box.y, box.width, box.height);
	}
	const struct wlr_box *clipped_region_box = &fx_options->clipped_region.area;
	const struct fx_corner_fradii *clipped_region_corners = &fx_options->clipped_region.corners;
	apply_clip_region(&clip_region, clipped_region_box, clipped_region_corners);
	render_pass_mark_updated(pass, &box, &clip_region);

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("Box (WxH, X, Y): %dx%d, %d, %d", box.width, box.height, box.x, box.y);
	TRACY_ZONE_TEXT_f("Clip Box (WxH, X, Y): %dx%d, %d, %d",
			clipped_region_box->width, clipped_region_box->height,
			clipped_region_box->x, clipped_region_box->y);
	TRACY_ZONE_TEXT_f("Clip Box Corners (TL, TR, BL, BR): %f, %f, %f, %f",
			clipped_region_corners->top_left,
			clipped_region_corners->top_right,
			clipped_region_corners->bottom_left,
			clipped_region_corners->bottom_right);
	TRACY_ZONE_TEXT_f("Color RGBA: %f, %f, %f, %f", color->r, color->g, color->b, color->a);
	TRACY_ZONE_TEXT_f("Corners (TL, TR, BL, BR): %f, %f, %f, %f",
			clipped_region_corners->top_left,
			clipped_region_corners->top_right,
			clipped_region_corners->bottom_left,
			clipped_region_corners->bottom_right);
	push_fx_debug(renderer);

	setup_blending(WLR_RENDER_BLEND_MODE_PREMULTIPLIED);

	struct quad_round_shader shader = renderer->shaders.quad_round;

	glUseProgram(shader.program);

	set_proj_matrix(shader.proj, pass->projection_matrix, &box);
	glUniform4f(shader.color, color->r, color->g, color->b, color->a);

	glUniform2f(shader.size, box.width, box.height);
	glUniform2f(shader.position, box.x, box.y);
	glUniform2f(shader.clip_size, clipped_region_box->width, clipped_region_box->height);
	glUniform2f(shader.clip_position, clipped_region_box->x, clipped_region_box->y);
	uniform_corner_radii_set(&shader.clip_radius, clipped_region_corners);

	struct fx_corner_fradii corners = fx_options->corners;
	uniform_corner_radii_set(&shader.radius, &corners);

	render(&box, &clip_region, renderer->shaders.quad_round.pos_attrib);
	pixman_region32_fini(&clip_region);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_border(struct fx_gles_render_pass *pass,
		const struct fx_render_border_options *options) {
	struct fx_renderer *renderer = pass->buffer->renderer;
	const struct wlr_render_color inner_color =
		pass_color(pass, &options->inner_color);
	const struct wlr_render_color outer_color =
		pass_color(pass, &options->outer_color);

	pixman_region32_t clip_region;
	if (options->clip != NULL) {
		pixman_region32_init(&clip_region);
		pixman_region32_copy(&clip_region, options->clip);
	} else {
		pixman_region32_init_rect(&clip_region,
			options->box.x, options->box.y,
			options->box.width, options->box.height);
	}
	apply_clip_region(&clip_region,
		&options->clipped_region.area, &options->clipped_region.corners);
	render_pass_mark_updated(pass, &options->box, &clip_region);

	TRACY_BOTH_ZONES_START(renderer);
	push_fx_debug(renderer);
	setup_blending(WLR_RENDER_BLEND_MODE_PREMULTIPLIED);

	struct border_shader shader = renderer->shaders.border;
	glUseProgram(shader.program);
	set_proj_matrix(shader.proj, pass->projection_matrix, &options->box);
	glUniform4f(shader.color,
		outer_color.r, outer_color.g, outer_color.b, outer_color.a);
	glUniform4f(shader.inner_color,
		inner_color.r, inner_color.g, inner_color.b, inner_color.a);
	glUniform2f(shader.clip_size,
		options->clipped_region.area.width,
		options->clipped_region.area.height);
	glUniform2f(shader.clip_position,
		options->clipped_region.area.x,
		options->clipped_region.area.y);
	uniform_corner_radii_set(&shader.inner_radius,
		&options->clipped_region.corners);
	uniform_corner_radii_set(&shader.seam_radius,
		&options->seam_corners);
	uniform_corner_radii_set(&shader.outer_radius,
		&options->outer_corners);
	glUniform1f(shader.inner_width, options->inner_width);
	glUniform1f(shader.outer_width, options->outer_width);

	render(&options->box, &clip_region, shader.pos_attrib);
	pixman_region32_fini(&clip_region);
	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_rounded_rect_grad(struct fx_gles_render_pass *pass,
		const struct fx_render_rounded_rect_grad_options *fx_options) {
	const struct wlr_render_rect_options *options = &fx_options->base;

	struct fx_renderer *renderer = pass->buffer->renderer;

	if (renderer->shaders.quad_grad_round.max_len <= fx_options->gradient.count) {
		glDeleteProgram(renderer->shaders.quad_grad_round.program);
		if (!link_quad_grad_round_program(&renderer->shaders.quad_grad_round, fx_options->gradient.count + 1)) {
			wlr_log(WLR_ERROR, "Could not link quad shader after updating max_len to %d. Aborting renderer", fx_options->gradient.count + 1);
			abort();
		}
	}

	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = pass->buffer->buffer;
	wlr_render_rect_options_get_box(options, wlr_buffer, &box);
	float *gradient_colors = pass_gradient_colors(pass, &fx_options->gradient);
	if (gradient_colors == NULL) {
		return;
	}
	render_pass_mark_updated(pass, &box, options->clip);

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("Box (WxH, X, Y): %dx%d, %d, %d", box.width, box.height, box.x, box.y);
	TRACY_ZONE_TEXT_f("Corners (TL, TR, BL, BR): %f, %f, %f, %f",
			fx_options->corners.top_left,
			fx_options->corners.top_right,
			fx_options->corners.bottom_left,
			fx_options->corners.bottom_right);
	TRACY_ZONE_TEXT_f("Gradient:");
	TRACY_ZONE_TEXT_f("\tNum Colors: %d", fx_options->gradient.count);
	TRACY_ZONE_TEXT_f("\tBlend: %d", fx_options->gradient.blend);
	TRACY_ZONE_TEXT_f("\tDegree: %f", fx_options->gradient.degree);
	TRACY_ZONE_TEXT_f("\tType: %s",
			fx_options->gradient.linear == 1 ? "Linear"
			: fx_options->gradient.linear == 2 ? "Conic"
			: "Unknown");
	TRACY_ZONE_TEXT_f("\tOrigin: %fx%f",
			fx_options->gradient.origin[0], fx_options->gradient.origin[1]);
	TRACY_ZONE_TEXT_f("\tRange (WxH, X, Y): %dx%d, %d, %d",
			fx_options->gradient.range.width, fx_options->gradient.range.height,
			fx_options->gradient.range.x, fx_options->gradient.range.y);
	// TODO: Display Colors (not really sure how it works without a scene example...)
	push_fx_debug(renderer);

	setup_blending(WLR_RENDER_BLEND_MODE_PREMULTIPLIED);

	struct quad_grad_round_shader shader = renderer->shaders.quad_grad_round;
	glUseProgram(shader.program);

	set_proj_matrix(shader.proj, pass->projection_matrix, &box);

	glUniform2f(shader.size, box.width, box.height);
	glUniform2f(shader.position, box.x, box.y);

	glUniform4fv(shader.colors, fx_options->gradient.count, gradient_colors);
	glUniform1i(shader.count, fx_options->gradient.count);
	glUniform2f(shader.grad_size, fx_options->gradient.range.width, fx_options->gradient.range.height);
	glUniform1f(shader.degree, fx_options->gradient.degree);
	glUniform1f(shader.linear, fx_options->gradient.linear);
	glUniform1f(shader.blend, fx_options->gradient.blend);
	glUniform2f(shader.grad_box, fx_options->gradient.range.x, fx_options->gradient.range.y);
	glUniform2f(shader.origin, fx_options->gradient.origin[0], fx_options->gradient.origin[1]);

	struct fx_corner_fradii corners = fx_options->corners;
	uniform_corner_radii_set(&shader.radius, &corners);

	render(&box, options->clip, shader.pos_attrib);
	if (gradient_colors != fx_options->gradient.colors) {
		free(gradient_colors);
	}

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

void fx_render_pass_add_box_shadow(struct fx_gles_render_pass *pass,
		const struct fx_render_box_shadow_options *options) {
	struct fx_renderer *renderer = pass->buffer->renderer;

	struct wlr_box box = options->box;
	assert(box.width > 0 && box.height > 0);

	pixman_region32_t clip_region;
	if (options->clip) {
		pixman_region32_init(&clip_region);
		pixman_region32_copy(&clip_region, options->clip);
	} else {
		pixman_region32_init_rect(&clip_region, box.x, box.y, box.width, box.height);
	}
	const struct wlr_box clipped_region_box = options->clipped_region.area;
	struct fx_corner_fradii clipped_region_corners = options->clipped_region.corners;
	apply_clip_region(&clip_region, &clipped_region_box, &clipped_region_corners);
	render_pass_mark_updated(pass, &box, &clip_region);

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("Box (WxH, X, Y): %dx%d, %d, %d", box.width, box.height, box.x, box.y);
	TRACY_ZONE_TEXT_f("Clip Box (WxH, X, Y): %dx%d, %d, %d",
			clipped_region_box.width, clipped_region_box.height,
			clipped_region_box.x, clipped_region_box.y);
	TRACY_ZONE_TEXT_f("Clip Box Corners (TL, TR, BL, BR): %f, %f, %f, %f",
			clipped_region_corners.top_left,
			clipped_region_corners.top_right,
			clipped_region_corners.bottom_left,
			clipped_region_corners.bottom_right);
	TRACY_ZONE_TEXT_f("Shadow Options:");
	TRACY_ZONE_TEXT_f("\tColor RGBA: %f, %f, %f, %f",
			options->color.r, options->color.g, options->color.b, options->color.a);
	TRACY_ZONE_TEXT_f("\tBlur Sigma: %f", options->blur_sigma);
	push_fx_debug(renderer);

	// blending will practically always be needed (unless we have a madman
	// who uses opaque shadows with zero sigma), so just enable it
	setup_blending(WLR_RENDER_BLEND_MODE_PREMULTIPLIED);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(renderer->shaders.box_shadow.program);

	const struct wlr_render_color converted_color = pass_color(pass, &options->color);
	const struct wlr_render_color *color = &converted_color;
	set_proj_matrix(renderer->shaders.box_shadow.proj, pass->projection_matrix, &box);
	glUniform4f(renderer->shaders.box_shadow.color, color->r, color->g, color->b, color->a);
	glUniform1f(renderer->shaders.box_shadow.blur_sigma, options->blur_sigma);
	glUniform2f(renderer->shaders.box_shadow.size, box.width, box.height);
	glUniform2f(renderer->shaders.box_shadow.position, box.x, box.y);
	glUniform1f(renderer->shaders.box_shadow.corner_radius, options->corner_radius);

	uniform_corner_radii_set(&renderer->shaders.box_shadow.clip_radius, &clipped_region_corners);

	glUniform2f(renderer->shaders.box_shadow.clip_position, clipped_region_box.x, clipped_region_box.y);
	glUniform2f(renderer->shaders.box_shadow.clip_size, clipped_region_box.width, clipped_region_box.height);

	render(&box, &clip_region, renderer->shaders.box_shadow.pos_attrib);
	pixman_region32_fini(&clip_region);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

// Renders the blur for each damaged rect and swaps the buffer
static void render_blur_segments(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options, struct blur_shader* shader,
		int sample_divisor) {
	struct fx_render_texture_options *tex_options = &fx_options->tex_options;
	struct wlr_render_texture_options *options = &tex_options->base;
	struct fx_renderer *renderer = pass->buffer->renderer;
	struct blur_data *blur_data = fx_options->blur_data;

	TRACY_BOTH_ZONES_START(renderer);
	push_fx_debug(renderer);

	// Swap fbo
	if (fx_options->current_buffer == pass->fx_offscreen_buffers->effects_buffer) {
		fx_framebuffer_bind(pass->fx_offscreen_buffers->effects_buffer_swapped);
	} else {
		fx_framebuffer_bind(pass->fx_offscreen_buffers->effects_buffer);
	}

	options->texture = fx_texture_from_buffer(&renderer->wlr_renderer,
			fx_options->current_buffer->buffer);
	struct fx_texture *texture = fx_get_texture(options->texture);

	/*
	 * Render
	 */

	struct wlr_box dst_box;
	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	wlr_render_texture_options_get_dst_box(options, &dst_box);
	src_fbox.x /= options->texture->width;
	src_fbox.y /= options->texture->height;
	src_fbox.width /= options->texture->width;
	src_fbox.height /= options->texture->height;

	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(shader->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);

	switch (options->filter_mode) {
	case WLR_SCALE_FILTER_BILINEAR:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case WLR_SCALE_FILTER_NEAREST:
		abort();
	}

	glUniform1i(shader->tex, 0);
	glUniform1f(shader->radius, blur_data->radius);
	// Reduced blur levels occupy the top-left of full-size textures. Clamp to
	// that level's edge texels so the kernel never samples the unused remainder.
	const int sample_width = options->texture->width / sample_divisor
		+ (options->texture->width % sample_divisor != 0);
	const int sample_height = options->texture->height / sample_divisor
		+ (options->texture->height % sample_divisor != 0);
	glUniform4f(shader->sample_bounds,
			0.5f / options->texture->width,
			0.5f / options->texture->height,
			(sample_width - 0.5f) / options->texture->width,
			(sample_height - 0.5f) / options->texture->height);

	if (shader == &renderer->shaders.blur1) {
		glUniform2f(shader->halfpixel,
				0.5f / (options->texture->width / 2.0f),
				0.5f / (options->texture->height / 2.0f));
	} else {
		glUniform2f(shader->halfpixel,
				0.5f / (options->texture->width * 2.0f),
				0.5f / (options->texture->height * 2.0f));
	}

	set_proj_matrix(shader->proj, pass->projection_matrix, &dst_box);
	set_tex_matrix(shader->tex_proj, options->transform, &src_fbox);

	render(&dst_box, options->clip, shader->pos_attrib);

	glBindTexture(texture->target, 0);
	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;

	wlr_texture_destroy(options->texture);

	// Swap buffer. We don't want to draw to the same buffer
	if (fx_options->current_buffer != pass->fx_offscreen_buffers->effects_buffer) {
		fx_options->current_buffer = pass->fx_offscreen_buffers->effects_buffer;
	} else {
		fx_options->current_buffer = pass->fx_offscreen_buffers->effects_buffer_swapped;
	}
}

static void render_blur_effects(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options) {
	struct fx_render_texture_options *tex_options = &fx_options->tex_options;
	struct wlr_render_texture_options *options = &tex_options->base;
	struct fx_renderer *renderer = pass->buffer->renderer;
	struct blur_data *blur_data = fx_options->blur_data;
	struct fx_texture *texture = fx_get_texture(options->texture);

	struct blur_effects_shader shader = renderer->shaders.blur_effects;

	struct wlr_box dst_box;
	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	wlr_render_texture_options_get_dst_box(options, &dst_box);

	src_fbox.x /= options->texture->width;
	src_fbox.y /= options->texture->height;
	src_fbox.width /= options->texture->width;
	src_fbox.height /= options->texture->height;

	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);

	TRACY_BOTH_ZONES_START(renderer);
	push_fx_debug(renderer);

	glUseProgram(shader.program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);

	switch (options->filter_mode) {
	case WLR_SCALE_FILTER_BILINEAR:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case WLR_SCALE_FILTER_NEAREST:
		abort();
	}

	glUniform1i(shader.tex, 0);
	glUniform1f(shader.noise, blur_data->noise);
	glUniform1f(shader.brightness, blur_data->brightness);
	glUniform1f(shader.contrast, blur_data->contrast);
	glUniform1f(shader.saturation, blur_data->saturation);
	glUniform1i(shader.linear, pass->has_color_transform);

	set_proj_matrix(shader.proj, pass->projection_matrix, &dst_box);
	set_tex_matrix(shader.tex_proj, options->transform, &src_fbox);

	render(&dst_box, options->clip, shader.pos_attrib);

	glBindTexture(texture->target, 0);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;

	wlr_texture_destroy(options->texture);
}

// Blurs the fx_options current_buffer content and returns the blurred framebuffer.
// Returns NULL when the blur parameters reach 0.
static struct fx_framebuffer *get_main_buffer_blur(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options) {
	if (pass->fx_offscreen_buffers == NULL) {
		wlr_log(WLR_ERROR, "FX Pass offscreen buffers not initialized. Skipping getting blur...");
		return NULL;
	}

	struct fx_render_blur_pass_options local_options = *fx_options;
	fx_options = &local_options;

	struct fx_renderer *renderer = pass->buffer->renderer;
	struct wlr_box buffer_bounds = {
		0, 0,
		fx_options->current_buffer->buffer->width, fx_options->current_buffer->buffer->height
	};

	// We don't want to affect the reference blur_data
	struct blur_data blur_data = blur_data_apply_strength(fx_options->blur_data, fx_options->blur_strength);
	if (fx_options->blur_strength <= 0 || !is_scene_blur_enabled(&blur_data)) {
		return NULL;
	}
	fx_options->blur_data = &blur_data;

	struct fx_offscreen_buffers *fbos = pass->fx_offscreen_buffers;
	if (ensure_offscreen_buffer(pass, &fbos->effects_buffer, true) == NULL
			|| ensure_offscreen_buffer(pass, &fbos->effects_buffer_swapped, true) == NULL) {
		return NULL;
	}

	// The clip and current buffer are already in physical render-target
	// coordinates. The incoming transform belongs to the optional surface mask
	// and must not rotate either the full-framebuffer samples or their damage.
	fx_options->tex_options.base.transform = WL_OUTPUT_TRANSFORM_NORMAL;

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_copy(&damage, fx_options->tex_options.base.clip);

	wlr_region_expand(&damage, &damage, blur_data_calc_size(&blur_data));
	// Make sure that the region doesn't expand past the buffer bounds
	pixman_region32_intersect_rect(&damage, &damage,
			0, 0, buffer_bounds.width, buffer_bounds.height);

	// damage region will be scaled, make a temp
	pixman_region32_t scaled_damage;
	pixman_region32_init(&scaled_damage);

	fx_options->tex_options.base.src_box = (struct wlr_fbox) {
		0, 0,
		buffer_bounds.width, buffer_bounds.height,
	};
	fx_options->tex_options.base.dst_box = buffer_bounds;
	// Clip the blur to the damage
	fx_options->tex_options.base.clip = &scaled_damage;
	// Artifacts with NEAREST filter
	fx_options->tex_options.base.filter_mode = WLR_SCALE_FILTER_BILINEAR;

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("dst_box (WxH, X, Y): %dx%d, %d, %d",
			fx_options->tex_options.base.dst_box.width,
			fx_options->tex_options.base.dst_box.height,
			fx_options->tex_options.base.dst_box.x,
			fx_options->tex_options.base.dst_box.y);
	TRACY_ZONE_TEXT_f("clip_box (WxH, X, Y): %dx%d, %d, %d",
			fx_options->tex_options.clip_box->width,
			fx_options->tex_options.clip_box->height,
			fx_options->tex_options.clip_box->x,
			fx_options->tex_options.clip_box->y);
	TRACY_ZONE_TEXT_f("Corners (TL, TR, BL, BR): %f, %f, %f, %f",
			fx_options->corners.top_left,
			fx_options->corners.top_right,
			fx_options->corners.bottom_left,
			fx_options->corners.bottom_right);
	TRACY_ZONE_TEXT_f("src_box (WxH, X, Y): %lfx%lf, %lf, %lf",
			fx_options->tex_options.base.src_box.width,
			fx_options->tex_options.base.src_box.height,
			fx_options->tex_options.base.src_box.x,
			fx_options->tex_options.base.src_box.y);
	TRACY_ZONE_TEXT_f("Ignore Alpha: %f", fx_options->ignore_alpha);
	TRACY_ZONE_TEXT_f("Discard Transparent: %d", fx_options->tex_options.discard_transparent);
	TRACY_ZONE_TEXT_f("Use Optimized Blur: %d", fx_options->use_optimized_blur);
	TRACY_ZONE_TEXT_f("Blur Options:");
	TRACY_ZONE_TEXT_f("\tNum Blur Passes: %d", fx_options->blur_data->num_passes);
	TRACY_ZONE_TEXT_f("\tBlur Radius: %f", fx_options->blur_data->radius);
	TRACY_ZONE_TEXT_f("\tBrightness: %f", fx_options->blur_data->brightness);
	TRACY_ZONE_TEXT_f("\tContrast: %f", fx_options->blur_data->contrast);
	TRACY_ZONE_TEXT_f("\tNoise: %f", fx_options->blur_data->noise);
	TRACY_ZONE_TEXT_f("\tSaturation: %f", fx_options->blur_data->saturation);
	push_fx_debug(renderer);

	// Downscale
	for (int i = 0; i < blur_data.num_passes; ++i) {
		wlr_region_scale(&scaled_damage, &damage, 1.0f / (1 << (i + 1)));
		render_blur_segments(pass, fx_options, &renderer->shaders.blur1, 1 << i);
	}

	// Upscale
	for (int i = blur_data.num_passes - 1; i >= 0; --i) {
		// when upsampling we make the region twice as big
		wlr_region_scale(&scaled_damage, &damage, 1.0f / (1 << i));
		render_blur_segments(pass, fx_options, &renderer->shaders.blur2, 1 << (i + 1));
	}

	pixman_region32_fini(&scaled_damage);

	// Render additional blur effects like saturation, noise, contrast, etc...
	if (blur_data_should_parameters_blur_effects(&blur_data)
			&& pixman_region32_not_empty(&damage)) {
		if (fx_options->current_buffer == pass->fx_offscreen_buffers->effects_buffer) {
			fx_framebuffer_bind(pass->fx_offscreen_buffers->effects_buffer_swapped);
		} else {
			fx_framebuffer_bind(pass->fx_offscreen_buffers->effects_buffer);
		}
		fx_options->tex_options.base.clip = &damage;
		fx_options->tex_options.base.texture = fx_texture_from_buffer(
				&renderer->wlr_renderer, fx_options->current_buffer->buffer);
		render_blur_effects(pass, fx_options);
		if (fx_options->current_buffer != pass->fx_offscreen_buffers->effects_buffer) {
			fx_options->current_buffer = pass->fx_offscreen_buffers->effects_buffer;
		} else {
			fx_options->current_buffer = pass->fx_offscreen_buffers->effects_buffer_swapped;
		}
	}

	pixman_region32_fini(&damage);

	// Bind back to the default buffer
	fx_framebuffer_bind(pass->buffer);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;

	return fx_options->current_buffer;
}

static bool optimized_buffer_ready(const struct fx_gles_render_pass *pass,
		const struct fx_framebuffer *buffer) {
	return buffer != NULL
		&& buffer->buffer->width == pass->buffer->buffer->width
		&& buffer->buffer->height == pass->buffer->buffer->height;
}

void fx_render_pass_add_blur(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options) {
	if (pass->fx_offscreen_buffers == NULL) {
		wlr_log(WLR_ERROR, "FX Pass offscreen buffers not initialized. Skipping blur...");
		return;
	}

	struct fx_renderer *renderer = pass->buffer->renderer;
	struct fx_render_texture_options *tex_options = &fx_options->tex_options;

	TRACY_BOTH_ZONES_START(renderer);
	push_fx_debug(renderer);

	const bool has_strength = fx_options->blur_strength < 1.0;
	struct fx_offscreen_buffers *fbos = pass->fx_offscreen_buffers;
	// The optimized buffers only hold content once an optimized blur node has
	// rendered on this output at the current size. Without that, blur the live
	// pass target rather than an empty buffer.
	const bool use_optimized = fx_options->use_optimized_blur
		&& optimized_buffer_ready(pass, fbos->optimized_blur_buffer)
		&& optimized_buffer_ready(pass, fbos->optimized_no_blur_buffer);
	struct fx_framebuffer *buffer = NULL;
	TRACY_ZONE_TEXT_f("Use Optimized Blur: %d", fx_options->use_optimized_blur);
	if (!use_optimized || has_strength) {
		// Render the blur into its own buffer
		struct fx_render_blur_pass_options blur_options = *fx_options;
		if (use_optimized) {
			// Re-blur the saved non-blurred version of the optimized blur.
			// Isn't as efficient as just using the optimized blur buffer
			blur_options.current_buffer = fbos->optimized_no_blur_buffer;
		} else {
			blur_options.current_buffer = pass->buffer;
		}
		buffer = get_main_buffer_blur(pass, &blur_options);
	} else {
		buffer = fbos->optimized_blur_buffer;
	}
	TRACY_ZONE_TEXT_f("Optimized Blur Successfully Used: %d", buffer && use_optimized);
	if (!buffer) {
		goto finish;
	}
	struct wlr_texture *wlr_texture =
		fx_texture_from_buffer(&renderer->wlr_renderer, buffer->buffer);
	struct fx_texture *blur_texture = fx_get_texture(wlr_texture);

	// Get a stencil of the window ignoring transparent regions
	bool masked = false;
	if (fx_options->ignore_alpha > 0.0f && fx_options->tex_options.base.texture) {
		masked = stencil_mask_init(pass);
	}
	if (masked) {
		struct fx_render_texture_options tex_options = fx_options->tex_options;
		tex_options.discard_transparent = fx_options->ignore_alpha;
		tex_options.clipped_region = fx_options->clipped_region;
		fx_render_pass_add_texture(pass, &tex_options);

		stencil_mask_close(true);
	}

	// Draw the blurred texture
	tex_options->base.dst_box = (struct wlr_box) {
		.x = 0,
		.y = 0,
		.width = buffer->buffer->width,
		.height = buffer->buffer->height,
	};
	tex_options->base.src_box = (struct wlr_fbox) {
		.x = 0,
		.y = 0,
		.width = buffer->buffer->width,
		.height = buffer->buffer->height,
	};
	tex_options->base.texture = &blur_texture->wlr_texture;
	if (pass->has_color_transform) {
		tex_options->base.transfer_function =
			WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
	}
	// since we're capturing from the fbo, transform will always be normal
	tex_options->base.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	tex_options->clipped_region = fx_options->clipped_region;
	fx_render_pass_add_texture(pass, tex_options);

	wlr_texture_destroy(&blur_texture->wlr_texture);

	// Finish stenciling
	if (masked) {
		stencil_mask_fini();
	}

finish:
	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
}

bool fx_render_pass_add_optimized_blur(struct fx_gles_render_pass *pass,
		struct fx_render_blur_pass_options *fx_options) {
	if (pass->fx_offscreen_buffers == NULL) {
		wlr_log(WLR_ERROR, "FX Pass offscreen buffers not initialized. Skipping optimized blur...");
		return false;
	}
	struct fx_renderer *renderer = pass->buffer->renderer;
	struct wlr_box dst_box = fx_options->tex_options.base.dst_box;

	TRACY_BOTH_ZONES_START(renderer);
	TRACY_ZONE_TEXT_f("dst_box (WxH, X, Y): %dx%d, %d, %d",
			fx_options->tex_options.base.dst_box.width,
			fx_options->tex_options.base.dst_box.height,
			fx_options->tex_options.base.dst_box.x,
			fx_options->tex_options.base.dst_box.y);
	TRACY_ZONE_TEXT_f("clip_box (WxH, X, Y): %dx%d, %d, %d",
			fx_options->tex_options.clip_box->width,
			fx_options->tex_options.clip_box->height,
			fx_options->tex_options.clip_box->x,
			fx_options->tex_options.clip_box->y);
	TRACY_ZONE_TEXT_f("src_box (WxH, X, Y): %lfx%lf, %lf, %lf",
			fx_options->tex_options.base.src_box.width,
			fx_options->tex_options.base.src_box.height,
			fx_options->tex_options.base.src_box.x,
			fx_options->tex_options.base.src_box.y);
	TRACY_ZONE_TEXT_f("Ignore Alpha: %f", fx_options->ignore_alpha);
	TRACY_ZONE_TEXT_f("Discard Transparent: %d", fx_options->tex_options.discard_transparent);
	TRACY_ZONE_TEXT_f("Use Optimized Blur: %d", fx_options->use_optimized_blur);
	TRACY_ZONE_TEXT_f("Blur Options:");
	TRACY_ZONE_TEXT_f("\tNum Blur Passes: %d", fx_options->blur_data->num_passes);
	TRACY_ZONE_TEXT_f("\tBlur Radius: %f", fx_options->blur_data->radius);
	TRACY_ZONE_TEXT_f("\tBrightness: %f", fx_options->blur_data->brightness);
	TRACY_ZONE_TEXT_f("\tContrast: %f", fx_options->blur_data->contrast);
	TRACY_ZONE_TEXT_f("\tNoise: %f", fx_options->blur_data->noise);
	TRACY_ZONE_TEXT_f("\tSaturation: %f", fx_options->blur_data->saturation);
	push_fx_debug(renderer);

	pixman_region32_t clip;
	pixman_region32_init_rect(&clip,
			dst_box.x, dst_box.y, dst_box.width, dst_box.height);

	// Render the blur into its own buffer
	struct fx_offscreen_buffers *fbos = pass->fx_offscreen_buffers;
	struct fx_framebuffer *blur_buffer =
		ensure_offscreen_buffer(pass, &fbos->optimized_blur_buffer, false);
	struct fx_framebuffer *no_blur_buffer =
		ensure_offscreen_buffer(pass, &fbos->optimized_no_blur_buffer, false);
	struct fx_framebuffer *fx_buffer = NULL;
	if (blur_buffer != NULL && no_blur_buffer != NULL) {
		struct fx_render_blur_pass_options blur_options = *fx_options;
		blur_options.current_buffer = pass->buffer;
		blur_options.tex_options.base.clip = &clip;
		fx_buffer = get_main_buffer_blur(pass, &blur_options);
	}
	if (fx_buffer != NULL) {
		// Render the newly blurred content into the blur_buffer
		fx_render_pass_read_to_buffer(pass, &clip, blur_buffer, fx_buffer);

		// Save the current scene pass state
		fx_render_pass_read_to_buffer(pass, &clip, no_blur_buffer, pass->buffer);
	}

	pixman_region32_fini(&clip);

	pop_fx_debug(renderer);
	TRACY_BOTH_ZONES_END;
	return fx_buffer != NULL;
}

void fx_render_pass_read_to_buffer(struct fx_gles_render_pass *pass,
		pixman_region32_t *_region, struct fx_framebuffer *dst_buffer,
		struct fx_framebuffer *src_buffer) {
	if (!_region || !pixman_region32_not_empty(_region)) {
		return;
	}
	TRACY_BOTH_ZONES_START(pass->buffer->renderer);

	pixman_region32_t region;
	pixman_region32_init(&region);
	pixman_region32_copy(&region, _region);

	struct wlr_texture *src_tex =
		fx_texture_from_buffer(&pass->buffer->renderer->wlr_renderer, src_buffer->buffer);
	if (src_tex == NULL) {
		goto done;
	}

	// These buffers hold whatever the pass target holds, so the copy must not
	// convert anything. In two-pass mode that is linear light: leaving the
	// transfer function unset would make the texture path treat the source as
	// sRGB-encoded and decode it, darkening the copied region on every hop.
	// EXT_LINEAR selects the identity conversion.
	struct wlr_render_texture_options options = {
		.texture = src_tex,
		.clip = &region,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		.dst_box = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = dst_buffer->buffer->width,
			.height = dst_buffer->buffer->height,
		},
		.src_box = (struct wlr_fbox){
			.x = 0,
			.y = 0,
			.width = src_buffer->buffer->width,
			.height = src_buffer->buffer->height,
		},
	};
	if (pass->has_color_transform) {
		options.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
	}

	// Draw onto the dst_buffer. Only the pass target feeds the output
	// transform, so a copy into an offscreen buffer must not extend the
	// region that gets copied out at submit time.
	pass->suppress_updated = dst_buffer != pass->buffer;
	fx_framebuffer_bind(dst_buffer);
	wlr_render_pass_add_texture(&pass->base, &options);
	pass->suppress_updated = false;
	wlr_texture_destroy(src_tex);

	// Bind back to the main WLR buffer
	fx_framebuffer_bind(pass->buffer);

done:
	TRACY_BOTH_ZONES_END;

	pixman_region32_fini(&region);
}

static const char *reset_status_str(GLenum status) {
	switch (status) {
	case GL_GUILTY_CONTEXT_RESET_KHR:
		return "guilty";
	case GL_INNOCENT_CONTEXT_RESET_KHR:
		return "innocent";
	case GL_UNKNOWN_CONTEXT_RESET_KHR:
		return "unknown";
	default:
		return "<invalid>";
	}
}

struct fx_gles_render_pass *fx_begin_buffer_pass(struct fx_framebuffer *buffer,
		struct wlr_egl_context *prev_ctx, struct fx_render_timer *timer,
		struct wlr_drm_syncobj_timeline *signal_timeline, uint64_t signal_point,
		struct wlr_color_transform *color_transform,
		struct fx_offscreen_buffers *output_buffers) {
	struct fx_renderer *renderer = buffer->renderer;
	struct wlr_buffer *wlr_buffer = buffer->buffer;
	const bool has_color_transform = color_transform != NULL;
	buffer->capture_sdr = false;
	buffer->sdr_capture_valid = false;
	buffer->output_buffers = output_buffers;
	buffer->output_generation = 0;

	if (renderer->procs.glGetGraphicsResetStatusKHR) {
		GLenum status = renderer->procs.glGetGraphicsResetStatusKHR();
		if (status != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "GPU reset (%s)", reset_status_str(status));
			wl_signal_emit_mutable(&renderer->wlr_renderer.events.lost, NULL);
			return NULL;
		}
	}

	struct fx_gles_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	struct fx_framebuffer *target = buffer;
	if (has_color_transform) {
		struct output_transform_state state = {
			.tf = WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR,
		};
		wlr_matrix_identity(state.matrix);
		if (!output_transform_collect(&state, color_transform)) {
			log_unsupported_output_transform();
			free(pass);
			return NULL;
		}
		struct wlr_allocator *allocator = output_buffers != NULL
			? output_buffers->allocator : renderer->allocator;
		if (allocator == NULL) {
			wlr_log(WLR_ERROR, "No allocator available for FP16 blend buffer");
			free(pass);
			return NULL;
		}

		struct fx_framebuffer **blend_buffer = output_buffers != NULL
			? &output_buffers->blend_buffer : &buffer->blend_buffer;
		const bool needs_full_damage = output_buffers != NULL &&
			(*blend_buffer == NULL || (*blend_buffer)->buffer == NULL ||
			(*blend_buffer)->buffer->width != wlr_buffer->width ||
			(*blend_buffer)->buffer->height != wlr_buffer->height ||
			(*blend_buffer)->drm_format != DRM_FORMAT_ABGR16161616F ||
			!output_buffers->blend_valid);
		bool failed = false;
		fx_framebuffer_get_or_create_custom(renderer, allocator,
			wlr_buffer->width, wlr_buffer->height,
			DRM_FORMAT_ABGR16161616F, blend_buffer, &failed);
		if (failed || *blend_buffer == NULL) {
			if (output_buffers != NULL) {
				output_buffers->blend_valid = false;
			}
			free(pass);
			return NULL;
		}
		if (output_buffers != NULL) {
			if (needs_full_damage) {
				output_buffers->blend_valid = false;
				output_buffers->sdr_capture_generation = 0;
			}
			pass->needs_full_damage = needs_full_damage;
		} else {
			(*blend_buffer)->blend_parent = buffer;
		}
		target = *blend_buffer;
		memcpy(pass->output_matrix, state.matrix, sizeof(pass->output_matrix));
		pass->output_tf = state.tf;
		if (state.lut != NULL) {
			struct fx_output_lut *lut =
				get_or_create_output_lut(renderer, state.lut);
			if (lut == NULL) {
				free(pass);
				return NULL;
			}
			pass->output_lut = lut->texture;
			pass->output_lut_dim = lut->dim;
		}
	}

	GLint fbo = fx_framebuffer_get_fbo(target);
	if (!fbo) {
		free(pass);
		return NULL;
	}

	wlr_render_pass_init(&pass->base, &render_pass_impl);
	wlr_buffer_lock(wlr_buffer);
	pass->buffer = target;
	pass->output_buffer = buffer;
	pass->timer = timer;
	pass->prev_ctx = *prev_ctx;
	pass->has_color_transform = has_color_transform;
	pass->color_transform = has_color_transform
		? wlr_color_transform_ref(color_transform) : NULL;
	pass->output_buffers = output_buffers;
	if (signal_timeline != NULL) {
		pass->signal_timeline = wlr_drm_syncobj_timeline_ref(signal_timeline);
		pass->signal_point = signal_point;
	}

	pass->fx_offscreen_buffers = NULL;
	pixman_region32_init(&pass->blur_padding_region);
	pixman_region32_init(&pass->updated_region);
	pass->has_blur = false;

	matrix_projection(pass->projection_matrix, wlr_buffer->width, wlr_buffer->height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	push_fx_debug(renderer);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glViewport(0, 0, wlr_buffer->width, wlr_buffer->height);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_SCISSOR_TEST);

	pop_fx_debug(renderer);
	return pass;
}
