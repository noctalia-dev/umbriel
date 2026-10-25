#pragma once

#include <cstdint>
#include <deque>

namespace umbriel {

  // Three-finger travel, in the accelerated motion libinput reports for gestures, for one full step: switching one
  // workspace, and scrolling the strip by one viewport. The overview reuses both so a swipe covers the same distance
  // whether the previews are up or not.
  inline constexpr double kSwipeWorkspacePx = 300.0;
  inline constexpr double kSwipeViewportPx = 1200.0;

  // The finger travel one viewport pan or workspace step needs, after the input.gesture.travel_scale_* value for
  // its axis. Below 1.0 the same movement takes more finger travel, so a swipe covers less ground; 1.0 keeps the
  // upstream distances. Travel is divided rather than the deltas multiplied so every conversion sees one
  // convention.
  [[nodiscard]] constexpr double swipeTravelPx(double travelPx, double travelScale) { return travelPx / travelScale; }

  // Which axis's travel scale a movement along `vertical` reads. Shared by every swipe conversion (strip pan,
  // workspace step, overview roles) so their picks cannot drift apart or swap arguments silently.
  [[nodiscard]] constexpr double axisTravelScale(bool vertical, double horizontalScale, double verticalScale) {
    return vertical ? verticalScale : horizontalScale;
  }

  // Sliding-window swipe tracker. Accumulates deltas since the gesture start, estimates velocity over recent history
  // only, and projects where the movement would coast to a stop under exponential deceleration.
  class SwipeTracker {
  public:
    // Pushes a new reading into the tracker.
    void push(double delta, uint32_t timeMsec);
    void reset();

    [[nodiscard]] double pos() const { return m_pos; }
    [[nodiscard]] double velocity() const;
    [[nodiscard]] double projectedEndPos() const;

  private:
    struct Event {
      double delta = 0;
      uint32_t timeMsec = 0;
    };

    static constexpr int32_t kHistoryLimitMsec = 150;
    static constexpr double kDecelerationTouchpad = 0.997;

    void trimHistory(uint32_t nowMsec);

    std::deque<Event> m_history;
    double m_pos = 0;
  };

} // namespace umbriel
