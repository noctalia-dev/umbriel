#pragma once

#include "input/swipe_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace umbriel {

  // Content-direction touchpad deltas, shared by finger axes and swipe events.
  // Keep axis selection separate from scene state so diagonal input and release
  // decisions can be tested without a compositor.
  class OverviewNavigation {
  public:
    enum class Axis : uint8_t { Pending, Horizontal, Vertical };

    void reset() {
      m_x.reset();
      m_y.reset();
      m_axis = Axis::Pending;
    }

    void update(double dx, double dy, uint32_t timeMsec) {
      m_x.push(dx, timeMsec);
      m_y.push(dy, timeMsec);
      if (m_axis == Axis::Pending && std::hypot(m_x.pos(), m_y.pos()) >= 16.0) {
        m_axis = std::abs(m_x.pos()) > std::abs(m_y.pos()) ? Axis::Horizontal : Axis::Vertical;
      }
    }

    [[nodiscard]] Axis axis() const { return m_axis; }
    [[nodiscard]] double position() const { return tracker().pos(); }
    [[nodiscard]] double projectedPosition() const { return tracker().projectedEndPos(); }
    [[nodiscard]] double velocity() const { return tracker().velocity(); }

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
