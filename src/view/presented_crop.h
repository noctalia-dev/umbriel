#pragma once

extern "C" {
#include <umbrielfx/types/fx/clipped_region.h>
#include <wlr/util/box.h>
}

namespace umbriel {
  // Offset centering a client buffer of `contentSize` in a fullscreen tile of `tileSize`. Negative when the buffer is
  // the larger of the two, which crops it equally on both sides; a fullscreen buffer is never scaled, because scaling a
  // client that is mid mode-change shows it at the wrong aspect ratio.
  [[nodiscard]] constexpr int fullscreenCenterOffset(int tileSize, int contentSize) {
    return contentSize > 0 ? (tileSize - contentSize) / 2 : 0;
  }

  // Map the visible part of an animated presentation back onto the committed buffer. `content` is the box the view is
  // drawn at and `clip` its visible part (surface coordinates); `base` is the surface's buffer source box (client
  // viewport and scale). A scene clip cannot do this: it crops 1:1 and caps at the committed surface size. Returns an
  // empty box when nothing is left, meaning the buffer should be left alone.
  [[nodiscard]] constexpr wlr_fbox croppedSourceBox(
      const wlr_fbox& base, const wlr_box& geometry, const wlr_box& content, const wlr_box& clip, int surfaceWidth,
      int surfaceHeight
  ) {
    if (content.width <= 0
        || content.height <= 0
        || geometry.width <= 0
        || geometry.height <= 0
        || surfaceWidth <= 0
        || surfaceHeight <= 0) {
      return {};
    }
    // Surface px per presented px.
    const double fx = static_cast<double>(geometry.width) / content.width;
    const double fy = static_cast<double>(geometry.height) / content.height;
    // Surface-local region backing the visible presented box.
    const double sx = geometry.x + (clip.x - geometry.x) * fx;
    const double sy = geometry.y + (clip.y - geometry.y) * fy;
    const double sw = clip.width * fx;
    const double sh = clip.height * fy;
    // Surface -> buffer coordinates (viewport/scale aware).
    const double bx = base.width / surfaceWidth;
    const double by = base.height / surfaceHeight;

    wlr_fbox src{base.x + sx * bx, base.y + sy * by, sw * bx, sh * by};
    if (src.x < base.x) {
      src.width -= base.x - src.x;
      src.x = base.x;
    }
    if (src.y < base.y) {
      src.height -= base.y - src.y;
      src.y = base.y;
    }
    const double maxWidth = base.x + base.width - src.x;
    const double maxHeight = base.y + base.height - src.y;
    src.width = src.width < maxWidth ? src.width : maxWidth;
    src.height = src.height < maxHeight ? src.height : maxHeight;
    if (src.width <= 0 || src.height <= 0) {
      return {};
    }
    return src;
  }
} // namespace umbriel
