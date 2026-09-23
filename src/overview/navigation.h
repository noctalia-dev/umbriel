#pragma once

#include "input/gesture_physics.h"
#include "input/swipe_tracker.h"

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

    void reset() {
      m_x.reset();
      m_y.reset();
      m_axis = Axis::Pending;
    }

    void update(double dx, double dy, uint32_t timeMsec) {
      m_x.push(dx, timeMsec);
      m_y.push(dy, timeMsec);
      if (m_axis == Axis::Pending && std::hypot(m_x.pos(), m_y.pos()) >= GesturePhysics::kAxisLock) {
        m_axis = std::abs(m_x.pos()) > std::abs(m_y.pos()) ? Axis::Horizontal : Axis::Vertical;
      }
    }

    [[nodiscard]] Axis axis() const { return m_axis; }
    [[nodiscard]] double position() const { return tracker().pos(); }
    // Where the travel would coast to a stop under the deceleration in SwipeTracker: the projection the release
    // settles on, and the one the snapshot in the other direction uses.
    [[nodiscard]] double projectedPosition() const { return tracker().projectedEndPos(); }
    [[nodiscard]] double velocity() const { return tracker().velocity(); }

    [[nodiscard]] static double travelScale(double extent, double zoom, double factor, double unitsPerStep) {
      return extent * GesturePhysics::zoomScale(zoom) * factor / unitsPerStep;
    }

  private:
    [[nodiscard]] const SwipeTracker& tracker() const { return m_axis == Axis::Horizontal ? m_x : m_y; }
    SwipeTracker m_x;
    SwipeTracker m_y;
    Axis m_axis = Axis::Pending;
  };

} // namespace umbriel
