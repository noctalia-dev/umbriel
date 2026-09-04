#ifndef _FX_OPENGL_H
#define _FX_OPENGL_H

#include <GLES2/gl2.h>
#include <umbrielfx/render/fx_renderer/fx_renderer.h>
#include <stdbool.h>
#include <time.h>
#include <wlr/render/egl.h>
#include <wlr/render/interface.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

#include "render/fx_renderer/shaders.h"
#include "render/tracy.h"

struct fx_framebuffer;
struct wlr_allocator;

void fx_renderer_set_allocator(struct wlr_renderer *renderer,
	struct wlr_allocator *allocator);

struct fx_pixel_format {
	uint32_t drm_format;
	// optional field, if empty then internalformat = format
	GLint gl_internalformat;
	GLint gl_format, gl_type;
};

bool is_fx_pixel_format_supported(const struct fx_renderer *renderer, const struct fx_pixel_format *format);
const struct fx_pixel_format *get_fx_format_from_drm(uint32_t fmt);
const struct fx_pixel_format *get_fx_format_from_gl(GLint gl_format, GLint gl_type, bool alpha);
void get_fx_shm_formats(const struct fx_renderer *renderer, struct wlr_drm_format_set *out);

GLuint fx_framebuffer_get_fbo(struct fx_framebuffer *buffer);

/**
 * Attaches a stencil renderbuffer to the framebuffer on first use. Leaves the
 * framebuffer bound. Returns false when no stencil could be attached.
 */
bool fx_framebuffer_ensure_stencil(struct fx_framebuffer *buffer);

///
/// fx_framebuffer
///

struct fx_framebuffer {
	struct wlr_buffer *buffer;
	struct fx_renderer *renderer;
	struct wl_list link; // fx_renderer.buffers
	bool external_only;
	bool owned;
	uint32_t drm_format;
	// Non-NULL when this is a swapchain target using output-local scratch buffers
	struct fx_offscreen_buffers *output_buffers;
	uint64_t output_generation;
	struct fx_framebuffer *blend_buffer;
	struct fx_framebuffer *blend_parent;
	struct fx_framebuffer *sdr_capture_buffer;
	struct fx_framebuffer *sdr_capture_parent;
	bool capture_sdr;
	bool sdr_capture_valid;

	EGLImageKHR image;
	GLuint rbo;
	GLuint fbo;
	GLuint tex;
	GLuint sb; // Stencil

	struct wlr_addon addon;
};

/**
 * Should only be used with custom fbs.
 * Note: Does not bind back to the default Framebuffer!
 */
void fx_framebuffer_get_or_create_custom(struct fx_renderer *fx_renderer,
		struct wlr_allocator *allocator, int width, int height, uint32_t format,
		struct fx_framebuffer **fx_buffer, bool *failed);

struct fx_framebuffer *fx_framebuffer_get_or_create(struct fx_renderer *renderer,
		struct wlr_buffer *wlr_buffer);

void fx_framebuffer_bind(struct fx_framebuffer *buffer);

/**
 * Destroy the fx_framebuffer.
 * Note: Doesn't drop the wlr_buffer, so should only be used internally.
 */
void fx_framebuffer_destroy(struct fx_framebuffer *buffer);

///
/// fx_texture
///

struct fx_texture {
	struct wlr_texture wlr_texture;
	struct fx_renderer *fx_renderer;
	struct wl_list link; // fx_renderer.textures

	GLuint target;

	// If this texture is imported from a buffer, the texture is does not own
	// these states. These cannot be destroyed along with the texture in this
	// case.
	GLuint tex;
	GLuint fbo;

	bool has_alpha;

	uint32_t drm_format; // for mutable textures only, used to interpret upload data
	struct fx_framebuffer *buffer; // for DMA-BUF imports only
	struct wlr_buffer *locked_buffer;
};

struct fx_texture *fx_get_texture(struct wlr_texture *wlr_texture);

void fx_texture_destroy(struct fx_texture *texture);

bool wlr_texture_is_fx(struct wlr_texture *wlr_texture);

bool wlr_renderer_is_fx(struct wlr_renderer *wlr_renderer);

struct fx_render_timer *fx_get_render_timer(struct wlr_render_timer *timer);

struct fx_texture *fx_get_texture(struct wlr_texture *wlr_texture);

struct wlr_renderer *fx_renderer_create_egl(struct wlr_egl *egl);

struct wlr_egl *wlr_fx_renderer_get_egl(struct wlr_renderer *renderer);

