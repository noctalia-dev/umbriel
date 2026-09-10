#include "input/swipe_tracker.h"

#include <cmath>
#include <ranges>

namespace umbriel {

  void SwipeTracker::push(double delta, uint32_t timeMsec) {
    // For the events we care about, timestamps should always increase monotonically.
    if (!m_history.empty()) {
      const uint32_t last = m_history.back().timeMsec;
      if (static_cast<int32_t>(timeMsec - last) < 0) {
        return;
      }
    }

    m_history.push_back({delta, timeMsec});
    m_pos += delta;

    trimHistory(timeMsec);
  }

  void SwipeTracker::reset() {
    m_history.clear();
    m_pos = 0;
  }

  double SwipeTracker::velocity() const {
    if (m_history.size() < 2) {
      return 0.0;
    }

    const auto totalTimeMsec =
        static_cast<double>(static_cast<int32_t>(m_history.back().timeMsec - m_history.front().timeMsec));
    if (totalTimeMsec == 0.0) {
      return 0.0;
    }

    // Each delta is the motion since the previous event, so the oldest one happened before the window starts and
    // would inflate the estimate: over two samples it doubles it.
    double totalDelta = 0.0;
    for (const Event& event : m_history | std::views::drop(1)) {
      totalDelta += event.delta;
    }
    // Seconds, matching a px/s velocity.
    return totalDelta / (totalTimeMsec / 1000.0);
  }

  double SwipeTracker::projectedEndPos() const {
    const double vel = velocity();
    return m_pos - vel / (1000.0 * std::log(kDecelerationTouchpad));
  }

  void SwipeTracker::trimHistory(uint32_t nowMsec) {
    while (!m_history.empty()) {
      const Event& first = m_history.front();
      if (static_cast<int32_t>(nowMsec - first.timeMsec) <= kHistoryLimitMsec) {
        break;
      }
      m_history.pop_front();
    }
  }

} // namespace umbriel
