#pragma once

#include "view/border_ring.h"

#include <array>
#include <vector>

struct wlr_scene_border;
struct wlr_scene_node;
struct wlr_scene_tree;

namespace umbriel {

  struct BorderSnapshot {
    wlr_scene_border* node = nullptr;
    std::array<float, 4> innerColor{};
    std::array<float, 4> outerColor{};
    // Unscaled ring the snapshot was taken with, so a closing shrink collapses what was drawn.
    int innerWidth = 0;
    int outerWidth = 0;
    int cornerRadius = 0;
    // Effect padding the ring was drawn with.
    int padding = 0;
  };

  // Position and size the single-pass border relative to the content origin.
  // The render margin belongs only to raster coverage; widths remain logical.
  void applyBorderGeometry(wlr_scene_border* border, const BorderRing& ring, int innerWidth, int outerWidth);

  // Copy `border` into a close-animation snapshot tree at (x, y). `effect` is the node carrying the ring's persistent
  // effect slots, frozen on the copy. `ring` holds the straight colours and metrics, both colours faded by `opacity`.
  void snapshotBorder(
      wlr_scene_tree* snapshot, const wlr_scene_border& border, int x, int y, wlr_scene_node* effect,
      BorderSnapshot ring, float opacity, std::vector<BorderSnapshot>& out
  );

} // namespace umbriel
