#pragma once

#include "output/direction.h"

#include <cstddef>
#include <optional>
#include <span>

namespace umbriel {

  enum class HotEdgeSide : size_t {
    Left,
    Right,
    Top,
    Bottom,
  };

  // The edge a pointer push points at, from the over-travel (requested minus clamped position). The horizontal axis
  // wins ties. No over-travel selects nothing.
  [[nodiscard]] std::optional<HotEdgeSide> hotEdgeSide(double overX, double overY);

  // Whether another output lies beyond `side` of `reference`, which makes the edge a boundary between outputs.
  [[nodiscard]] bool
  hotEdgeHasNeighbor(std::span<const OutputBox> boxes, size_t reference, HotEdgeSide side, double x, double y);

  // Counts over-travel along one edge and reports once per excursion when it reaches the resistance.
  class HotEdgeAccumulator {
  public:
    [[nodiscard]] bool push(HotEdgeSide side, double amount, double resistance);
    void reset();

  private:
    std::optional<HotEdgeSide> m_side;
    double m_total = 0;
    bool m_latched = false;
  };

} // namespace umbriel
