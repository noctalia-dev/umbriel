#include "scene/border_rect.h"

extern "C" {
#include <umbrielfx/render/effect.h>
#include <umbrielfx/types/fx/clipped_region.h>
}

// clang-format off
#include "wlr.h"
// clang-format on

namespace umbriel {

  void applyBorderGeometry(wlr_scene_border* border, const BorderRing& ring, int innerWidth, int outerWidth) {
    if (border == nullptr) {
      return;
    }

    wlr_scene_node_set_position(&border->node, ring.box.x, ring.box.y);
    wlr_scene_border_set_geometry(
        border, ring.box.width, ring.box.height, innerWidth, outerWidth,
        clipped_region{.area = ring.hole, .corners = ring.inner}, ring.seam, ring.outer
    );
  }

  void snapshotBorder(
      wlr_scene_tree* snapshot, const wlr_scene_border& border, int x, int y, wlr_scene_node* effect,
      BorderSnapshot ring, float opacity, std::vector<BorderSnapshot>& out
  ) {
    wlr_scene_border* copy = wlr_scene_border_create(snapshot, border.inner_color, border.outer_color);
    if (copy == nullptr) {
      return;
    }
    wlr_scene_border_set_geometry(
        copy, border.width, border.height, border.inner_width, border.outer_width, border.clipped_region,
        border.seam_corners, border.outer_corners
    );
    wlr_scene_node_set_position(&copy->node, x, y);
    if (effect != nullptr) {
      wlr_scene_node_copy_animations_for_snapshot(&copy->node, effect);
    }
    ring.node = copy;
    ring.innerColor[3] *= opacity;
    ring.outerColor[3] *= opacity;
    out.push_back(ring);
  }

} // namespace umbriel
