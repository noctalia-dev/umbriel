#include "scene/ssd_frame_geometry.h"

#include <algorithm>
#include <ranges>

extern "C" {
#include <wlr/util/edges.h>
}

namespace umbriel {
  int titleBarButtonDiameter(int height, int size) {
    if (size >= 0) {
      return std::clamp(size, 1, std::max(1, height));
    }
    const int diameter = std::max(8, (height + 1) / 2);
    if ((height - diameter) % 2 == 0) {
      return diameter;
    }
    return diameter > 8 ? diameter - 1 : diameter + 1;
  }

  TitleBarLayout titleBarLayout(
      int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      const TitleBarButtonGeometry& geometry
  ) {
    const int diameter = titleBarButtonDiameter(height, geometry.size);
    const int verticalInset = (height - diameter) / 2;
    int padding = geometry.padding < 0 ? verticalInset : geometry.padding;
    int gap = geometry.gap;
    const int count = static_cast<int>(shown.size());
    // Shrink the padding, then the gap, to fit a narrow bar. Buttons keep their size, so a row that still does not fit
    // extends past the opposite edge.
    if (count > 0) {
      padding = std::max(0, std::min(padding, width - count * diameter - (count - 1) * gap));
      if (count > 1) {
        gap = std::max(0, std::min(gap, (width - count * diameter - padding) / (count - 1)));
      }
    }
    TitleBarLayout result{.title = {kTitleBarTitlePadding, 0, 0, height}, .gap = gap};
    // Walk from the edge inward: the last listed button is outermost.
    int x = side == TitleBarButtonSide::Right ? width - padding - diameter : padding;
    const int step = side == TitleBarButtonSide::Right ? -(diameter + gap) : diameter + gap;
    std::optional<int> innermost;
    for (const TitleBarButton button : std::views::reverse(shown)) {
      auto& slot = result.buttons[static_cast<size_t>(button)];
      if (slot) {
        continue;
      }
      slot = wlr_box{x, verticalInset, diameter, diameter};
      innermost = x;
      x += step;
    }
    if (side == TitleBarButtonSide::Right) {
      result.title.width = std::max(0, innermost.value_or(width) - 2 * kTitleBarTitlePadding);
    } else {
      if (innermost) {
        result.title.x = *innermost + diameter + kTitleBarTitlePadding;
      }
      result.title.width = std::max(0, width - result.title.x - kTitleBarTitlePadding);
    }
    return result;
  }

  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      const TitleBarButtonGeometry& geometry, double x, double y
  ) {
    if (cw <= 0 || ch <= 0 || h < 0) {
      return {};
    }
    if (x >= 0 && x < cw && y >= -h && y < 0) {
      const TitleBarLayout layout = titleBarLayout(cw, h, side, shown, geometry);
      const auto inButton = [&layout, x, cw](TitleBarButton button) {
        const std::optional<wlr_box>& box = layout.button(button);
        if (!box) {
          return false;
        }
        const int left = std::max(0, box->x - layout.gap / 2);
        const int right = std::min(cw, box->x + box->width + (layout.gap + 1) / 2);
        return x >= left && x < right;
      };
      if (inButton(TitleBarButton::Close)) {
        return {SsdFramePart::Close, 0};
      }
      if (inButton(TitleBarButton::Maximize)) {
        return {SsdFramePart::Maximize, 0};
      }
      if (inButton(TitleBarButton::Minimize)) {
        return {SsdFramePart::Minimize, 0};
      }
      return {SsdFramePart::TitleBar, 0};
    }
    if (!resizable
        || x < -b - kSsdResizeBand
        || x >= cw + b + kSsdResizeBand
        || y < -h - b - kSsdResizeBand
        || y >= ch + b + kSsdResizeBand) {
      return {};
    }
    uint32_t edges = 0;
    if (x < 0) {
      edges |= WLR_EDGE_LEFT;
    }
    if (x >= cw) {
      edges |= WLR_EDGE_RIGHT;
    }
    if (y < -h) {
      edges |= WLR_EDGE_TOP;
    }
    if (y >= ch) {
      edges |= WLR_EDGE_BOTTOM;
    }
    if (edges == 0) {
      return {};
    }
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      if (y < -h - b + kSsdResizeCorner) {
        edges |= WLR_EDGE_TOP;
      }
      if (y >= ch + b - kSsdResizeCorner) {
        edges |= WLR_EDGE_BOTTOM;
      }
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0) {
      if (x < -b + kSsdResizeCorner) {
        edges |= WLR_EDGE_LEFT;
      }
      if (x >= cw + b - kSsdResizeCorner) {
        edges |= WLR_EDGE_RIGHT;
      }
    }
    return {SsdFramePart::Resize, edges};
  }

  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight) {
    return {tile.x, tile.y + titleBarHeight, tile.width, std::max(1, tile.height - titleBarHeight)};
  }
} // namespace umbriel
