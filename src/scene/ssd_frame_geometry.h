#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class TitleBarButtonSide : uint8_t { Left, Right };
  enum class TitleBarButton : uint8_t { Minimize, Maximize, Close };
  inline constexpr size_t kTitleBarButtonCount = 3;
  enum class SsdFramePart : uint8_t { None, TitleBar, Close, Maximize, Minimize, Resize };

  constexpr int kTitleBarTitlePadding = 8;
  constexpr int kSsdResizeBand = 6;
  constexpr int kSsdResizeCorner = 20;

  struct TitleBarLayout {
    // Indexed by TitleBarButton; empty for a button the bar does not show.
    std::array<std::optional<wlr_box>, kTitleBarButtonCount> buttons{};
    wlr_box title{};
    // The gap between neighbouring buttons after a narrow bar shrank it; the hit test splits it between them.
    int gap = 0;
    const std::optional<wlr_box>& button(TitleBarButton which) const { return buttons[static_cast<size_t>(which)]; }
  };
  struct SsdFramePoint {
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  // Configured button row geometry in logical pixels. A negative `size` derives from the bar height, and a negative
  // `padding` matches the vertical inset.
  struct TitleBarButtonGeometry {
    int size;
    int gap;
    int padding;
  };

  // A negative `size` derives about half the bar height, with the bar's parity so the margins above and below are whole
  // and equal pixels. An explicit `size` is used as is, capped at the height.
  int titleBarButtonDiameter(int height, int size);
  // `shown` lists the buttons from the title outward. The frame's inner radius sets a minimum inset for full
  // visibility.
  TitleBarLayout titleBarLayout(
      int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      const TitleBarButtonGeometry& geometry, int radius = 0, int titleHeight = 0
  );
  // Coordinates relative to client content; the title bar lies above y = 0.
  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      const TitleBarButtonGeometry& geometry, double x, double y, int radius = 0
  );
  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight);
  // How many logical pixels of the window's top edge an adaptive title bar can take: one flat colour row up to a few
  // rows of the window's own detail.
  inline constexpr int kTitleBarStripHeightMin = 1;
  inline constexpr int kTitleBarStripHeightMax = 4;
  // The slice of a surface's buffer that appears as the top `logicalHeight` pixels of the window content.
  // `source` is the visible part of the buffer in buffer coordinates, `bufferWidth`/`bufferHeight` its full size,
  // `surfaceWidth`/`surfaceHeight` the surface's own size in surface-local pixels, and `transform` its buffer
  // transform. A box with no area means the caller has no usable buffer.
  wlr_fbox titleBarStripSourceBox(
      const wlr_fbox& source, int bufferWidth, int bufferHeight, int surfaceWidth, int surfaceHeight,
      uint32_t transform, int logicalHeight
  );
} // namespace umbriel
