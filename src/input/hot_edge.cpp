#include "input/hot_edge.h"

#include <cmath>

namespace umbriel {

  std::optional<HotEdgeSide> hotEdgeSide(double overX, double overY) {
    if (overX != 0 && std::abs(overX) >= std::abs(overY)) {
      return overX > 0 ? HotEdgeSide::Right : HotEdgeSide::Left;
    }
    if (overY != 0) {
      return overY > 0 ? HotEdgeSide::Bottom : HotEdgeSide::Top;
    }
    return std::nullopt;
  }

  bool hotEdgeHasNeighbor(std::span<const OutputBox> boxes, size_t reference, HotEdgeSide side, double x, double y) {
    OutputDirection direction = OutputDirection::Left;
    switch (side) {
    case HotEdgeSide::Left:
      direction = OutputDirection::Left;
      break;
    case HotEdgeSide::Right:
      direction = OutputDirection::Right;
      break;
    case HotEdgeSide::Top:
      direction = OutputDirection::Up;
      break;
    case HotEdgeSide::Bottom:
      direction = OutputDirection::Down;
      break;
    }
    return adjacentOutputIndex(boxes, reference, direction, x, y).has_value();
  }

  bool HotEdgeAccumulator::push(HotEdgeSide side, double amount, double resistance) {
    if (m_side != side) {
      reset();
      m_side = side;
    }
    m_total += amount;
    if (m_latched || m_total < resistance) {
      return false;
    }
    m_latched = true;
    return true;
  }

  void HotEdgeAccumulator::reset() {
    m_side.reset();
    m_total = 0;
    m_latched = false;
  }

} // namespace umbriel
