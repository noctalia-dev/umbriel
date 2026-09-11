#include <drm_fourcc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "render/fx_renderer/fx_renderer.h"
#include "render/pixel_format.h"

#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

#ifndef GL_RGB16F
#define GL_RGB16F 0x881B
#endif

/*
 * The DRM formats are little endian while the GL formats are big endian,
 * so DRM_FORMAT_ARGB8888 is actually compatible with GL_BGRA_EXT.
 */
static const struct fx_pixel_format formats[] = {
	{
		.drm_format = DRM_FORMAT_ARGB8888,
		.gl_format = GL_BGRA_EXT,
		.gl_type = GL_UNSIGNED_BYTE,
	},
	{
		.drm_format = DRM_FORMAT_XRGB8888,
		.gl_format = GL_BGRA_EXT,
		.gl_type = GL_UNSIGNED_BYTE,
	},
	{
		.drm_format = DRM_FORMAT_XBGR8888,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_BYTE,
	},
	{
		.drm_format = DRM_FORMAT_ABGR8888,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_BYTE,
	},
	{
		.drm_format = DRM_FORMAT_BGR888,
		.gl_format = GL_RGB,
		.gl_type = GL_UNSIGNED_BYTE,
	},
#if WLR_LITTLE_ENDIAN
	{
		.drm_format = DRM_FORMAT_RGBX4444,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT_4_4_4_4,
	},
	{
		.drm_format = DRM_FORMAT_RGBA4444,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT_4_4_4_4,
	},
	{
		.drm_format = DRM_FORMAT_RGBX5551,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT_5_5_5_1,
	},
	{
		.drm_format = DRM_FORMAT_RGBA5551,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT_5_5_5_1,
	},
	{
		.drm_format = DRM_FORMAT_RGB565,
		.gl_format = GL_RGB,
		.gl_type = GL_UNSIGNED_SHORT_5_6_5,
	},
	{
		.drm_format = DRM_FORMAT_XBGR2101010,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_INT_2_10_10_10_REV_EXT,
	},
	{
		.drm_format = DRM_FORMAT_ABGR2101010,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_INT_2_10_10_10_REV_EXT,
	},
	{
		.drm_format = DRM_FORMAT_BGR161616F,
		.gl_format = GL_RGB,
		.gl_type = GL_HALF_FLOAT_OES,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616F,
		.gl_format = GL_RGBA,
		.gl_type = GL_HALF_FLOAT_OES,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616F,
		.gl_format = GL_RGBA,
		.gl_type = GL_HALF_FLOAT_OES,
	},
	{
		.drm_format = DRM_FORMAT_BGR161616,
		.gl_internalformat = GL_RGB16_EXT,
		.gl_format = GL_RGB,
		.gl_type = GL_UNSIGNED_SHORT,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616,
		.gl_internalformat = GL_RGBA16_EXT,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616,
		.gl_internalformat = GL_RGBA16_EXT,
		.gl_format = GL_RGBA,
		.gl_type = GL_UNSIGNED_SHORT,
	},
#endif
};

// TODO: more pixel formats

/*
 * Return true if supported for texturing, even if other operations like
 * reading aren't supported.
 */
bool is_fx_pixel_format_supported(const struct fx_renderer *renderer,
		const struct fx_pixel_format *format) {
	if (format->gl_type == GL_UNSIGNED_INT_2_10_10_10_REV_EXT
			&& !renderer->exts.EXT_texture_type_2_10_10_10_REV) {
		return false;
	}
	if (format->gl_type == GL_HALF_FLOAT_OES
			&& !renderer->exts.half_float_renderable) {
		return false;
	}
	if (format->gl_type == GL_UNSIGNED_SHORT
			&& !renderer->exts.EXT_texture_norm16) {
		return false;
	}
	/*
	 * Note that we don't need to check for GL_EXT_texture_format_BGRA8888
	 * here, since we've already checked if we have it at renderer creation
	 * time and bailed out if not. We do the check there because Wayland
	 * requires all compositors to support SHM buffers in that format.
	 */
	return true;
}

GLenum fx_resolve_gl_type(const struct fx_renderer *renderer, GLenum gl_type) {
	if (gl_type == GL_HALF_FLOAT_OES && renderer->is_gles3) {
		return GL_HALF_FLOAT;
	}
	return gl_type;
}

GLint fx_resolve_internal_format(const struct fx_renderer *renderer,
		const struct fx_pixel_format *fmt) {
	// GLES3 core pairs GL_HALF_FLOAT with a sized internal format. The unsized
	// gl_format fallback below is only legal with GL_HALF_FLOAT_OES on GLES2.
	if (fmt->gl_type == GL_HALF_FLOAT_OES && renderer->is_gles3) {
		return fmt->gl_format == GL_RGB ? GL_RGB16F : GL_RGBA16F;
	}
	if (fmt->gl_internalformat) {
		return fmt->gl_internalformat;
	}
	return fmt->gl_format;
}

const struct fx_pixel_format *get_fx_format_from_drm(uint32_t fmt) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
		if (formats[i].drm_format == fmt) {
			return &formats[i];
		}
	}
	return NULL;
}

const struct fx_pixel_format *get_fx_format_from_gl(
		GLint gl_format, GLint gl_type, bool alpha) {
	if (gl_type == GL_HALF_FLOAT) {
		gl_type = GL_HALF_FLOAT_OES;
	}

	for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
		if (formats[i].gl_format != gl_format ||
				formats[i].gl_type != gl_type) {
			continue;
		}

		if (pixel_format_has_alpha(formats[i].drm_format) != alpha) {
			continue;
		}

		return &formats[i];
	}
	return NULL;
}

void get_fx_shm_formats(const struct fx_renderer *renderer,
		struct wlr_drm_format_set *out) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (!is_fx_pixel_format_supported(renderer, &formats[i])) {
			continue;
		}
		wlr_drm_format_set_add(out, formats[i].drm_format, DRM_FORMAT_MOD_INVALID);
		wlr_drm_format_set_add(out, formats[i].drm_format, DRM_FORMAT_MOD_LINEAR);
	}
}
