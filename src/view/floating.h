#pragma once

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

namespace umbriel {

  struct FloatingPoint {
    int x = 0;
    int y = 0;
  };

  // How much of a floating window must stay on screen per axis. A float may hang
  // off an edge, but never so far that there is nothing left to grab it by.
  [[nodiscard]] constexpr int floatingKeepVisible(int size) {
    const int quarter = size / 4;
    if (quarter < 10) {
      return 10;
    }
    return quarter > 75 ? 75 : quarter;
  }

  // Pull `origin` back until `floatingKeepVisible` of the window is inside
  // `usable` on both axes.
  [[nodiscard]] constexpr FloatingPoint
  clampFloatingOrigin(FloatingPoint origin, const wlr_box& geometry, const wlr_box& usable) {
    const auto clamp = [](int value, int low, int high) {
      if (low > high) {
        return low;
      }
      return value < low ? low : (value > high ? high : value);
    };
    const int visibleX = floatingKeepVisible(geometry.width);
    const int visibleY = floatingKeepVisible(geometry.height);
    return {
        .x = clamp(origin.x, usable.x + visibleX - geometry.width, usable.x + usable.width - visibleX),
        .y = clamp(origin.y, usable.y + visibleY - geometry.height, usable.y + usable.height - visibleY),
    };
  }

  // Where the window's content sits mid-resize. The edge being dragged moves and the opposite one stays put, which
  // matters because the client's geometry catches up asynchronously: without an anchor, a left- or top-edge drag would
  // visibly walk the far edge as each configure lands.
  [[nodiscard]] constexpr FloatingPoint
  anchoredContentOrigin(const wlr_box& anchor, uint32_t edges, const wlr_box& geometry) {
    return {
        .x = (edges & WLR_EDGE_LEFT) != 0 ? anchor.x + anchor.width - geometry.width : anchor.x,
        .y = (edges & WLR_EDGE_TOP) != 0 ? anchor.y + anchor.height - geometry.height : anchor.y,
    };
  }

  // Has the client caught up with the configure we sent at `requested`? Serials are a free-running 32-bit counter that
  // wraps, so this is a signed difference rather than `committed >= requested`: a plain comparison inverts for the
  // whole second half of the counter's range after a wrap.
  [[nodiscard]] constexpr bool serialSettled(uint32_t committed, uint32_t requested) {
    return static_cast<int32_t>(committed - requested) >= 0;
  }

  // Center a window of the given size in `usable`. Floats keep their own size,
  // so this only ever moves them.
  [[nodiscard]] constexpr FloatingPoint centeredOrigin(const wlr_box& usable, int width, int height) {
    return {
        .x = usable.x + (usable.width - width) / 2,
        .y = usable.y + (usable.height - height) / 2,
    };
  }

  // Pixel length of `fraction` of the usable area on one axis. Rounds like
  // Layout::fractionalWidth so a float and a tiled lane at the same fraction do
  // not disagree by a pixel on an odd axis. A degenerate axis yields 0, which
  // callers pass through unclamped so xdg-shell keeps the client's own
  // preference for that axis.
  [[nodiscard]] constexpr int floatingFractionSize(double fraction, int usable) {
    if (usable <= 0) {
      return 0;
    }
    const int pixels = static_cast<int>(std::lround(fraction * static_cast<double>(usable)));
    return pixels < 1 ? 1 : (pixels > usable ? usable : pixels);
  }

  // `size` as a fraction of a usable-axis length. The caller guarantees usable > 0.
  [[nodiscard]] constexpr double floatingSizeFraction(int size, int usable) {
    return static_cast<double>(size) / static_cast<double>(usable);
  }

  // The next preset fraction from `current` in `direction` (negative shrinks),
  // wrapping to the opposite end. Shared by scrolling columns and floats.
  [[nodiscard]] inline double nextFractionPreset(const std::vector<double>& presets, double current, int direction) {
    if (presets.empty()) {
      return current;
    }
    if (direction < 0) {
      for (const double preset : std::views::reverse(presets)) {
        if (preset < current - 0.0001) {
          return preset;
        }
      }
      return presets.back();
    }
    for (const double preset : presets) {
      if (preset > current + 0.0001) {
        return preset;
      }
    }
    return presets.front();
  }

  // The remembered state of a window's floating identity, kept across tiled round trips so that floating a window twice
  // puts it back where it was. It also owns the configure-serial handoff. A floating client's size is compositor-owned
  // right up until it commits the serial we last requested; after that the client owns it and the compositor must stop
  // echoing configures back, or the two fight and the window shivers.
  class FloatingGeometry {
  public:
    // Remembered size and position
    void rememberSize(int width, int height) { m_size = {{width, height}}; }
    [[nodiscard]] const std::optional<std::array<int, 2>>& size() const { return m_size; }
    // Position is stored as a fraction of the usable area so a float restored on
    // a different-sized output lands in the same relative spot.
    void rememberPositionFraction(FloatingPoint origin, const wlr_box& usable);
    [[nodiscard]] std::optional<FloatingPoint> restoredOrigin(const wlr_box& usable) const;

    // Configure-serial handoff. A fully-specified request is the pending basis
    // resize actions accumulate against until the client commits its serial.
    void recordSizeRequest(int width, int height, uint32_t serial) {
      if (width > 0 && height > 0) {
        m_pendingSize = {{width, height}};
      } else {
        m_pendingSize.reset();
      }
      m_sizeRequestSerial = serial;
    }
    [[nodiscard]] const std::optional<std::array<int, 2>>& pendingSize() const { return m_pendingSize; }
    void clearSizeRequest() {
      m_sizeRequestSerial.reset();
      m_pendingSize.reset();
    }
    // True once the client has committed the requested configure (or there was none outstanding). Retires the request
    // and, unless a resize is still in progress, the anchor with it.
    [[nodiscard]] bool retireSizeRequestIfSettled(uint32_t committedSerial);

    // Interactive resize
    void beginResize(const wlr_box& anchor, uint32_t edges);
    void endResize();
    [[nodiscard]] const std::optional<wlr_box>& anchor() const { return m_anchor; }
    [[nodiscard]] uint32_t edges() const { return m_edges; }

  private:
    void clearAnchor();

    std::optional<std::array<int, 2>> m_size;
    std::optional<std::array<double, 2>> m_posFrac;
    std::optional<uint32_t> m_sizeRequestSerial;
    std::optional<std::array<int, 2>> m_pendingSize;
    std::optional<wlr_box> m_anchor;
    uint32_t m_edges = 0;
    bool m_resizeActive = false;
  };

} // namespace umbriel
