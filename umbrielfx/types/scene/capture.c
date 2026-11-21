// Adapted from wlroots 0.20.2 types/ext_image_capture_source_v1/scene.c (MIT).
#include "types/wlr_output.h"
#include "types/wlr_scene.h"
#include "umbrielfx/types/scene_capture.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/util/log.h>

struct scene_node_source {
  struct wlr_ext_image_capture_source_v1 base;

  struct wlr_scene_node* node;

  struct wlr_backend backend;
  struct wlr_output output;
  struct wlr_scene_output* scene_output;

  size_t num_started;
  float scale;
  bool revoked;
  struct wl_event_source* revoke_timer;

  struct wl_listener node_destroy;
  struct wl_listener scene_output_destroy;
  struct wl_listener output_frame;
};

struct scene_node_source_frame_event {
  struct wlr_ext_image_capture_source_v1_frame_event base;
  struct wlr_buffer* buffer;
  struct timespec when;
};

static size_t last_output_num = 0;

static void
_get_scene_node_extents(struct wlr_scene_node* node, int lx, int ly, int* x_min, int* y_min, int* x_max, int* y_max) {
  // Unmapped auxiliary surfaces and clipped children do not contribute visible bounds.
  if (!node->enabled) {
    return;
  }
  if (node->type == WLR_SCENE_NODE_BUFFER && wlr_scene_buffer_from_node(node)->buffer == NULL) {
    return;
  }

  switch (node->type) {
  case WLR_SCENE_NODE_TREE:;
    struct wlr_scene_tree* scene_tree = wlr_scene_tree_from_node(node);
    struct wlr_scene_node* child;
    wl_list_for_each(child, &scene_tree->children, link) {
      _get_scene_node_extents(child, lx + child->x, ly + child->y, x_min, y_min, x_max, y_max);
    }
    break;
  case WLR_SCENE_NODE_RECT:
  case WLR_SCENE_NODE_BORDER:
  case WLR_SCENE_NODE_SHADOW:
  case WLR_SCENE_NODE_OPTIMIZED_BLUR:
  case WLR_SCENE_NODE_BLUR:
  case WLR_SCENE_NODE_BUFFER:;
    struct wlr_box node_box = {.x = lx, .y = ly};
    scene_node_get_size(node, &node_box.width, &node_box.height);
    if (node_box.width <= 0 || node_box.height <= 0) {
      break;
    }

    if (node_box.x < *x_min) {
      *x_min = node_box.x;
    }
    if (node_box.y < *y_min) {
      *y_min = node_box.y;
    }
    if (node_box.x + node_box.width > *x_max) {
      *x_max = node_box.x + node_box.width;
    }
    if (node_box.y + node_box.height > *y_max) {
      *y_max = node_box.y + node_box.height;
    }
    break;
  }
}

static void get_scene_node_extents(struct wlr_scene_node* node, struct wlr_box* box) {
  int lx = 0, ly = 0;
  wlr_scene_node_coords(node, &lx, &ly);
  *box = (struct wlr_box){.x = INT_MAX, .y = INT_MAX};
  int x_max = INT_MIN, y_max = INT_MIN;
  _get_scene_node_extents(node, lx, ly, &box->x, &box->y, &x_max, &y_max);
  if (box->x == INT_MAX || box->y == INT_MAX) {
    *box = (struct wlr_box){0};
    return;
  }
  box->width = x_max - box->x;
  box->height = y_max - box->y;
}

static void source_render(struct scene_node_source* source) {
  if (source->revoked) {
    return;
  }
  struct wlr_scene_output* scene_output = source->scene_output;

  struct wlr_box extents;
  get_scene_node_extents(source->node, &extents);

  if (extents.width <= 0 || extents.height <= 0) {
    return;
  }

  wlr_scene_output_set_position(scene_output, extents.x, extents.y);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  // Scene extents stay logical. Only the capture output's render target is physical.
  double width = ceil(extents.width * (double)source->scale);
  double height = ceil(extents.height * (double)source->scale);
  if (width > INT_MAX || height > INT_MAX) {
    wlr_output_state_finish(&state);
    return;
  }
  wlr_output_state_set_scale(&state, source->scale);
  wlr_output_state_set_custom_mode(&state, (int)width, (int)height, 0);
  bool ok =
      wlr_scene_output_build_state(scene_output, &state, NULL) && wlr_output_commit_state(scene_output->output, &state);
  wlr_output_state_finish(&state);

  if (!ok) {
    // TODO: send failure
    return;
  }
}

