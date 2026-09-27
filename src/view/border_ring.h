#pragma once

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace umbriel {

  // Raster bounds, content hole, and nested radii for a single-pass border.
  struct BorderRing {
    wlr_box box;
    wlr_box hole;
    fx_corner_radii inner;
    fx_corner_radii seam;
    fx_corner_radii outer;
  };

  // Smoothly reduces a positive outer radius without collapsing an inset contour to square.
  [[nodiscard]] constexpr int nestedRadius(int radius, int inset) {
    if (radius <= 0) {
      return 0;
    }
    const int denominator = radius + inset;
    const int rounded = (radius * radius + denominator / 2) / denominator;
    return rounded > 0 ? rounded : 1;
  }

  // `padding` is transparent space around the ring that a border effect may paint into.
  [[nodiscard]] BorderRing
  makeBorderRing(int contentWidth, int contentHeight, int outerRadius, int innerWidth, int outerWidth, int padding = 0);

} // namespace umbriel
