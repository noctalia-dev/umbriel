#ifndef UMBRIELFX_TYPES_LINUX_DRM_SYNCOBJ_H
#define UMBRIELFX_TYPES_LINUX_DRM_SYNCOBJ_H

#include <stdint.h>

struct wlr_drm_syncobj_timeline;
struct wlr_surface;
struct wlr_surface_state;

/**
 * Returns the release timeline of the surface's current linux-drm-syncobj
 * state and stores its point, or returns NULL when the surface has none.
 * wlroots keeps these fields private.
 */
struct wlr_drm_syncobj_timeline*
umbrielfx_linux_drm_syncobj_release_point(struct wlr_surface* surface, uint64_t* point);

/**
 * Returns the release timeline of one of the surface's cached states, one that
 * wlroots has not applied yet, and stores its point. Returns NULL when that
 * commit attached no explicitly synchronized buffer.
 */
struct wlr_drm_syncobj_timeline* umbrielfx_linux_drm_syncobj_cached_release_point(
    struct wlr_surface* surface, struct wlr_surface_state* cached, uint64_t* point
);

#endif
