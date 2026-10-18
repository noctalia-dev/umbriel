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

struct wlr_drm_syncobj_timeline* umbrielfx_linux_drm_syncobj_cached_release_point(
    struct wlr_surface* surface, struct wlr_surface_state* cached, uint64_t* point
) {
  struct wlr_linux_drm_syncobj_surface_v1_state* current = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
  if (current == NULL) {
    return NULL;
  }
  // The syncobj extension keeps one state per surface state, at its wlr_surface_synced index. Its current state is
  // the only one wlroots exposes, so find the index that holds it.
  void** currentStates = surface->current.synced.data;
  void** cachedStates = cached->synced.data;
  struct wlr_surface_synced* synced;
  wl_list_for_each(synced, &surface->synced, link) {
    if (currentStates[synced->index] != current) {
      continue;
    }
    struct wlr_linux_drm_syncobj_surface_v1_state* state = cachedStates[synced->index];
    if (state == NULL || state->release_timeline == NULL) {
      return NULL;
    }
    *point = state->release_point;
    return state->release_timeline;
  }
  return NULL;
}
