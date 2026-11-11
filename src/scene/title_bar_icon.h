#pragma once

#include "scene/ssd_frame_geometry.h"

#include <array>

struct wlr_buffer;

namespace umbriel {
  // A symbol's side and stroke width, each a fraction of the button it is drawn on.
  struct TitleBarGlyph {
    double size = 0.5;
    double stroke = 1.0 / 12.0;
    bool operator==(const TitleBarGlyph&) const = default;
  };

  // The glyph for a `button` logical pixels wide; a negative setting derives that dimension from the button.
  TitleBarGlyph titleBarGlyph(int button, int size, double stroke);

  // Rasterizes geometric button strokes into a transparent square at the requested device resolution.
  [[nodiscard]] wlr_buffer*
  renderTitleBarIcon(TitleBarButton button, int pixels, const std::array<float, 4>& color, const TitleBarGlyph& glyph);
} // namespace umbriel
