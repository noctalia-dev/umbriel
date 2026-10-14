#pragma once

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
} // namespace umbriel
