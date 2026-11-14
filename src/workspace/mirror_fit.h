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

  // The mapping between a source output's layout coordinates and where a mirror presents them on its receiving output.
  // Rendering, input and fullscreen coverage all go through it, so they agree on every pixel.
  struct MirrorMapping {
    wlr_box source{};
    MirrorFit fit;

    // A source layout position on the receiver, rounded as the scene places nodes.
    int toReceiverX(int x) const { return fit.x + static_cast<int>(std::lround((x - source.x) * fit.scale)); }
    int toReceiverY(int y) const { return fit.y + static_cast<int>(std::lround((y - source.y) * fit.scale)); }
    int toReceiverLength(int length) const { return static_cast<int>(std::lround(length * fit.scale)); }
    wlr_box toReceiver(const wlr_box& box) const {
      return {toReceiverX(box.x), toReceiverY(box.y), toReceiverLength(box.width), toReceiverLength(box.height)};
    }
    // A receiving layout point back in the source's layout coordinates.
    double toSourceX(double x) const { return source.x + (x - fit.x) / fit.scale; }
    double toSourceY(double y) const { return source.y + (y - fit.y) / fit.scale; }
  };

  inline std::optional<MirrorMapping> mirrorMapping(const wlr_box& from, const wlr_box& to) {
    const std::optional<MirrorFit> fit = fitMirror(from, to);
    return fit ? std::optional(MirrorMapping{.source = from, .fit = *fit}) : std::nullopt;
  }
} // namespace umbriel