static void source_start(struct wlr_ext_image_capture_source_v1* base, bool with_cursors) {
  struct scene_node_source* source = wl_container_of(base, source, base);

  source->num_started++;
  if (source->num_started > 1) {
    return;
  }

  source_render(source);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(source->scene_output, &now);
}

static void source_stop(struct wlr_ext_image_capture_source_v1* base) {
  struct scene_node_source* source = wl_container_of(base, source, base);

  source->num_started--;
  if (source->num_started > 0) {
    return;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, false);
  wlr_output_commit_state(&source->output, &state);
  wlr_output_state_finish(&state);
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1* base, bool schedule_frame) {
  struct scene_node_source* source = wl_container_of(base, source, base);
  if (source->output.frame_pending) {
    wlr_output_send_frame(&source->output);
  }
  if (schedule_frame) {
    wlr_output_update_needs_frame(&source->output);
  }
}

static void source_copy_frame(
    struct wlr_ext_image_capture_source_v1* base, struct wlr_ext_image_copy_capture_frame_v1* frame,
    struct wlr_ext_image_capture_source_v1_frame_event* base_event
) {
  struct scene_node_source* source = wl_container_of(base, source, base);
  struct scene_node_source_frame_event* event = wl_container_of(base_event, event, base);

  if (source->revoked) {
    wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    return;
  }
  if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, event->buffer, source->output.renderer)) {
    wlr_ext_image_copy_capture_frame_v1_ready(frame, source->output.transform, &event->when);
  }
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop = source_stop,
    .request_frame = source_request_frame,
    .copy_frame = source_copy_frame,
};

static const struct wlr_backend_impl backend_impl = {0};

static void source_update_buffer_constraints(struct scene_node_source* source, const struct wlr_output_state* state) {
  struct wlr_output* output = &source->output;

  if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
    return;
  }

  wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base, output->swapchain, output->renderer);
}

static bool output_test(struct wlr_output* output, const struct wlr_output_state* state) {
  uint32_t supported =
      WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
  if ((state->committed & ~supported) != 0) {
    return false;
  }

  if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
    int pending_width, pending_height;
    output_pending_resolution(output, state, &pending_width, &pending_height);
    if (state->buffer->width != pending_width || state->buffer->height != pending_height) {
      return false;
    }
    struct wlr_fbox src_box;
    src_box = state->buffer_src_box;
    if (src_box.width == 0.0 && src_box.height == 0.0) {
      src_box.width = state->buffer->width;
      src_box.height = state->buffer->height;
    }
    if (src_box.x != 0.0
        || src_box.y != 0.0
        || src_box.width != (double)state->buffer->width
        || src_box.height != (double)state->buffer->height) {
      return false;
    }
  }

  return true;
}

static bool output_commit(struct wlr_output* output, const struct wlr_output_state* state) {
  struct scene_node_source* source = wl_container_of(output, source, output);

  if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled) {
    return true;
  }

  if (state->committed & WLR_OUTPUT_STATE_MODE) {
    source_update_buffer_constraints(source, state);
  }

  if (!(state->committed & WLR_OUTPUT_STATE_BUFFER)) {
    wlr_log(WLR_DEBUG, "Failed to commit capture output: missing buffer");
    return false;
  }

  struct wlr_buffer* buffer = state->buffer;

  pixman_region32_t full_damage;
  pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

  const pixman_region32_t* damage;
  if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
    damage = &state->damage;
  } else {
    damage = &full_damage;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  struct scene_node_source_frame_event frame_event = {
      .base = {.damage = damage},
      .buffer = buffer,
      .when = now,
  };
  wl_signal_emit_mutable(&source->base.events.frame, &frame_event.base);

  pixman_region32_fini(&full_damage);

  return true;
}

static const struct wlr_output_impl output_impl = {
    .test = output_test,
    .commit = output_commit,
};

static void source_destroy(struct scene_node_source* source) {
  if (source->revoke_timer != NULL) {
    wl_event_source_remove(source->revoke_timer);
  }
  wl_list_remove(&source->node_destroy.link);
  wl_list_remove(&source->scene_output_destroy.link);
  wl_list_remove(&source->output_frame.link);
  wlr_ext_image_capture_source_v1_finish(&source->base);
  wlr_scene_output_destroy(source->scene_output);
  wlr_output_finish(&source->output);
  wlr_backend_finish(&source->backend);
  free(source);
}

