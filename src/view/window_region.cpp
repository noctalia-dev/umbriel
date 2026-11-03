#include "view/window_region.h"

#include <algorithm>
#include <array>
#include <utility>

extern "C" {
#include <wlr/util/edges.h>
}

namespace umbriel {
  namespace {
    constexpr std::array kNames{"left",     "right",     "top",         "bottom",
                                "top_left", "top_right", "bottom_left", "bottom_right"};
  }

  std::string_view windowRegionName(WindowRegion region) { return kNames[static_cast<size_t>(region)]; }

  std::optional<WindowRegion> windowRegionNamed(std::string_view name) {
    for (size_t i = 0; i < kNames.size(); ++i) {
      if (name == kNames[i]) {
        return static_cast<WindowRegion>(i);
      }
    }
    return std::nullopt;
  }

  wlr_box windowRegionBox(const wlr_box& area, WindowRegion region, int spacing) {
    const bool right =
        region == WindowRegion::Right || region == WindowRegion::TopRight || region == WindowRegion::BottomRight;
    const bool splitX = region != WindowRegion::Top && region != WindowRegion::Bottom;
    const bool splitY = region != WindowRegion::Left && region != WindowRegion::Right;
    const bool bottom =
        region == WindowRegion::Bottom || region == WindowRegion::BottomLeft || region == WindowRegion::BottomRight;
    const int gapX = std::clamp(spacing, 0, std::max(0, area.width));
    const int gapY = std::clamp(spacing, 0, std::max(0, area.height));
    const int leftWidth = std::max(0, area.width - gapX) / 2;
    const int topHeight = std::max(0, area.height - gapY) / 2;
    return {
        .x = area.x + (right ? leftWidth + gapX : 0),
        .y = area.y + (bottom ? topHeight + gapY : 0),
        .width = splitX ? (right ? std::max(0, area.width - gapX - leftWidth) : leftWidth) : area.width,
        .height = splitY ? (bottom ? std::max(0, area.height - gapY - topHeight) : topHeight) : area.height,
    };
  }

  std::optional<WindowRegion> dropZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges) {
    if (output.width <= 0
        || output.height <= 0
        || x < output.x
        || y < output.y
        || x >= output.x + output.width
        || y >= output.y + output.height) {
      return std::nullopt;
    }
    const double left = x - output.x;
    const double right = output.x + output.width - x;
    const double top = y - output.y;
    const double bottom = output.y + output.height - y;
    const bool onLeft = left < kDropZoneEdge && (closedEdges & WLR_EDGE_LEFT) == 0;
    const bool onRight = right <= kDropZoneEdge && (closedEdges & WLR_EDGE_RIGHT) == 0;
    const bool onTop = top < kDropZoneEdge && (closedEdges & WLR_EDGE_TOP) == 0;
    const bool onBottom = bottom <= kDropZoneEdge && (closedEdges & WLR_EDGE_BOTTOM) == 0;
    if ((onLeft && top < kDropZoneCornerSpan) || (onTop && left < kDropZoneCornerSpan))
      return WindowRegion::TopLeft;
    if ((onRight && top < kDropZoneCornerSpan) || (onTop && right <= kDropZoneCornerSpan))
      return WindowRegion::TopRight;
    if ((onLeft && bottom <= kDropZoneCornerSpan) || (onBottom && left < kDropZoneCornerSpan))
      return WindowRegion::BottomLeft;
    if ((onRight && bottom <= kDropZoneCornerSpan) || (onBottom && right <= kDropZoneCornerSpan))
      return WindowRegion::BottomRight;
    if (onTop)
      return WindowRegion::Top;
    if (onBottom)
      return WindowRegion::Bottom;
    if (onLeft)
      return WindowRegion::Left;
    if (onRight)
      return WindowRegion::Right;
    return std::nullopt;
  }
} // namespace umbriel
