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

  constexpr int kTitleBarButtonGap = 8;
  constexpr int kTitleBarTitlePadding = 8;
  constexpr int kSsdResizeBand = 6;
  constexpr int kSsdResizeCorner = 20;

  struct TitleBarLayout {
    // Indexed by TitleBarButton; empty for a button the bar does not show.
    std::array<std::optional<wlr_box>, kTitleBarButtonCount> buttons{};
    wlr_box title{};
    int gap = kTitleBarButtonGap;
    const std::optional<wlr_box>& button(TitleBarButton which) const { return buttons[static_cast<size_t>(which)]; }
  };
  struct SsdFramePoint {
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  // A negative `size` derives about half the bar height; otherwise `size` is capped at the height. Either way the
  // result takes the bar's parity so the margins above and below are whole and equal pixels.
  int titleBarButtonDiameter(int height, int size = -1);
  // `shown` lists the buttons from the title outward, so the last one sits at the bar's edge on `side`.
  TitleBarLayout titleBarLayout(
      int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown, int size = -1,
      int gap = kTitleBarButtonGap, int margin = -1
  );
  // Coordinates relative to client content; the title bar lies above y = 0.
  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      double x, double y, int size = -1, int gap = kTitleBarButtonGap, int margin = -1
  );
  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight);
} // namespace umbriel