void push_fx_debug_(struct fx_renderer *renderer, const char *file, const char *func);

#define push_fx_debug(renderer) push_fx_debug_(renderer, _WLR_FILENAME, __func__)

void pop_fx_debug(struct fx_renderer *renderer);

///
/// Render Timer
///

struct fx_render_timer {
	struct wlr_render_timer base;
	struct fx_renderer *renderer;
	struct timespec cpu_start;
	struct timespec cpu_end;
	GLuint id;
	GLint64 gl_cpu_end;
};

bool wlr_render_timer_is_fx(struct wlr_render_timer *timer);

///
/// fx_renderer
///

/**
 * OpenGL ES 2 renderer.
 *
 * Care must be taken to avoid stepping each other's toes with EGL contexts:
 * the current EGL is global state. The GLES2 renderer operations will save
 * and restore any previous EGL context when called. A render pass is seen as
 * a single operation.
 *
 * The GLES2 renderer doesn't support arbitrarily nested render passes. It
 * supports a subset only: after a nested render pass is created, any parent
 * render pass can't be used before the nested render pass is submitted.
 */

struct fx_renderer {
	struct wlr_renderer wlr_renderer;

	struct wlr_egl *egl;
	int drm_fd;
	struct wlr_allocator *allocator;

	struct wlr_drm_format_set shm_texture_formats;

	const char *exts_str;
	struct {
		bool EXT_read_format_bgra;
		bool KHR_debug;
		bool OES_egl_image_external;
		bool OES_egl_image;
		bool EXT_texture_type_2_10_10_10_REV;
		bool OES_texture_half_float_linear;
		bool EXT_texture_norm16;
		bool EXT_disjoint_timer_query;
	} exts;

	struct {
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
		PFNGLDEBUGMESSAGECALLBACKKHRPROC glDebugMessageCallbackKHR;
		PFNGLDEBUGMESSAGECONTROLKHRPROC glDebugMessageControlKHR;
		PFNGLPOPDEBUGGROUPKHRPROC glPopDebugGroupKHR;
		PFNGLPUSHDEBUGGROUPKHRPROC glPushDebugGroupKHR;
		PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES;
		PFNGLGETGRAPHICSRESETSTATUSKHRPROC glGetGraphicsResetStatusKHR;
		PFNGLGENQUERIESEXTPROC glGenQueriesEXT;
		PFNGLDELETEQUERIESEXTPROC glDeleteQueriesEXT;
		PFNGLQUERYCOUNTEREXTPROC glQueryCounterEXT;
		PFNGLGETQUERYOBJECTIVEXTPROC glGetQueryObjectivEXT;
		PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT;
		PFNGLGETINTEGER64VEXTPROC glGetInteger64vEXT;
		TRACY_FN(
			PFNGLGETQUERYIVEXTPROC glGetQueryivEXT;
		)
	} procs;

	struct {
		struct quad_shader quad;
		struct quad_shader quad_clip;
		struct quad_grad_shader quad_grad;
		struct quad_round_shader quad_round;
		struct border_shader border;
		struct quad_grad_round_shader quad_grad_round;

		struct tex_shader tex_rgba;
		struct tex_shader tex_rgbx;
		struct tex_shader tex_ext;
		struct tex_shader tex_effects_rgba;
		struct tex_shader tex_effects_rgbx;
		struct tex_shader tex_effects_ext;
		struct tex_shader tex_clamp_rgba;
		struct tex_shader tex_clamp_rgbx;
		struct tex_shader tex_clamp_ext;
		struct tex_shader tex_clamp_effects_rgba;
		struct tex_shader tex_clamp_effects_rgbx;
		struct tex_shader tex_clamp_effects_ext;
		struct output_shader output;

		struct box_shadow_shader box_shadow;
		struct blur_shader blur1;
		struct blur_shader blur2;
		struct blur_effects_shader blur_effects;
	} shaders;

	struct wl_list buffers; // fx_framebuffer.link
	struct wl_list textures; // fx_texture.link
	struct wl_list offscreen_buffers; // fx_offscreen_buffers.link
	struct wl_list output_luts; // fx_output_lut.link

	TRACY_FN(
		struct tracy_data *tracy_data;
	)
};

struct fx_output_lut {
	struct wlr_addon addon;
	struct wl_list link; // fx_renderer.output_luts
	struct fx_renderer *renderer;
	GLuint texture;
	float dim;
};

void fx_output_lut_destroy(struct fx_output_lut *lut);

#endif
