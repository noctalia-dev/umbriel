// ext-image-copy-capture publishes one SHM readback format, and its consumers (the
// portal's PipeWire stream, meeting clients) expect the BGRA byte order every other
// wlroots compositor publishes. fx render targets are GL_RGBA, so the naive answer is
// the RGBx order and a shared screen comes out black.

#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>

#include "umbrielfx/render/fx_renderer/fx_renderer.h"

#define SIZE 2
#define STRIDE (SIZE * 4)

static bool check(bool condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
	}
	return condition;
}

// A BGRA-order read puts red in the third byte, an RGBx-order one in the first.
static bool red_is_where_the_format_says(const uint32_t *pixels, uint32_t format) {
	const uint8_t *bytes = (const uint8_t *)pixels;

	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return bytes[2] == 0xFF && bytes[0] == 0x00;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return bytes[0] == 0xFF && bytes[2] == 0x00;
	default:
		return false;
	}
}

static bool is_bgra_order(uint32_t format) {
	return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888;
}

struct probe {
	// Format the texture is filled with; it decides the read format the driver reports.
	uint32_t source;
	uint32_t pixel; // One solid red pixel in `source`'s layout.
	const char *name;
};

static bool run_probe(struct wlr_renderer *renderer, const struct probe *probe) {
	uint32_t source[SIZE * SIZE];
	for (int i = 0; i < SIZE * SIZE; ++i) {
		source[i] = probe->pixel;
	}

	struct wlr_texture *texture = wlr_texture_from_pixels(
		renderer, probe->source, STRIDE, SIZE, SIZE, source);
	if (!check(texture != NULL, probe->name)) {
		fprintf(stderr, "  could not create a texture from %s\n", probe->name);
		return false;
	}

	uint32_t published = wlr_texture_preferred_read_format(texture);

	uint32_t readback[SIZE * SIZE] = {0};
	bool ok = check(wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options) {
			.data = readback,
			.format = published,
			.stride = STRIDE,
		}), probe->name);
	if (ok) {
		char message[256];
		snprintf(message, sizeof(message),
			"%s: the published format 0x%08X did not read back in its own byte order",
			probe->name, published);
		ok = check(red_is_where_the_format_says(readback, published), message);
	}

	// What the texture publishes has to agree with the BGRA order whenever the driver
	// can read it back, because capture consumers are written against that order.
	static const uint32_t bgra_formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
	bool bgra_readable = false;
	for (size_t i = 0; i < sizeof(bgra_formats) / sizeof(*bgra_formats); ++i) {
		uint32_t attempt[SIZE * SIZE] = {0};
		bgra_readable = wlr_texture_read_pixels(texture,
			&(struct wlr_texture_read_pixels_options) {
				.data = attempt,
				.format = bgra_formats[i],
				.stride = STRIDE,
			}) || bgra_readable;
	}
	if (bgra_readable && !is_bgra_order(published)) {
		char message[256];
		snprintf(message, sizeof(message),
			"%s: the driver can read the BGRA order back but the texture publishes 0x%08X",
			probe->name, published);
		ok = check(false, message) && ok;
	}

	wlr_texture_destroy(texture);
	return ok;
}

int main(void) {
	int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		fprintf(stderr, "SKIP: no render node\n");
		return 77;
	}

	struct wlr_renderer *renderer = fx_renderer_create_with_drm_fd(drm_fd);
	if (renderer == NULL) {
		fprintf(stderr, "SKIP: no renderer\n");
		close(drm_fd);
		return 77;
	}

	// GL_RGBA targets (the case that reports the RGBx order) and GL_BGRA targets
	// (which already answer in the BGRA order).
	static const struct probe probes[] = {
		{DRM_FORMAT_XBGR8888, 0x000000FF, "XBGR8888 source"},
		{DRM_FORMAT_ABGR8888, 0xFF0000FF, "ABGR8888 source"},
		{DRM_FORMAT_XRGB8888, 0x00FF0000, "XRGB8888 source"},
		{DRM_FORMAT_ARGB8888, 0xFFFF0000, "ARGB8888 source"},
	};

	bool ok = true;
	for (size_t i = 0; i < sizeof(probes) / sizeof(*probes); ++i) {
		ok = run_probe(renderer, &probes[i]) && ok;
	}

	wlr_renderer_destroy(renderer);
	close(drm_fd);

	if (!ok) {
		return 1;
	}
	printf("preferred read format publishes the BGRA byte order capture consumers expect\n");
	return 0;
}
