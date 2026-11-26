#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"

#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/fx_renderer/fx_renderer.h"

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

struct output_sdr_transfer {
  struct wlr_addon addon;
  enum wlr_color_transfer_function transfer_function;
};

static void output_sdr_transfer_destroy(struct wlr_addon* addon) {
  struct output_sdr_transfer* state = wl_container_of(addon, state, addon);
  wlr_addon_finish(addon);
  free(state);
}

static const struct wlr_addon_interface output_sdr_transfer_impl = {
    .name = "umbrielfx_output_sdr_transfer",
    .destroy = output_sdr_transfer_destroy,
};

enum wlr_color_transfer_function fx_output_get_sdr_transfer_function(struct wlr_output* output) {
  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &output_sdr_transfer_impl);
  if (addon == NULL) {
    return WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
  }
  struct output_sdr_transfer* state = wl_container_of(addon, state, addon);
  return state->transfer_function;
}

bool fx_output_set_sdr_transfer_function(
    struct wlr_output* output, enum wlr_color_transfer_function transfer_function
) {
  if (transfer_function != WLR_COLOR_TRANSFER_FUNCTION_GAMMA22
      && transfer_function != WLR_COLOR_TRANSFER_FUNCTION_SRGB) {
    return false;
  }
  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &output_sdr_transfer_impl);
  if (addon == NULL && transfer_function == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22) {
    return true;
  }
  struct output_sdr_transfer* state;
  if (addon != NULL) {
    state = wl_container_of(addon, state, addon);
  } else {
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
      return false;
    }
    wlr_addon_init(&state->addon, &output->addons, output, &output_sdr_transfer_impl);
  }
  state->transfer_function = transfer_function;
  return true;
}

static void drop_framebuffer(struct fx_framebuffer** buffer) {
  if (*buffer == NULL) {
    return;
  }
  wlr_buffer_drop((*buffer)->buffer);
  *buffer = NULL;
}

static void clear_animation_buffers(struct fx_target_buffers* buffers) {
  for (size_t i = 0; i < FX_ANIMATION_DEPTH; i++) {
    drop_framebuffer(&buffers->animation_buffers[i]);
  }
  drop_framebuffer(&buffers->animation_backdrop);
  drop_framebuffer(&buffers->animation_shadow_blur);
  drop_framebuffer(&buffers->in_place_source);
}

static void clear_target_buffers(struct fx_target_buffers* buffers) {
  clear_animation_buffers(buffers);
  drop_framebuffer(&buffers->effects_buffer);
  drop_framebuffer(&buffers->effects_buffer_swapped);
}

static void clear_effect_buffers(struct fx_offscreen_buffers* fbos) {
  fbos->sdr_composition_valid = false;
  drop_framebuffer(&fbos->sdr_composition_buffer);
  clear_target_buffers(&fbos->output);
  clear_target_buffers(&fbos->group);
  drop_framebuffer(&fbos->optimized_blur_buffer);
  drop_framebuffer(&fbos->optimized_no_blur_buffer);
  drop_framebuffer(&fbos->blur_saved_pixels_buffer);
}

static void addon_handle_destroy(struct wlr_addon* addon) {
  struct fx_offscreen_buffers* fbos = wl_container_of(addon, fbos, addon);

  struct fx_framebuffer* buffer;
  wl_list_for_each(buffer, &fbos->renderer->buffers, link) {
    if (buffer->output_buffers == fbos) {
      buffer->output_buffers = NULL;
      buffer->capture_sdr = false;
    }
  }

  if (fbos->audio_texture != 0) {
    struct wlr_egl_context previous;
    if (wlr_egl_make_current(fbos->renderer->egl, &previous)) {
      glDeleteTextures(1, &fbos->audio_texture);
      wlr_egl_restore_context(&previous);
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

static bool fx_offscreen_buffers_assign(struct wlr_output* output, struct fx_offscreen_buffers* fbos) {
  wlr_addon_init(&fbos->addon, &output->addons, output, &fbos_addon_impl);
  return true;
}

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers* fbos) { addon_handle_destroy(&fbos->addon); }

void fx_offscreen_buffers_clear_group(struct fx_offscreen_buffers* fbos) { clear_target_buffers(&fbos->group); }

void fx_renderer_clear_output_effect_buffers(struct wlr_output* output) {
  if (output == NULL) {
    return;
  }

  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &fbos_addon_impl);
  if (addon == NULL) {
    return;
  }

  struct fx_offscreen_buffers* fbos = wl_container_of(addon, fbos, addon);
  clear_effect_buffers(fbos);
}

void fx_renderer_clear_animation_buffers(struct wlr_output* output) {
  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &fbos_addon_impl);
  if (addon == NULL) {
    return;
  }
  struct fx_offscreen_buffers* fbos = wl_container_of(addon, fbos, addon);
  clear_animation_buffers(&fbos->output);
  clear_animation_buffers(&fbos->group);
}

void fx_offscreen_buffers_invalidate_blend(struct wlr_output* output) {
  if (output == NULL) {
    return;
  }

  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &fbos_addon_impl);
  if (addon == NULL) {
    return;
  }

  struct fx_offscreen_buffers* fbos = wl_container_of(addon, fbos, addon);
  fbos->blend_valid = false;
  fbos->sdr_composition_valid = false;
  fbos->sdr_capture_generation = 0;
}

struct fx_offscreen_buffers* fx_offscreen_buffers_try_get(struct wlr_output* output) {
  struct fx_offscreen_buffers* fbos = NULL;
  if (!output) {
    return NULL;
  }

  struct wlr_addon* addon = wlr_addon_find(&output->addons, output, &fbos_addon_impl);
  if (!addon) {
    goto create_new;
  }

  if (!(fbos = wl_container_of(addon, fbos, addon))) {
    goto create_new;
  }
  return fbos;

create_new:;
  struct fx_renderer* renderer = fx_get_renderer(output->renderer);
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
    wlr_log(WLR_ERROR, "Could not assign fx_offscreen_buffers to output: '%s'", output->name);
    free(fbos);
    return NULL;
  }
  wl_list_insert(&renderer->offscreen_buffers, &fbos->link);
  return fbos;
}
