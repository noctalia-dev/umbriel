#ifndef UMBRIELFX_TESTS_RENDER_FIXTURE_H
#define UMBRIELFX_TESTS_RENDER_FIXTURE_H

#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <xf86drm.h>

#include "render/color.h"
#include "render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"
#include "umbrielfx/render/pass.h"
#include "umbrielfx/types/fx/blur_data.h"
#include <wlr/render/swapchain.h>
#include <umbrielfx/types/wlr_scene.h>

#define TEST_WIDTH 16
#define TEST_HEIGHT 16

struct fixture {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output *output;
	int drm_fd;
};

static bool check(bool condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
	}
	return condition;
}

static bool fixture_try_device(struct fixture *fixture, const char *path) {
	int drm_fd = open(path, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		return false;
	}

	struct wlr_renderer *renderer = fx_renderer_create_with_drm_fd(drm_fd);
	if (renderer == NULL || !renderer->features.output_color_transform) {
		if (renderer != NULL) {
			wlr_renderer_destroy(renderer);
		}
		close(drm_fd);
		return false;
	}

	fixture->backend->buffer_caps |= WLR_BUFFER_CAP_DMABUF;
	struct wlr_allocator *allocator =
		wlr_allocator_autocreate(fixture->backend, renderer);
	if (allocator == NULL) {
		wlr_renderer_destroy(renderer);
		close(drm_fd);
		return false;
	}

	fixture->renderer = renderer;
	fixture->allocator = allocator;
	fixture->drm_fd = drm_fd;
	return true;
}

static bool fixture_init(struct fixture *fixture) {
	*fixture = (struct fixture) { .drm_fd = -1 };
	fixture->display = wl_display_create();
	if (fixture->display == NULL) {
		return false;
	}
	fixture->backend = wlr_headless_backend_create(
		wl_display_get_event_loop(fixture->display));
	if (fixture->backend == NULL) {
		return false;
	}

	const char *requested_device = getenv("UMBRIELFX_TEST_DRM_DEVICE");
	if (requested_device != NULL &&
			fixture_try_device(fixture, requested_device)) {
		goto create_output;
	}

	drmDevicePtr devices[64] = {0};
	int devices_len = drmGetDevices2(0, devices, 64);
	for (int i = 0; i < devices_len && fixture->renderer == NULL; i++) {
		if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
			continue;
		}
		fixture_try_device(fixture, devices[i]->nodes[DRM_NODE_RENDER]);
	}
	if (devices_len > 0) {
		drmFreeDevices(devices, devices_len);
	}
	if (fixture->renderer == NULL) {
		return false;
	}

create_output:
	fixture->output = wlr_headless_add_output(
		fixture->backend, TEST_WIDTH, TEST_HEIGHT);
	if (fixture->output == NULL || !wlr_output_init_render(fixture->output,
			fixture->allocator, fixture->renderer)) {
		return false;
	}
	fx_renderer_set_allocator(fixture->renderer, fixture->allocator);
	return true;
}

static void fixture_finish(struct fixture *fixture) {
	if (fixture->allocator != NULL) {
		wlr_allocator_destroy(fixture->allocator);
	}
	if (fixture->renderer != NULL) {
		wlr_renderer_destroy(fixture->renderer);
	}
	if (fixture->backend != NULL) {
		wlr_backend_destroy(fixture->backend);
	}
	if (fixture->display != NULL) {
		wl_display_destroy(fixture->display);
	}
	if (fixture->drm_fd >= 0) {
		close(fixture->drm_fd);
	}
}

static const struct wlr_drm_format *get_render_format(
		struct fixture *fixture, uint32_t format) {
	const struct wlr_drm_format_set *formats =
		fixture->renderer->impl->get_render_formats(fixture->renderer);
	return wlr_drm_format_set_get(formats, format);
}

static struct wlr_buffer *create_output_buffer(struct fixture *fixture,
		uint32_t format, int width, int height) {
	const struct wlr_drm_format *drm_format =
		get_render_format(fixture, format);
	if (drm_format == NULL) {
		return NULL;
	}
	return wlr_allocator_create_buffer(
		fixture->allocator, width, height, drm_format);
}

static bool read_buffer(struct fixture *fixture, struct wlr_buffer *buffer,
		uint32_t format, uint32_t stride, void *data) {
	struct wlr_texture *texture =
		wlr_texture_from_buffer(fixture->renderer, buffer);
	if (texture == NULL) {
		return false;
	}
	bool ok = wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options) {
			.data = data,
			.format = format,
			.stride = stride,
		});
	wlr_texture_destroy(texture);
	return ok;
}

// Renders `scene` onto the fixture output through wlr_scene_output_build_state,
// the same path the compositor uses, and returns the locked rendered buffer.
// The caller unlocks it and finishes `state`.
static inline struct wlr_buffer *fixture_render_scene(struct fixture *fixture,
		struct wlr_scene_output *scene_output, struct wlr_output_state *state) {
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	if (format == NULL) {
		return NULL;
	}
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format);
	if (swapchain == NULL) {
		return NULL;
	}
	wlr_output_state_init(state);
	struct wlr_scene_output_state_options options = { .swapchain = swapchain };
	struct wlr_buffer *rendered = NULL;
	if (wlr_scene_output_build_state(scene_output, state, &options) && state->buffer != NULL) {
		rendered = wlr_buffer_lock(state->buffer);
	}
	wlr_swapchain_destroy(swapchain);
	return rendered;
}

// 8-bit ARGB pixel readback of a rendered buffer at (x, y); returns false when unreadable.
static inline bool fixture_read_pixel(struct fixture *fixture, struct wlr_buffer *buffer, int x, int y, uint8_t out[4]) {
	uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4];
	if (!read_buffer(fixture, buffer, DRM_FORMAT_ARGB8888, TEST_WIDTH * 4, pixels)) {
		return false;
	}
	memcpy(out, &pixels[(y * TEST_WIDTH + x) * 4], 4);   // B G R A byte order
	return true;
}

// Reads one pixel of the buffer's own framebuffer with glReadPixels. Unlike
// read_buffer this never imports the buffer as a texture, so it sees the
// display composition even while a capture substitute is valid.
static inline bool fixture_read_display_pixel(struct fixture *fixture, struct wlr_buffer *buffer, int x, int y, uint8_t out[4]) {
	struct fx_renderer *renderer = fx_get_renderer(fixture->renderer);
	struct wlr_egl_context previous;
	if (!wlr_egl_make_current(renderer->egl, &previous)) {
		return false;
	}
	GLuint fbo = fx_renderer_get_buffer_fbo(fixture->renderer, buffer);
	bool ok = fbo != 0;
	if (ok) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		uint8_t rgba[4];
		// The pass projects with FLIPPED_180, so GL row y is buffer row y.
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		ok = glGetError() == GL_NO_ERROR;
		out[0] = rgba[2]; out[1] = rgba[1]; out[2] = rgba[0]; out[3] = rgba[3];   // B G R A like fixture_read_pixel
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	wlr_egl_restore_context(&previous);
	return ok;
}
#endif
