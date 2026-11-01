#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class WindowRegion : uint8_t { Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
  constexpr int kDropZoneEdge = 16;
  constexpr int kDropZoneCornerSpan = 100;
  constexpr int kDropZoneDwellMs = 150;

  [[nodiscard]] std::string_view windowRegionName(WindowRegion region);
  [[nodiscard]] std::optional<WindowRegion> windowRegionNamed(std::string_view name);
  [[nodiscard]] wlr_box windowRegionBox(const wlr_box& area, WindowRegion region, int spacing);
  // Output coordinates and thresholds are logical. Shared edges are excluded until the drag dwell expires.
  [[nodiscard]] std::optional<WindowRegion> dropZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges);
} // namespace umbriel
