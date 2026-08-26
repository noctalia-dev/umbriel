#pragma once

#include <cstdint>
#include <deque>

namespace umbriel {

  // Sliding-window swipe tracker. Accumulates deltas since the gesture start,
  // estimates velocity over recent history only, and projects where the movement would coast to a
  // stop under touchpad-style exponential deceleration.
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
