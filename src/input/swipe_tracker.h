#pragma once

#include <cstdint>
#include <deque>

namespace umbriel {

  // Three-finger travel, in the accelerated motion libinput reports for gestures, for one full step: switching one
  // workspace, and scrolling the strip by one viewport. The overview reuses both so a swipe covers the same distance
  // whether the previews are up or not.
  inline constexpr double kSwipeWorkspacePx = 300.0;
  inline constexpr double kSwipeViewportPx = 1200.0;

  // Coasting distance a released swipe still travels, as an S-curve over the release speed. The band just below the
  // midpoint is wide, so two and three pages stay distinguishable, and hard flicks saturate above three pages. A floor
  // that fades in with speed keeps an ordinary slow release flipping the nearest page. Speeds are in
  // libinput-accelerated units per second.
  inline constexpr double kCoastMaxPx = 3.0 * kSwipeViewportPx;
  inline constexpr double kCoastMidSpeed = 20700.0;
  inline constexpr double kCoastWidthSlow = 14000.0;
  inline constexpr double kCoastWidthFast = 9500.0;
  inline constexpr double kCoastFloorPx = 0.5 * kSwipeViewportPx;
  inline constexpr double kCoastFloorSpeed = 1500.0;

  // Sliding-window swipe tracker. Accumulates deltas since the gesture start, estimates velocity over recent history
  // only, and projects where the movement would coast to a stop under the speed-dependent gain.
  class SwipeTracker {
  public:
    // Pushes a new reading into the tracker.
    void push(double delta, uint32_t timeMsec);
    void reset();

    double pos() const { return m_pos; }
    double velocity() const;
    double projectedEndPos() const;

  private:
    struct Event {
      double delta = 0;
      uint32_t timeMsec = 0;
    };

    static constexpr int32_t kHistoryLimitMsec = 150;

    void trimHistory(uint32_t nowMsec);

    std::deque<Event> m_history;
    double m_pos = 0;
  };

} // namespace umbriel
