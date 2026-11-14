#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class WindowRegion : uint8_t { Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
  constexpr int kDropZoneEdge = 16;
  constexpr int kDropZoneCornerSpan = 100;
  constexpr int kDropZoneDwellMs = 150;

  // The one spelling of each region, indexed by `WindowRegion`; `window-set-region` and shell completion both read it.
  inline constexpr std::array<std::string_view, 8> kWindowRegionNames{"left",        "right",       "top",
                                                                      "bottom",      "top_left",    "top_right",
                                                                      "bottom_left", "bottom_right"};

  [[nodiscard]] std::string_view windowRegionName(WindowRegion region);
  [[nodiscard]] std::span<const std::string_view> windowRegionNames();
  [[nodiscard]] std::optional<WindowRegion> windowRegionNamed(std::string_view name);
  [[nodiscard]] wlr_box windowRegionBox(const wlr_box& area, WindowRegion region, int spacing);
  // Output coordinates and thresholds are logical. Shared edges are excluded until the drag dwell expires.
  std::optional<WindowRegion> dropZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges);
} // namespace umbriel
