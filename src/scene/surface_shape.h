#pragma once

#include <pixman.h>

struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_surface;

namespace umbriel {

  // Project a surface-local bounding shape through the buffer's current source crop and destination size.
  // NULL removes the mask; an empty region hides the buffer. Surface coordinates remain independent of output scale.
  void applySurfaceShape(wlr_scene_buffer* buffer, wlr_surface* surface, const pixman_region32_t* shape);
  // Applies the shape to copies of this surface in a live or capture subsurface tree, including disabled copies.
  void applySurfaceTreeShape(wlr_scene_node* node, wlr_surface* surface, const pixman_region32_t* shape);

} // namespace umbriel
