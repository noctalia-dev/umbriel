#pragma once

#include "input/swipe_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace umbriel {

  // Which touchpad stream drives an overview gesture. libinput reports two-finger scrolling as unaccelerated scroll
  // units and three-finger swipes as pointer-accelerated motion, so the same physical travel arrives as different
  // numbers and each stream carries its own distances.
  enum class NavigationSource : uint8_t { Scroll, Swipe };

  // Content-direction touchpad deltas, shared by finger axes and swipe events.
  // Keep axis selection separate from scene state so diagonal input and release
  // decisions can be tested without a compositor.
  class OverviewNavigation {
  public:
    enum class Axis : uint8_t { Pending, Horizontal, Vertical };

    // Travel that moves one workspace, and travel that pans the scrolling strip by one viewport. The swipe distances
    // are the ones three-finger gestures already use outside the overview, so the hand reads the same distance on
    // either side of it.
    struct Travel {
      double workspace = 0;
      double viewport = 0;
    };
    static constexpr Travel kScrollTravel{.workspace = 500.0, .viewport = 500.0};
    static constexpr Travel kSwipeTravel{.workspace = kSwipeWorkspacePx, .viewport = kSwipeViewportPx};

    [[nodiscard]] static constexpr Travel travelFor(NavigationSource source) {
      return source == NavigationSource::Scroll ? kScrollTravel : kSwipeTravel;
    }

    // Travel in either direction before the gesture commits to one axis.
    static constexpr double kAxisLock = 16.0;
    // How far the velocity at release is projected past the release position.
    static constexpr double kProjectionSec = 0.12;
    // Overscroll past either end, in workspaces for the filmstrip and in viewports for the strip.
    static constexpr double kOverscroll = 0.15;

    void reset() {
      m_x.reset();
      m_y.reset();
      m_axis = Axis::Pending;
    }

    void update(double dx, double dy, uint32_t timeMsec) {
      m_x.push(dx, timeMsec);
      m_y.push(dy, timeMsec);
      if (m_axis == Axis::Pending && std::hypot(m_x.pos(), m_y.pos()) >= kAxisLock) {
        m_axis = std::abs(m_x.pos()) > std::abs(m_y.pos()) ? Axis::Horizontal : Axis::Vertical;
      }
    }

    [[nodiscard]] Axis axis() const { return m_axis; }
    [[nodiscard]] double position() const { return tracker().pos(); }
    [[nodiscard]] double projectedPosition() const { return projectRelease(position(), velocity()); }
    [[nodiscard]] double velocity() const { return tracker().velocity(); }

    [[nodiscard]] static double projectRelease(double position, double velocity) {
      return position + velocity * kProjectionSec;
    }

    [[nodiscard]] static double travelScale(double extent, double zoom, double factor, double unitsPerStep) {
      return extent * zoomScale(zoom) * factor / unitsPerStep;
    }

    [[nodiscard]] static double rubberBandDerivative(double position, double maximum, double limit) {
      const double excess = position - std::clamp(position, 0.0, maximum);
      const double denominator = 1.0 + std::abs(excess) / limit;
      return 1.0 / (denominator * denominator);
    }

    // Reduce the effect of overview zoom on touchpad travel, rather than
    // multiplying sensitivity by the full inverse zoom.
    [[nodiscard]] static double zoomScale(double zoom) { return 1.0 / (1.0 + (zoom - 1.0) / 2.5); }

    [[nodiscard]] static double rubberBand(double position, double maximum, double limit) {
      const double clamped = std::clamp(position, 0.0, maximum);
      const double excess = position - clamped;
      return clamped + std::copysign(limit * (1.0 - 1.0 / (1.0 + std::abs(excess) / limit)), excess);
    }

    [[nodiscard]] static int workspaceTarget(double projected, int last) {
      return static_cast<int>(std::lround(std::clamp(projected, 0.0, static_cast<double>(last))));
    }

  private:
    [[nodiscard]] const SwipeTracker& tracker() const { return m_axis == Axis::Horizontal ? m_x : m_y; }
    SwipeTracker m_x;
    SwipeTracker m_y;
    Axis m_axis = Axis::Pending;
  };

} // namespace umbriel
