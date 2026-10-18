#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace umbriel {

  enum class WindowPositionAnchor {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Top,
    Bottom,
    Left,
    Right,
    Center,
  };

  struct WindowPosition {
    int x = 0;
    int y = 0;
    WindowPositionAnchor anchor = WindowPositionAnchor::Center;
    bool operator==(const WindowPosition&) const = default;
  };

  struct WindowPositionAnchorName {
    std::string_view name;
    WindowPositionAnchor anchor;
  };

  // The one spelling of each anchor, shared by `default_position.anchor`, `window-move-to` and the cheatsheet.
  inline constexpr std::array<WindowPositionAnchorName, 9> kWindowPositionAnchorNames{{
      {.name = "top_left", .anchor = WindowPositionAnchor::TopLeft},
      {.name = "top_right", .anchor = WindowPositionAnchor::TopRight},
      {.name = "bottom_left", .anchor = WindowPositionAnchor::BottomLeft},
      {.name = "bottom_right", .anchor = WindowPositionAnchor::BottomRight},
      {.name = "top", .anchor = WindowPositionAnchor::Top},
      {.name = "bottom", .anchor = WindowPositionAnchor::Bottom},
      {.name = "left", .anchor = WindowPositionAnchor::Left},
      {.name = "right", .anchor = WindowPositionAnchor::Right},
      {.name = "center", .anchor = WindowPositionAnchor::Center},
  }};

  constexpr std::optional<WindowPositionAnchor> windowPositionAnchorFromName(std::string_view name) {
    const auto* found = std::ranges::find(kWindowPositionAnchorNames, name, &WindowPositionAnchorName::name);
    if (found == kWindowPositionAnchorNames.end()) {
      return std::nullopt;
    }
    return found->anchor;
  }

  constexpr std::string_view windowPositionAnchorName(WindowPositionAnchor anchor) {
    const auto* found = std::ranges::find(kWindowPositionAnchorNames, anchor, &WindowPositionAnchorName::anchor);
    return found == kWindowPositionAnchorNames.end() ? std::string_view{} : found->name;
  }
} // namespace umbriel
