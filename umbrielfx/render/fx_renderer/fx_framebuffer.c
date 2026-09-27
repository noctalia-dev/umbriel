#include <stdlib.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>

#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"

static void handle_buffer_destroy(struct wlr_addon *addon) {
	struct fx_framebuffer *buffer =
		wl_container_of(addon, buffer, addon);
	fx_framebuffer_destroy(buffer);
}

static const struct wlr_addon_interface buffer_addon_impl = {
	.name = "fx_framebuffer",
	.destroy = handle_buffer_destroy,
};

GLuint fx_framebuffer_get_fbo(struct fx_framebuffer *buffer) {
	if (buffer->external_only) {
		wlr_log(WLR_ERROR, "DMA-BUF format is external-only");
		return 0;
	}

	if (buffer->fbo) {
		return buffer->fbo;
	}

	push_fx_debug(buffer->renderer);

	if (!buffer->rbo) {
		glGenRenderbuffers(1, &buffer->rbo);
		glBindRenderbuffer(GL_RENDERBUFFER, buffer->rbo);
		buffer->renderer->procs.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
				buffer->image);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	glGenFramebuffers(1, &buffer->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, buffer->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, buffer->rbo);
	GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "Failed to create FBO");
		glDeleteFramebuffers(1, &buffer->fbo);
		buffer->fbo = 0;
	}

	pop_fx_debug(buffer->renderer);

	return buffer->fbo;
}

bool fx_framebuffer_ensure_stencil(struct fx_framebuffer *buffer) {
	if (buffer->sb) {
		return true;
	}
	GLuint fbo = fx_framebuffer_get_fbo(buffer);
	if (!fbo) {
		return false;
	}

	push_fx_debug(buffer->renderer);

	glGenRenderbuffers(1, &buffer->sb);
	glBindRenderbuffer(GL_RENDERBUFFER, buffer->sb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
			buffer->buffer->width, buffer->buffer->height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, buffer->sb);
	GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "Failed to attach stencil buffer to FBO");
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
				GL_RENDERBUFFER, 0);
		glDeleteRenderbuffers(1, &buffer->sb);
		buffer->sb = 0;
	}

	pop_fx_debug(buffer->renderer);

	return buffer->sb != 0;
}

void fx_framebuffer_get_or_create_custom(struct fx_renderer *renderer,
		struct wlr_allocator *allocator, int width, int height, uint32_t format,
		struct fx_framebuffer **fx_framebuffer, bool *failed) {
	if (*failed) {
		return;
	}

	if (*fx_framebuffer != NULL) {
		struct wlr_buffer *wlr_buffer = (*fx_framebuffer)->buffer;
		if (wlr_buffer != NULL) {
			if (wlr_buffer->width == width && wlr_buffer->height == height &&
					(*fx_framebuffer)->drm_format == format) {
				return;
			}
			// Create a new wlr_buffer if it's null or if the output size has
			// changed
			wlr_buffer_drop(wlr_buffer);
		} else {
			fx_framebuffer_destroy(*fx_framebuffer);
		}
		*fx_framebuffer = NULL;
	}

	// Get the best supported DRM format (DMABUF if supported)
	const struct wlr_drm_format_set *texture_formats =
		wlr_renderer_get_texture_formats(&renderer->wlr_renderer,
			renderer->wlr_renderer.render_buffer_caps);
	const struct wlr_drm_format_set *formats = format == DRM_FORMAT_ABGR16161616F
		? wlr_egl_get_dmabuf_render_formats(renderer->egl)
		: texture_formats;
	const struct wlr_drm_format *drm_format =
		wlr_drm_format_set_get(formats, format);
	if (drm_format == NULL) {
		wlr_log(WLR_ERROR, "Failed to get a supported format while allocating buffer");
		*failed = true;
		return;
	}

	struct wlr_buffer *wlr_buffer =
		wlr_allocator_create_buffer(allocator, width, height, drm_format);
	if (wlr_buffer == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_buffer");
		*failed = true;
		return;
	}

	*fx_framebuffer = fx_framebuffer_get_or_create(renderer, wlr_buffer);
	if (*fx_framebuffer == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate fx_buffer");
		wlr_buffer_drop(wlr_buffer);
		*failed = true;
		return;
	}
	(*fx_framebuffer)->owned = true;
	GLuint fbo = fx_framebuffer_get_fbo(*fx_framebuffer);
	if (fbo == 0) {
		wlr_buffer_drop(wlr_buffer);
		*fx_framebuffer = NULL;
		*failed = true;
		return;
	}

	// Allocated DMA-BUF contents are undefined. This is especially visible
	// with FP16 effect buffers, where a blur kernel can spread extreme values
	// far beyond the region that has been rendered so far.
	GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat clear_color[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
	if (scissor_enabled) {
		glEnable(GL_SCISSOR_TEST);
	}
}

struct fx_framebuffer *fx_framebuffer_get_or_create(struct fx_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_addon *addon =
		wlr_addon_find(&wlr_buffer->addons, renderer, &buffer_addon_impl);
	if (addon) {
		struct fx_framebuffer *buffer = wl_container_of(addon, buffer, addon);
		return buffer;
	}

	struct fx_framebuffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->buffer = wlr_buffer;
	buffer->renderer = renderer;

	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		goto error_buffer;
	}
	buffer->drm_format = dmabuf.format;

	buffer->image = wlr_egl_create_image_from_dmabuf(renderer->egl,
		&dmabuf, &buffer->external_only);
	if (buffer->image == EGL_NO_IMAGE_KHR) {
		goto error_buffer;
	}

	wlr_addon_init(&buffer->addon, &wlr_buffer->addons, renderer,
		&buffer_addon_impl);

	wl_list_insert(&renderer->buffers, &buffer->link);

	wlr_log(WLR_DEBUG, "Created GL FBO for buffer %dx%d",
		wlr_buffer->width, wlr_buffer->height);

	return buffer;

error_buffer:
	free(buffer);
	return NULL;
}

