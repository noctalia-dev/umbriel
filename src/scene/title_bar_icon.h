#pragma once

#include "scene/ssd_frame_geometry.h"

#include <array>
#include <optional>

struct wlr_buffer;

namespace umbriel {
  // Rasterizes geometric button strokes into a transparent square at the requested device resolution. `outline`, when
  // set, strokes the same path wider underneath, so the symbol keeps an edge over an arbitrary title bar colour.
  [[nodiscard]] wlr_buffer* renderTitleBarIcon(
      TitleBarButton button, int pixels, const std::array<float, 4>& color,
      const std::optional<std::array<float, 4>>& outline = std::nullopt
  );
} // namespace umbriel
