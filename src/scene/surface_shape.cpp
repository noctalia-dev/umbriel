#include "scene/surface_shape.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  void applySurfaceShape(wlr_scene_buffer* buffer, wlr_surface* surface, const pixman_region32_t* shape) {
    if (shape == nullptr) {
      wlr_scene_node_set_clip_region(&buffer->node, nullptr);
      return;
    }
    pixman_region32_t clip;
    pixman_region32_init(&clip);
    wlr_fbox full{};
    wlr_surface_get_buffer_source_box(surface, &full);
    wlr_fbox source = buffer->src_box;
    if (source.width <= 0 || source.height <= 0) {
      source = full;
    }
    const int width = buffer->dst_width > 0 ? buffer->dst_width : surface->current.width;
    const int height = buffer->dst_height > 0 ? buffer->dst_height : surface->current.height;
    if (surface->current.width > 0
        && surface->current.height > 0
        && source.width > 0
        && source.height > 0
        && width > 0
        && height > 0) {
      // Xwayland's surface coordinates name the X window's pixels. Its scene surface can be drawn at a different
      // logical size, then cropped again while a resize is in progress or an overview card is laid out.
      const double sx = full.width / surface->current.width;
      const double sy = full.height / surface->current.height;
      int count = 0;
      const pixman_box32_t* rectangles = pixman_region32_rectangles(shape, &count);
      for (int index = 0; index < count; ++index) {
        const pixman_box32_t& rect = rectangles[index];
        const double left = (full.x + rect.x1 * sx - source.x) * width / source.width;
        const double top = (full.y + rect.y1 * sy - source.y) * height / source.height;
        const double right = (full.x + rect.x2 * sx - source.x) * width / source.width;
        const double bottom = (full.y + rect.y2 * sy - source.y) * height / source.height;
        const int x1 = static_cast<int>(std::lround(std::clamp(left, 0.0, static_cast<double>(width))));
        const int y1 = static_cast<int>(std::lround(std::clamp(top, 0.0, static_cast<double>(height))));
        const int x2 = static_cast<int>(std::lround(std::clamp(right, 0.0, static_cast<double>(width))));
        const int y2 = static_cast<int>(std::lround(std::clamp(bottom, 0.0, static_cast<double>(height))));
        if (x2 > x1 && y2 > y1) {
          pixman_region32_union_rect(&clip, &clip, x1, y1, x2 - x1, y2 - y1);
        }
      }
    }
    wlr_scene_node_set_clip_region(&buffer->node, &clip);
    pixman_region32_fini(&clip);
  }

  void applySurfaceTreeShape(wlr_scene_node* node, wlr_surface* surface, const pixman_region32_t* shape) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
      wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
      wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
      if (sceneSurface != nullptr && sceneSurface->surface == surface) {
        applySurfaceShape(buffer, surface, shape);
      }
    } else if (node->type == WLR_SCENE_NODE_TREE) {
      wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &tree->children, link) { applySurfaceTreeShape(child, surface, shape); }
    }
  }

} // namespace umbriel
