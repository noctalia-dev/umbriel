#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#include "render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"

static void drop_framebuffer(struct fx_framebuffer **buffer) {
	if (*buffer == NULL) {
		return;
	}
	wlr_buffer_drop((*buffer)->buffer);
	*buffer = NULL;
}

static void clear_effect_buffers(struct fx_offscreen_buffers *fbos) {
	drop_framebuffer(&fbos->optimized_blur_buffer);
	drop_framebuffer(&fbos->optimized_no_blur_buffer);
	drop_framebuffer(&fbos->blur_saved_pixels_buffer);
	drop_framebuffer(&fbos->effects_buffer);
	drop_framebuffer(&fbos->effects_buffer_swapped);
}

static void addon_handle_destroy(struct wlr_addon *addon) {
	struct fx_offscreen_buffers *fbos = wl_container_of(addon, fbos, addon);

	struct fx_framebuffer *buffer;
	wl_list_for_each(buffer, &fbos->renderer->buffers, link) {
		if (buffer->output_buffers == fbos) {
			buffer->output_buffers = NULL;
			buffer->capture_sdr = false;
		}
	}

	clear_effect_buffers(fbos);
	drop_framebuffer(&fbos->sdr_capture_buffer);
	drop_framebuffer(&fbos->blend_buffer);

	wl_list_remove(&fbos->link);
	wlr_addon_finish(&fbos->addon);
	free(fbos);
}

static const struct wlr_addon_interface fbos_addon_impl = {
	.name = "fx_offscreen_buffers",
	.destroy = addon_handle_destroy,
};

static bool fx_offscreen_buffers_assign(struct wlr_output *output,
		struct fx_offscreen_buffers *fbos) {
	wlr_addon_init(&fbos->addon, &output->addons, output, &fbos_addon_impl);
	return true;
}

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers *fbos) {
	addon_handle_destroy(&fbos->addon);
}

void fx_renderer_clear_output_effect_buffers(struct wlr_output *output) {
	if (output == NULL) {
		return;
	}

	struct wlr_addon *addon = wlr_addon_find(&output->addons, output,
			&fbos_addon_impl);
	if (addon == NULL) {
		return;
	}

	struct fx_offscreen_buffers *fbos =
		wl_container_of(addon, fbos, addon);
	clear_effect_buffers(fbos);
}

void fx_offscreen_buffers_invalidate_blend(struct wlr_output *output) {
	if (output == NULL) {
		return;
	}

	struct wlr_addon *addon = wlr_addon_find(&output->addons, output,
		&fbos_addon_impl);
	if (addon == NULL) {
		return;
	}

	struct fx_offscreen_buffers *fbos =
		wl_container_of(addon, fbos, addon);
	fbos->blend_valid = false;
	fbos->sdr_capture_generation = 0;
}

struct fx_offscreen_buffers *fx_offscreen_buffers_try_get(struct wlr_output *output) {
	struct fx_offscreen_buffers *fbos = NULL;
	if (!output) {
		return NULL;
	}

	struct wlr_addon *addon = wlr_addon_find(&output->addons, output,
			&fbos_addon_impl);
	if (!addon) {
		goto create_new;
	}

	if (!(fbos = wl_container_of(addon, fbos, addon))) {
		goto create_new;
	}
	return fbos;

create_new:;
	struct fx_renderer *renderer = fx_get_renderer(output->renderer);
	if (!renderer) {
		return NULL;
	}

	fbos = calloc(1, sizeof(*fbos));
	if (!fbos) {
		wlr_log(WLR_ERROR, "Could not allocate a fx_offscreen_buffers");
		return NULL;
	}
	fbos->renderer = renderer;

	if (!fx_offscreen_buffers_assign(output, fbos)) {
		wlr_log(WLR_ERROR, "Could not assign fx_offscreen_buffers to output: '%s'",
				output->name);
		free(fbos);
		return NULL;
	}
	wl_list_insert(&renderer->offscreen_buffers, &fbos->link);
	return fbos;
}
