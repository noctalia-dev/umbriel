#include "view/border_ring.h"

namespace umbriel {

  BorderRing
  makeBorderRing(int contentWidth, int contentHeight, int outerRadius, int innerWidth, int outerWidth, int padding) {
    const int thickness = innerWidth + outerWidth;
    // One transparent logical pixel gives fractional outer coverage a fragment
    // to land in instead of clipping it at the scene box.
    const int renderMargin = thickness > 0 ? 1 : 0;
    const int extent = thickness + renderMargin + (padding > 0 ? padding : 0);
    const int innerRadius = nestedRadius(outerRadius, thickness);
    const int seamRadius = nestedRadius(outerRadius, outerWidth);
    return {
        .box = {-extent, -extent, contentWidth + 2 * extent, contentHeight + 2 * extent},
        .hole = {extent, extent, contentWidth, contentHeight},
        .inner = corner_radii_new(innerRadius, innerRadius, innerRadius, innerRadius),
        .seam = corner_radii_new(seamRadius, seamRadius, seamRadius, seamRadius),
        .outer = corner_radii_new(outerRadius, outerRadius, outerRadius, outerRadius),
    };
  }

} // namespace umbriel
