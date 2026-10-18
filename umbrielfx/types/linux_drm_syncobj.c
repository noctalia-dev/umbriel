#include "umbrielfx/types/linux_drm_syncobj.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

struct wlr_drm_syncobj_timeline*
umbrielfx_linux_drm_syncobj_release_point(struct wlr_surface* surface, uint64_t* point) {
  struct wlr_linux_drm_syncobj_surface_v1_state* state = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
  if (state == NULL || state->release_timeline == NULL) {
    return NULL;
  }
  *point = state->release_point;
  return state->release_timeline;
}

// The syncobj extension keeps one state per surface state, at its wlr_surface_synced index. Its current state is the
// only one wlroots exposes, so find the index that holds it.
static struct wlr_linux_drm_syncobj_surface_v1_state*
syncobj_state(struct wlr_surface* surface, struct wlr_surface_state* surfaceState) {
  struct wlr_linux_drm_syncobj_surface_v1_state* current = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
  if (current == NULL) {
    return NULL;
  }
  void** currentStates = surface->current.synced.data;
  void** states = surfaceState->synced.data;
  struct wlr_surface_synced* synced;
  wl_list_for_each(synced, &surface->synced, link) {
    if (currentStates[synced->index] == current) {
      return states[synced->index];
    }
  }
  return NULL;
}

struct wlr_drm_syncobj_timeline* umbrielfx_linux_drm_syncobj_state_release_point(
    struct wlr_surface* surface, struct wlr_surface_state* surfaceState, uint64_t* point
) {
  struct wlr_linux_drm_syncobj_surface_v1_state* state = syncobj_state(surface, surfaceState);
  if (state == NULL || state->release_timeline == NULL) {
    return NULL;
  }
  *point = state->release_point;
  return state->release_timeline;
}

struct wlr_drm_syncobj_timeline* umbrielfx_linux_drm_syncobj_state_acquire_point(
    struct wlr_surface* surface, struct wlr_surface_state* surfaceState, uint64_t* point
) {
  struct wlr_linux_drm_syncobj_surface_v1_state* state = syncobj_state(surface, surfaceState);
  if (state == NULL || state->acquire_timeline == NULL) {
    return NULL;
  }
  *point = state->acquire_point;
  return state->acquire_timeline;
}
