// Exercise the real scene-surface listeners with two scenes sharing a surface.
// No renderer or running desktop is required.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>

#include "umbrielfx/types/wlr_scene.h"

static int failures;
#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			fprintf(stderr, "%d: %s\n", __LINE__, #condition);                                     \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

static void update(struct wlr_scene_surface *surface, struct wlr_scene_output *output) {
	struct wlr_scene_outputs_update_event event = {.active = &output, .size = output ? 1 : 0};
	wl_signal_emit_mutable(&surface->buffer->events.outputs_update, &event);
}

static struct wlr_surface_output *membership(struct wlr_surface *surface,
											 struct wlr_output *output) {
	struct wlr_surface_output *item;
	wl_list_for_each(item, &surface->current_outputs, link) {
		if (item->output == output) {
			return item;
		}
	}
	return NULL;
}

static int callbacks;
static void callback_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	callbacks++;
}

static void request_frame(struct wlr_surface *surface, struct wl_client *client) {
	struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1, 0);
	assert(callback);
	wl_resource_set_implementation(callback, NULL, NULL, callback_destroy);
	wl_list_insert(&surface->current.frame_callback_list, wl_resource_get_link(callback));
}

static void frame(struct wlr_scene_surface *surface, struct wlr_scene_output *output) {
	struct wlr_scene_frame_done_event event = {.output = output, .when = {1, 0}};
	wl_signal_emit_mutable(&surface->buffer->events.frame_done, &event);
}

int main(void) {
	struct wl_display *display = wl_display_create();
	assert(display);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
	struct wl_client *client = wl_client_create(display, sockets[0]);
	assert(client);
	struct wlr_backend *backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	assert(backend);
	struct wlr_output *monitor = wlr_headless_add_output(backend, 800, 600);
	struct wlr_output *capture = wlr_headless_add_output(backend, 800, 600);
	assert(monitor && capture);
	monitor->refresh = 60000;
	capture->refresh = 0;
	struct wlr_scene *desktop = wlr_scene_create(), *mirror = wlr_scene_create();
	assert(desktop && mirror);
	struct wlr_scene_output *desktop_output = wlr_scene_output_create(desktop, monitor);
	struct wlr_scene_output *capture_output = wlr_scene_output_create(mirror, capture);
	assert(desktop_output && capture_output);
	// Hand-built surface: only the fields the scene-surface listeners and
	// wlr_surface_send_enter/leave touch. A wlroots bump can add more.
	struct wlr_surface surface = {0};
	surface.resource = wl_resource_create(client, &wl_surface_interface, 1, 0);
	assert(surface.resource);
	wl_list_init(&surface.current_outputs);
	wl_list_init(&surface.current.frame_callback_list);
	wl_signal_init(&surface.events.destroy);
	wl_signal_init(&surface.events.commit);
	wlr_addon_set_init(&surface.addons);
	pixman_region32_init(&surface.opaque_region);
	struct wlr_scene_surface *real = wlr_scene_surface_create(&desktop->tree, &surface);
	struct wlr_scene_surface *copy = wlr_scene_surface_create(&mirror->tree, &surface);
	assert(real && copy);
	update(real, desktop_output);
	CHECK(membership(&surface, monitor) != NULL);
	// Starting capture must preserve membership and frame pacing on the monitor.
	update(copy, capture_output);
	CHECK(membership(&surface, monitor) != NULL);
	CHECK(membership(&surface, capture) != NULL);
	request_frame(&surface, client);
	frame(real, desktop_output);
	CHECK(callbacks == 1);
	update(real, desktop_output);
	CHECK(membership(&surface, capture) != NULL);
	// Stopping capture must not prevent the next desktop frame callback.
	update(copy, NULL);
	struct wlr_surface_output *capture_member = membership(&surface, capture);
	CHECK(capture_member != NULL && capture_member->suspended);
	frame(real, desktop_output);
	CHECK(callbacks == 1);
	request_frame(&surface, client);
	frame(real, desktop_output);
	CHECK(callbacks == 2);
	// A hidden window can be paced by capture; once capture stops, neither output
	// may deliver callbacks until the desktop or capture scene is active again.
	update(real, NULL);
	update(copy, capture_output);
	request_frame(&surface, client);
	frame(real, desktop_output);
	CHECK(callbacks == 2);
	frame(copy, capture_output);
	CHECK(callbacks == 3);
	update(copy, NULL);
	request_frame(&surface, client);
	frame(copy, capture_output);
	CHECK(callbacks == 3);
	update(real, desktop_output);
	frame(real, desktop_output);
	CHECK(callbacks == 4);
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
