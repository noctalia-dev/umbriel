// Pin the presentation-hint split between a provider scene (the desktop) and a
// render-only mirror scene (window capture) that wrap the same client surface.
// The mirror keeps output membership for capture frame pacing but must not
// publish scale hints, and the provider's derivation must ignore outputs whose
// provider answer is non-positive (the capture mirror's synthetic output).
#undef NDEBUG
#include "umbrielfx/types/wlr_scene.h"

#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>

static int failures;
#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "%d: %s\n", __LINE__, #condition);                                                               \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void emit_outputs_update(struct wlr_scene_surface* surface, struct wlr_scene_output* output) {
  struct wlr_scene_outputs_update_event event = {.active = &output, .size = output ? 1 : 0};
  wl_signal_emit_mutable(&surface->buffer->events.outputs_update, &event);
}

static struct wlr_surface_output* membership(struct wlr_surface* surface, struct wlr_output* output) {
  struct wlr_surface_output* item;
  wl_list_for_each(item, &surface->current_outputs, link) {
    if (item->output == output) {
      return item;
    }
  }
  return NULL;
}

// Production shape (Server's provider): the bound desktop output answers with
// the client render scale, anything else is a capture mirror's synthetic
// output and is excluded with a non-positive answer.
static struct wlr_output* monitor_wlr;
static double provider_scale(struct wlr_output* output, void* data) { return output == monitor_wlr ? 2.0 : -1.0; }

int main(void) {
  struct wl_display* display = wl_display_create();
  assert(display);
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  struct wl_client* client = wl_client_create(display, sockets[0]);
  assert(client);
  struct wlr_backend* backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
  assert(backend);
  struct wlr_output* monitor = wlr_headless_add_output(backend, 800, 600);
  struct wlr_output* capture = wlr_headless_add_output(backend, 800, 600);
  assert(monitor && capture);
  // The raw scales must not reach the client: the provider answers 2.0 for the
  // monitor and excludes the capture output even though its raw scale is the
  // highest of the three (any leak shows up as a hint of 3).
  monitor->scale = 1.0;
  capture->scale = 3.0;
  monitor_wlr = monitor;

  struct wlr_scene* desktop = wlr_scene_create();
  struct wlr_scene* mirror = wlr_scene_create();
  assert(desktop && mirror);
  wlr_scene_set_preferred_scale_provider(desktop, provider_scale, NULL);
  struct wlr_scene_output* desktop_output = wlr_scene_output_create(desktop, monitor);
  struct wlr_scene_output* capture_output = wlr_scene_output_create(mirror, capture);
  assert(desktop_output && capture_output);

  // Hand-built surface: only the fields the scene-surface listeners and
  // wlr_surface_send_enter/leave touch. A wlroots bump can add more.
  struct wlr_surface surface = {0};
  // Version 6 resources can receive preferred_buffer_scale; older ones keep
  // the surface's hint at its zero default and hide the sends under test.
  surface.resource = wl_resource_create(client, &wl_surface_interface, wl_surface_interface.version, 0);
  assert(surface.resource);
  wl_list_init(&surface.current_outputs);
  wl_list_init(&surface.current.frame_callback_list);
  wl_signal_init(&surface.events.destroy);
  wl_signal_init(&surface.events.commit);
  wlr_addon_set_init(&surface.addons);
  pixman_region32_init(&surface.opaque_region);
  struct wlr_scene_surface* real = wlr_scene_surface_create(&desktop->tree, &surface);
  struct wlr_scene_surface* copy = wlr_scene_surface_create(&mirror->tree, &surface);
  assert(real && copy);

  // Desktop enter: the hint comes from the provider (2.0), not monitor->scale.
  emit_outputs_update(real, desktop_output);
  CHECK(membership(&surface, monitor) != NULL);
  CHECK(surface.preferred_buffer_scale == 2);

  // Capture starts: the mirror keeps membership for frame pacing but publishes
  // no hint, so the raw capture scale (3.0) must not reach the client.
  emit_outputs_update(copy, capture_output);
  CHECK(membership(&surface, capture) != NULL);
  CHECK(surface.preferred_buffer_scale == 2);

  // Re-derivation on the desktop scene with both memberships present: the
  // mirror output's non-positive provider answer excludes it even though its
  // raw scale is the highest.
  emit_outputs_update(real, desktop_output);
  CHECK(surface.preferred_buffer_scale == 2);

  // A hidden window (its only desktop update is a suspend) captured by the
  // mirror alone still gets no hint from the mirror.
  emit_outputs_update(real, NULL);
  emit_outputs_update(copy, capture_output);
  CHECK(surface.preferred_buffer_scale == 2);

  wl_signal_emit_mutable(&surface.events.destroy, &surface);
  wlr_scene_node_destroy(&desktop->tree.node);
  wlr_scene_node_destroy(&mirror->tree.node);
  wlr_backend_destroy(backend);
  pixman_region32_fini(&surface.opaque_region);
  wlr_addon_set_finish(&surface.addons);
  wl_display_destroy_clients(display);
  wl_display_destroy(display);
  close(sockets[1]);
  return failures ? 1 : 0;
}
