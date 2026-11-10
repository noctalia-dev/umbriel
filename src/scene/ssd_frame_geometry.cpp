#include "scene/ssd_frame_geometry.h"

#include <algorithm>
#include <cmath>
#include <ranges>

extern "C" {
#include <wlr/util/edges.h>
#include <wlr/util/transform.h>
}

namespace umbriel {
  namespace {
    int cornerInsetAt(int radius, int y) {
      if (radius <= y) {
        return 0;
      }
      return static_cast<int>(std::ceil(radius - std::sqrt(static_cast<double>(y) * (2 * radius - y)))) + 1;
    }
  } // namespace

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
      const TitleBarButtonGeometry& geometry, int radius, int titleHeight
  ) {
    const int diameter = titleBarButtonDiameter(height, geometry.size);
    const int verticalInset = (height - diameter) / 2;
    // Keep the complete button box inside the top arc, including its antialiased edge.
    const int cornerInset = cornerInsetAt(radius, verticalInset);
    int padding = geometry.padding < 0 ? verticalInset : geometry.padding;
    padding = std::max(padding, cornerInset);
    int gap = geometry.gap;
    const int count = static_cast<int>(shown.size());
    // Shrink the padding, then the gap, to fit a narrow bar. Buttons keep their size; the innermost ones that still do
    // not fit get no slot, so they are neither drawn nor clickable.
    if (count > 0) {
      padding = std::max(cornerInset, std::min(padding, width - cornerInset - count * diameter - (count - 1) * gap));
      if (count > 1) {
        gap = std::max(0, std::min(gap, (width - cornerInset - count * diameter - padding) / (count - 1)));
      }
    }
    const int titlePadding =
        std::max(kTitleBarTitlePadding, cornerInsetAt(radius, std::max(0, (height - titleHeight) / 2)));
    TitleBarLayout result{.title = {titlePadding, 0, 0, height}, .gap = gap};
    // Walk from the edge inward: the last listed button is outermost.
    int x = side == TitleBarButtonSide::Right ? width - padding - diameter : padding;
    const int step = side == TitleBarButtonSide::Right ? -(diameter + gap) : diameter + gap;
    std::optional<int> innermost;
    for (const TitleBarButton button : std::views::reverse(shown)) {
      auto& slot = result.buttons[static_cast<size_t>(button)];
      if (slot) {
        continue;
      }
      if (x < cornerInset || x + diameter > width - cornerInset) {
        break;
      }
      slot = wlr_box{x, verticalInset, diameter, diameter};
      innermost = x;
      x += step;
    }
    if (side == TitleBarButtonSide::Right) {
      const int right = std::min(width - titlePadding, innermost.value_or(width) - kTitleBarTitlePadding);
      result.title.width = std::max(0, right - result.title.x);
    } else {
      if (innermost) {
        result.title.x = std::max(titlePadding, *innermost + diameter + kTitleBarTitlePadding);
      }
      result.title.width = std::max(0, width - result.title.x - titlePadding);
    }
    return result;
  }

  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      const TitleBarButtonGeometry& geometry, double x, double y, int radius
  ) {
    if (cw <= 0 || ch <= 0 || h < 0) {
      return {};
    }
    if (x >= 0 && x < cw && y >= -h && y < 0) {
      const TitleBarLayout layout = titleBarLayout(cw, h, side, shown, geometry, radius);
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

  wlr_fbox titleBarStripSourceBox(
      const wlr_fbox& source, int bufferWidth, int bufferHeight, const wlr_box& geometry, int surfaceWidth,
      int surfaceHeight, uint32_t transform, int logicalHeight, double widthFraction
  ) {
    // The whole transform, flip bit included: a flip moves which buffer pixels the geometry maps to, while only a
    // quarter turn swaps the buffer's dimensions.
    const auto full = static_cast<wl_output_transform>(transform);
    const auto rotation = static_cast<wl_output_transform>(transform & 3U);
    const bool quarterTurn = rotation == WL_OUTPUT_TRANSFORM_90 || rotation == WL_OUTPUT_TRANSFORM_270;
    // The whole buffer as it shows upright on screen: a quarter turn swaps its dimensions.
    const double uprightWidth = quarterTurn ? bufferHeight : bufferWidth;
    const double uprightHeight = quarterTurn ? bufferWidth : bufferHeight;
    if (source.width <= 0.0
        || source.height <= 0.0
        || uprightWidth <= 0.0
        || uprightHeight <= 0.0
        || surfaceWidth <= 0
        || surfaceHeight <= 0) {
      return {};
    }
    // Upright first: the window geometry is in surface coordinates, and a quarter turn puts surface x on buffer y, so
    // cropping the buffer box before the turn would take the offset off the wrong axis.
    wlr_fbox visible = source;
    if (full != WL_OUTPUT_TRANSFORM_NORMAL) {
      wlr_fbox_transform(&visible, &source, full, bufferWidth, bufferHeight);
    }
    // The surface's coordinates stretch over exactly `visible`, so the geometry scales into it. A window geometry that
    // is empty, or that misses the visible part, leaves the whole surface visible.
    const double scaleX = visible.width / static_cast<double>(surfaceWidth);
    const double scaleY = visible.height / static_cast<double>(surfaceHeight);
    if (geometry.width > 0 && geometry.height > 0) {
      const double left = std::max(visible.x, visible.x + geometry.x * scaleX);
      const double top = std::max(visible.y, visible.y + geometry.y * scaleY);
      const double right = std::min(visible.x + visible.width, visible.x + (geometry.x + geometry.width) * scaleX);
      const double bottom = std::min(visible.y + visible.height, visible.y + (geometry.y + geometry.height) * scaleY);
      if (right > left && bottom > top) {
        visible = {left, top, right - left, bottom - top};
      }
    }
    // Only the leading part of the edge when the caller samples rather than mirrors it: the title is much narrower
    // than the bar, and each readback pixel costs a finished frame.
    if (widthFraction < 1.0) {
      visible.width = std::max(0.0, visible.width * std::clamp(widthFraction, 0.0, 1.0));
    }
    // Cap after the floor: a viewport can make the source shorter than a buffer pixel, which inverts clamp()'s bounds.
    const double thickness = std::min(visible.height, std::max(1.0, static_cast<double>(logicalHeight) * scaleY));
    const wlr_fbox strip{visible.x, visible.y, visible.width, thickness};
    wlr_fbox inBuffer = strip;
    if (full != WL_OUTPUT_TRANSFORM_NORMAL) {
      wlr_fbox_transform(&inBuffer, &strip, wlr_output_transform_invert(full), uprightWidth, uprightHeight);
    }
    // Keep the box inside the buffer: the scene refuses a negative origin, and overshoot samples outside it.
    const double right = std::min(inBuffer.x + inBuffer.width, static_cast<double>(bufferWidth));
    const double bottom = std::min(inBuffer.y + inBuffer.height, static_cast<double>(bufferHeight));
    inBuffer.x = std::clamp(inBuffer.x, 0.0, static_cast<double>(bufferWidth));
    inBuffer.y = std::clamp(inBuffer.y, 0.0, static_cast<double>(bufferHeight));
    inBuffer.width = std::max(0.0, right - inBuffer.x);
    inBuffer.height = std::max(0.0, bottom - inBuffer.y);
    return inBuffer;
  }
} // namespace umbriel
