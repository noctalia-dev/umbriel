#include "layout/layout_motion.h"

#include <algorithm>
#include <cmath>

namespace umbriel {

  namespace {

    double lerpEdge(int from, int to, double progress) {
      return std::lerp(static_cast<double>(from), static_cast<double>(to), progress);
    }

  } // namespace

  wlr_fbox interpolateBox(const wlr_box& from, const wlr_box& to, double progress) {
    const double left = lerpEdge(from.x, to.x, progress);
    const double top = lerpEdge(from.y, to.y, progress);
    const double right = lerpEdge(from.x + from.width, to.x + to.width, progress);
    const double bottom = lerpEdge(from.y + from.height, to.y + to.height, progress);
    return {left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
  }

  wlr_fbox interpolateBox(const wlr_box& from, const wlr_box& to, double positionProgress, double sizeProgress) {
    if (positionProgress == sizeProgress) {
      return interpolateBox(from, to, positionProgress);
    }
    return {
        lerpEdge(from.x, to.x, positionProgress),
        lerpEdge(from.y, to.y, positionProgress),
        std::max(0.0, lerpEdge(from.width, to.width, sizeProgress)),
        std::max(0.0, lerpEdge(from.height, to.height, sizeProgress)),
    };
  }

  wlr_fbox interpolatePresentationBox(
      const wlr_box& from, const wlr_box& to, double positionProgress, double sizeProgress, double outputScale,
      const wlr_box& outputBox, int outputPixelWidth, int outputPixelHeight, const wlr_fbox* presentedFrom
  ) {
    if (positionProgress == sizeProgress && (positionProgress == 0.0 || positionProgress == 1.0)) {
      if (positionProgress == 0.0 && presentedFrom != nullptr) {
        return *presentedFrom;
      }
      return interpolateBox(from, to, positionProgress);
    }
    const auto align = [outputScale](int edge, int origin, int extent, int pixels) {
      const double physical = pixels > 0 && edge == origin + extent
          ? static_cast<double>(pixels)
          : std::round((static_cast<double>(edge) - origin) * outputScale);
      return origin + physical / outputScale;
    };
    const auto alignedBox = [&](const wlr_box& box) {
      const double left = align(box.x, outputBox.x, outputBox.width, outputPixelWidth);
      const double top = align(box.y, outputBox.y, outputBox.height, outputPixelHeight);
      const double right = align(box.x + box.width, outputBox.x, outputBox.width, outputPixelWidth);
      const double bottom = align(box.y + box.height, outputBox.y, outputBox.height, outputPixelHeight);
      return wlr_fbox{left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
    };
    const wlr_fbox alignedFrom = presentedFrom != nullptr ? *presentedFrom : alignedBox(from);
    const wlr_fbox alignedTo = alignedBox(to);
    const double x = std::lerp(alignedFrom.x, alignedTo.x, positionProgress);
    const double y = std::lerp(alignedFrom.y, alignedTo.y, positionProgress);
    if (positionProgress == sizeProgress) {
      const double right = std::lerp(alignedFrom.x + alignedFrom.width, alignedTo.x + alignedTo.width, sizeProgress);
      const double bottom = std::lerp(alignedFrom.y + alignedFrom.height, alignedTo.y + alignedTo.height, sizeProgress);
      return {x, y, std::max(0.0, right - x), std::max(0.0, bottom - y)};
    }
    return {
        x,
        y,
        std::max(0.0, std::lerp(alignedFrom.width, alignedTo.width, sizeProgress)),
        std::max(0.0, std::lerp(alignedFrom.height, alignedTo.height, sizeProgress)),
    };
  }

  bool keepsSeparation(const MotionBox& a, const MotionBox& b) {
    // `lo` entirely on the low side of `hi` along one axis, touching allowed.
    const auto leftOf = [](const wlr_box& lo, const wlr_box& hi) { return lo.x + lo.width <= hi.x; };
    const auto above = [](const wlr_box& lo, const wlr_box& hi) { return lo.y + lo.height <= hi.y; };
    return (leftOf(a.from, b.from) && leftOf(a.to, b.to))
        || (leftOf(b.from, a.from) && leftOf(b.to, a.to))
        || (above(a.from, b.from) && above(a.to, b.to))
        || (above(b.from, a.from) && above(b.to, a.to));
  }

} // namespace umbriel