void fx_framebuffer_bind(struct fx_framebuffer *fx_buffer) {
	glBindFramebuffer(GL_FRAMEBUFFER, fx_framebuffer_get_fbo(fx_buffer));
}

void fx_framebuffer_release_effect_capture(struct fx_framebuffer *fx_buffer) {
	struct fx_framebuffer *capture = fx_buffer->effect_capture_buffer;
	fx_buffer->effect_capture_valid = false;
	fx_buffer->effect_capture_owner = NULL;
	if (capture == NULL) {
		return;
	}
	fx_buffer->effect_capture_buffer = NULL;
	capture->effect_capture_parent = NULL;
	wlr_buffer_drop(capture->buffer);
}

void fx_framebuffer_destroy(struct fx_framebuffer *fx_buffer) {
	if (!fx_buffer) {
		wlr_log(WLR_ERROR, "Trying to destroy an already destroyed fx_framebuffer");
		return;
	}

	if (fx_buffer->blend_buffer != NULL) {
		struct wlr_buffer *blend_buffer = fx_buffer->blend_buffer->buffer;
		fx_buffer->blend_buffer->blend_parent = NULL;
		fx_buffer->blend_buffer = NULL;
		wlr_buffer_drop(blend_buffer);
	}
	if (fx_buffer->blend_parent != NULL) {
		fx_buffer->blend_parent->blend_buffer = NULL;
		fx_buffer->blend_parent = NULL;
	}
	if (fx_buffer->sdr_capture_buffer != NULL) {
		struct wlr_buffer *capture_buffer =
			fx_buffer->sdr_capture_buffer->buffer;
		fx_buffer->sdr_capture_buffer->sdr_capture_parent = NULL;
		fx_buffer->sdr_capture_buffer = NULL;
		wlr_buffer_drop(capture_buffer);
	}
	if (fx_buffer->sdr_capture_parent != NULL) {
		fx_buffer->sdr_capture_parent->sdr_capture_buffer = NULL;
		fx_buffer->sdr_capture_parent->sdr_capture_valid = false;
		fx_buffer->sdr_capture_parent = NULL;
	}
	fx_framebuffer_release_effect_capture(fx_buffer);
	if (fx_buffer->effect_capture_parent != NULL) {
		fx_buffer->effect_capture_parent->effect_capture_buffer = NULL;
		fx_buffer->effect_capture_parent->effect_capture_valid = false;
		fx_buffer->effect_capture_parent = NULL;
	}

	// Release the framebuffer
	wl_list_remove(&fx_buffer->link);
	wlr_addon_finish(&fx_buffer->addon);

	struct wlr_egl_context prev_ctx;
	wlr_egl_make_current(fx_buffer->renderer->egl, &prev_ctx);

	glDeleteFramebuffers(1, &fx_buffer->fbo);
	fx_buffer->fbo = -1;
	glDeleteRenderbuffers(1, &fx_buffer->rbo);
	fx_buffer->rbo = -1;
	glDeleteTextures(1, &fx_buffer->tex);
	fx_buffer->tex = -1;
	glDeleteRenderbuffers(1, &fx_buffer->sb);
	fx_buffer->sb = -1;

	wlr_egl_destroy_image(fx_buffer->renderer->egl, fx_buffer->image);

	wlr_egl_restore_context(&prev_ctx);

	free(fx_buffer);
}
