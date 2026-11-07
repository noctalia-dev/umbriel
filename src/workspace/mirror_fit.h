#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  // A source output's layout box fitted inside a receiving output's: one scale for both axes, so the whole source stays
  // visible, centered on the axis it leaves free. `x` and `y` are the layout coordinates where the source origin lands
  // on the receiver.
  struct MirrorFit {
    double scale = 1;
    int x = 0;
    int y = 0;
    bool operator==(const MirrorFit&) const = default;
  };

  inline std::optional<MirrorFit> fitMirror(const wlr_box& from, const wlr_box& to) {
    if (from.width <= 0 || from.height <= 0 || to.width <= 0 || to.height <= 0) {
      return std::nullopt;
    }
    const double scale =
        std::min(static_cast<double>(to.width) / from.width, static_cast<double>(to.height) / from.height);
    return MirrorFit{
        .scale = scale,
        .x = to.x + static_cast<int>(std::lround((to.width - from.width * scale) / 2)),
        .y = to.y + static_cast<int>(std::lround((to.height - from.height * scale) / 2)),
    };
  }
} // namespace umbriel