static int source_finish_revoke(void* data) {
  source_destroy(data);
  return 0;
}

void umbrielfx_scene_capture_source_revoke(struct wlr_ext_image_capture_source_v1* base) {
  if (base == NULL) {
    return;
  }
  assert(base->impl == &source_impl);
  struct scene_node_source* source = wl_container_of(base, source, base);
  if (!source->revoked) {
    source->revoked = true;
    wl_event_source_timer_update(source->revoke_timer, 1);
  }
}

static void source_handle_node_destroy(struct wl_listener* listener, void* data) {
  struct scene_node_source* source = wl_container_of(listener, source, node_destroy);
  source_destroy(source);
}

static void source_handle_scene_output_destroy(struct wl_listener* listener, void* data) {
  struct scene_node_source* source = wl_container_of(listener, source, scene_output_destroy);
  source->scene_output = NULL;
  wl_list_remove(&source->scene_output_destroy.link);
  wl_list_init(&source->scene_output_destroy.link);
}

static void source_handle_output_frame(struct wl_listener* listener, void* data) {
  struct scene_node_source* source = wl_container_of(listener, source, output_frame);
  if (source->scene_output == NULL) {
    return;
  }

  if (!wlr_scene_output_needs_frame(source->scene_output)) {
    return;
  }

  // We can only emit frames with damage
  if (!pixman_region32_empty(&source->scene_output->pending_commit_damage)) {
    source_render(source);
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(source->scene_output, &now);
}

struct wlr_ext_image_capture_source_v1* wlr_ext_image_capture_source_v1_create_with_scene_node(
    struct wlr_scene_node* node, struct wl_event_loop* event_loop, struct wlr_allocator* allocator,
    struct wlr_renderer* renderer
) {
  struct scene_node_source* source = calloc(1, sizeof(*source));
  if (source == NULL) {
    return NULL;
  }

  source->node = node;
  source->scale = 1.0f;

  wlr_ext_image_capture_source_v1_init(&source->base, &source_impl);

  wlr_backend_init(&source->backend, &backend_impl);
  source->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

  wlr_output_init(&source->output, &source->backend, &output_impl, event_loop, NULL);
  // The synthetic output's `data` must stay NULL (the calloc above): shells
  // resolve their own output binding through `wlr_output::data` and treat
  // data-less outputs as capture mirrors — excluded from client presentation
  // hints.

  size_t output_num = ++last_output_num;
  char name[64];
  snprintf(name, sizeof(name), "CAPTURE-%zu", output_num);
  wlr_output_set_name(&source->output, name);

  if (!wlr_output_init_render(&source->output, allocator, renderer)) {
    wlr_output_finish(&source->output);
    wlr_backend_finish(&source->backend);
    wlr_ext_image_capture_source_v1_finish(&source->base);
    free(source);
    return NULL;
  }

  struct wlr_scene* scene = scene_node_get_root(node);
  source->scene_output = wlr_scene_output_create(scene, &source->output);
  if (source->scene_output == NULL) {
    wlr_output_finish(&source->output);
    wlr_backend_finish(&source->backend);
    wlr_ext_image_capture_source_v1_finish(&source->base);
    free(source);
    return NULL;
  }

  source->node_destroy.notify = source_handle_node_destroy;
  wl_signal_add(&node->events.destroy, &source->node_destroy);

  source->scene_output_destroy.notify = source_handle_scene_output_destroy;
  wl_signal_add(&source->scene_output->events.destroy, &source->scene_output_destroy);

  source->output_frame.notify = source_handle_output_frame;
  wl_signal_add(&source->output.events.frame, &source->output_frame);

  // Allocate the safe teardown boundary before any session can reference us.
  source->revoke_timer = wl_event_loop_add_timer(event_loop, source_finish_revoke, source);
  if (source->revoke_timer == NULL) {
    source_destroy(source);
    return NULL;
  }
  return &source->base;
}

void umbrielfx_scene_capture_source_set_scale(struct wlr_ext_image_capture_source_v1* base, float scale) {
  assert(base->impl == &source_impl);
  assert(isfinite(scale) && scale > 0.0f);
  struct scene_node_source* source = wl_container_of(base, source, base);
  if (source->scale == scale) {
    return;
  }
  source->scale = scale;
  // Cached but idle sources must not allocate or render until capture starts.
  if (source->num_started > 0 && source->scene_output != NULL) {
    source_render(source);
  }
}
