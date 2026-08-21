#pragma once

namespace umbriel {

  [[nodiscard]] constexpr bool
  maximizeRequestTargetsEdges(bool edgesActive, bool columnFullWidth, bool requestedMaximized) {
    return edgesActive || (requestedMaximized && !columnFullWidth);
  }

} // namespace umbriel
