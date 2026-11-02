#pragma once

#include "scene/ssd_frame_geometry.h"

#include <array>

struct wlr_buffer;

namespace umbriel {
  // Rasterizes geometric button strokes into a transparent square at the requested device resolution.
  [[nodiscard]] wlr_buffer* renderTitleBarIcon(TitleBarButton button, int pixels, const std::array<float, 4>& color);
} // namespace umbriel
