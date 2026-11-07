#pragma once

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace umbriel {

  struct FloatingPoint {
    int x = 0;
    int y = 0;
  };

  constexpr FloatingPoint keepTitleBarReachable(FloatingPoint origin, int titleBarHeight, const wlr_box& usable) {
    if (titleBarHeight > 0 && origin.y < usable.y + titleBarHeight) {
      origin.y = usable.y + titleBarHeight;
    }
    return origin;
  }

  constexpr int floatingCascadeStep(int titleBarHeight, int borderInset) {
    return std::max(32, titleBarHeight + borderInset + 5);
  }

  // Coordinates describe complete frames. Taken frames are ordered from front to back, so a hidden corner does not
  // displace a new window merely because it covers an already obscured window.
  inline FloatingPoint cascadeOrigin(
      FloatingPoint start, std::span<const wlr_box> taken, int step, const wlr_box& usable, int width, int height
  ) {
    const auto coveredFrame = [&](FloatingPoint candidate) -> const wlr_box* {
      for (size_t i = 0; i < taken.size(); ++i) {
        const wlr_box& other = taken[i];
        const bool sameOrigin = std::abs(candidate.x - other.x) <= 4 && std::abs(candidate.y - other.y) <= 4;
        const bool contains = other.width > 0
            && other.height > 0
            && candidate.x <= other.x
            && candidate.y <= other.y
            && candidate.x + width >= other.x + other.width
            && candidate.y + height >= other.y + other.height;
        const bool hidden = std::ranges::any_of(taken.first(i), [&](const wlr_box& above) {
          return other.x >= above.x
              && other.y >= above.y
              && other.x < above.x + above.width
              && other.y < above.y + above.height;
        });
        if (sameOrigin || (contains && !hidden)) {
          return &other;
        }
      }
      return nullptr;
    };
    if (step <= 0) {
      return start;
    }
    const int maxX = usable.x + std::max(0, usable.width - width);
    const int maxY = usable.y + std::max(0, usable.height - height);
    int wraps = 0;
    for (size_t attempt = 0; attempt <= taken.size(); ++attempt) {
      if (start.x < usable.x || start.y < usable.y || start.x > maxX || start.y > maxY) {
        // A new diagonal starts farther right each time. Check wrapped positions too, since a burst can fill them.
        const int columns = std::max(1, (maxX - usable.x) / step);
        start = {
            usable.x + std::min(maxX - usable.x, step * (1 + wraps % columns)),
            usable.y + std::min(maxY - usable.y, step)
        };
        ++wraps;
      }
      const wlr_box* other = coveredFrame(start);
      if (other == nullptr) {
        return start;
      }
      start = {.x = other->x + step, .y = other->y + step};
    }
    return {std::clamp(start.x, usable.x, maxX), std::clamp(start.y, usable.y, maxY)};
  }

  // How much of a floating window must stay on screen per axis. A float may hang
  // off an edge, but never so far that there is nothing left to grab it by.
  constexpr int floatingKeepVisible(int size) {
    const int quarter = size / 4;
    if (quarter < 10) {
      return 10;
    }
    return quarter > 75 ? 75 : quarter;
  }

  // Pull `origin` back until `floatingKeepVisible` of the window is inside
  // `usable` on both axes.
  constexpr FloatingPoint clampFloatingOrigin(FloatingPoint origin, const wlr_box& geometry, const wlr_box& usable) {
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

  // clampFloatingOrigin for a resize target, except that an axis the window fills
  // or overflows snaps to the usable edge rather than keeping its old offset.
  constexpr FloatingPoint
  clampFloatingOriginForResize(FloatingPoint origin, const wlr_box& geometry, const wlr_box& usable) {
    const FloatingPoint clamped = clampFloatingOrigin(origin, geometry, usable);
    return {
        .x = geometry.width >= usable.width ? usable.x : clamped.x,
        .y = geometry.height >= usable.height ? usable.y : clamped.y,
    };
  }

  // Where the window's content sits mid-resize. The edge being dragged moves and the opposite one stays put, which
  // matters because the client's geometry catches up asynchronously: without an anchor, a left- or top-edge drag would
  // visibly walk the far edge as each configure lands.
  constexpr FloatingPoint anchoredContentOrigin(const wlr_box& anchor, uint32_t edges, const wlr_box& geometry) {
    return {
        .x = (edges & WLR_EDGE_LEFT) != 0 ? anchor.x + anchor.width - geometry.width : anchor.x,
        .y = (edges & WLR_EDGE_TOP) != 0 ? anchor.y + anchor.height - geometry.height : anchor.y,
    };
  }

  // Has the client caught up with the configure we sent at `requested`? Serials are a free-running 32-bit counter that
  // wraps, so this is a signed difference rather than `committed >= requested`: a plain comparison inverts for the
  // whole second half of the counter's range after a wrap.
  constexpr bool serialSettled(uint32_t committed, uint32_t requested) {
    return static_cast<int32_t>(committed - requested) >= 0;
  }

  // Center a window of the given size in `usable`. Floats keep their own size,
  // so this only ever moves them.
  constexpr FloatingPoint centeredOrigin(const wlr_box& usable, int width, int height) {
    return {
        .x = usable.x + (usable.width - width) / 2,
        .y = usable.y + (usable.height - height) / 2,
    };
  }

  // Center a window of the given size over what shows of `box` inside `usable`, kept fully inside `usable` when it
  // fits. A box entirely out of view centers the window on `usable` instead.
  constexpr FloatingPoint centeredOverShown(const wlr_box& box, const wlr_box& usable, int width, int height) {
    const auto clamp = [](int value, int low, int high) { return value < low ? low : (value > high ? high : value); };
    const int left = box.x > usable.x ? box.x : usable.x;
    const int top = box.y > usable.y ? box.y : usable.y;
    const int right = box.x + box.width < usable.x + usable.width ? box.x + box.width : usable.x + usable.width;
    const int bottom = box.y + box.height < usable.y + usable.height ? box.y + box.height : usable.y + usable.height;
    const wlr_box shown{.x = left, .y = top, .width = right - left, .height = bottom - top};
    FloatingPoint origin = centeredOrigin(shown.width > 0 && shown.height > 0 ? shown : usable, width, height);
    origin.x = clamp(origin.x, usable.x, usable.x + (usable.width > width ? usable.width - width : 0));
    origin.y = clamp(origin.y, usable.y, usable.y + (usable.height > height ? usable.height - height : 0));
    return origin;
  }

  // One axis of an attached dialog's place, as an offset from the start of the window it is attached to: centred on
  // that window's `length`, then moved inside the area [`areaStart`, `areaStart + areaLength`) with `inset` left for
  // the dialog's border. A dialog longer than the area keeps its start inside it. A window resting wholly outside the
  // area, as a scrolled-away column does, takes its dialog along centred.
  constexpr int attachmentOffset(int start, int length, int size, int areaStart, int areaLength, int inset) {
    const int centred = (length - size) / 2;
    if (areaLength <= 0 || start + length <= areaStart || start >= areaStart + areaLength) {
      return centred;
    }
    const int low = areaStart + inset;
    const int high = areaStart + areaLength - inset - size;
    const int placed = start + centred;
    return (placed > high ? (high > low ? high : low) : (placed < low ? low : placed)) - start;
  }

  // Pixel length of `fraction` of the usable area on one axis. Rounds like
  // Layout::fractionalWidth so a float and a tiled lane at the same fraction do
  // not disagree by a pixel on an odd axis. A degenerate axis yields 0, which
  // callers pass through unclamped so xdg-shell keeps the client's own
  // preference for that axis.
  constexpr int floatingFractionSize(double fraction, int usable) {
    if (usable <= 0) {
      return 0;
    }
    const int pixels = static_cast<int>(std::lround(fraction * static_cast<double>(usable)));
    return pixels < 1 ? 1 : (pixels > usable ? usable : pixels);
  }

  // `size` as a fraction of a usable-axis length. The caller guarantees usable > 0.
  constexpr double floatingSizeFraction(int size, int usable) {
    return static_cast<double>(size) / static_cast<double>(usable);
  }

  // A float's cycle basis. `floatingFractionSize` rounds, so a size read back as a fraction can sit just under its
  // preset and stall `nextFractionPreset`. Snaps onto the preset whose pixel size matches; any other size keeps its
  // measured fraction. The caller guarantees extent > 0.
  inline double presetSnappedFraction(const std::vector<double>& presets, int size, int extent) {
    for (const double preset : presets) {
      if (floatingFractionSize(preset, extent) == size) {
        return preset;
      }
    }
    return floatingSizeFraction(size, extent);
  }

  // The remembered state of a window's floating identity, kept across tiled round trips so that floating a window twice
  // puts it back where it was. It also owns the configure-serial handoff. A floating client's size is compositor-owned
  // right up until it commits the serial we last requested; after that the client owns it and the compositor must stop
  // echoing configures back, or the two fight and the window shivers.
  class FloatingGeometry {
  public:
    // Remembered size and position
    void rememberSize(int width, int height) { m_size = {{width, height}}; }
    const std::optional<std::array<int, 2>>& size() const { return m_size; }
    // Position is stored as a fraction of the usable area so a float restored on
    // a different-sized output lands in the same relative spot.
    void rememberPositionFraction(FloatingPoint origin, const wlr_box& usable);
    std::optional<FloatingPoint> restoredOrigin(const wlr_box& usable) const;

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
    const std::optional<std::array<int, 2>>& pendingSize() const { return m_pendingSize; }
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
    const std::optional<wlr_box>& anchor() const { return m_anchor; }
    uint32_t edges() const { return m_edges; }

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
