#ifndef UMBRIELFX_RENDER_FX_RENDERER_H
#define UMBRIELFX_RENDER_FX_RENDERER_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wlr/backend.h>
#include <wlr/render/interface.h>
#include <wlr/types/wlr_buffer.h>

struct fx_renderer;
struct wlr_output;

struct wlr_renderer *fx_renderer_create_with_drm_fd(int drm_fd);
/* Creates through GBM without enumerating EGL devices. */
struct wlr_renderer *fx_renderer_create_with_drm_fd_gbm(int drm_fd);
struct wlr_renderer *fx_renderer_create(struct wlr_backend *backend);

struct fx_renderer *fx_get_renderer(struct wlr_renderer *wlr_renderer);

bool fx_renderer_check_ext(struct wlr_renderer *renderer, const char *ext);
GLuint fx_renderer_get_buffer_fbo(struct wlr_renderer *renderer, struct wlr_buffer *buffer);
void fx_renderer_clear_output_effect_buffers(struct wlr_output *output);

//
// fx_texture
//

struct fx_texture_attribs {
	GLenum target; /* either GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
	GLuint tex;

	bool has_alpha;
};

struct wlr_texture *fx_texture_from_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer);

void fx_texture_get_attribs(struct wlr_texture *texture,
	struct fx_texture_attribs *attribs);

#endif
