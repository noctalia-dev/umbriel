#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace umbriel {

  struct OutputBox {
    int x;
    int y;
    int width;
    int height;
  };

  enum class OutputDirection {
    Left,
    Right,
    Up,
    Down,
  };

  // Select the nearest output in `direction` from `reference`. The point is in
  // layout coordinates and only affects which matching output is nearest.
  [[nodiscard]] std::optional<size_t> adjacentOutputIndex(
      std::span<const OutputBox> boxes, size_t reference, OutputDirection direction, double refX, double refY
  );

  // Select the output `step` places away from `reference` in layout order, wrapping at both ends. Layout order runs
  // left to right, then top to bottom, over output centers, so it depends on how the monitors are arranged rather
  // than on the order they were plugged in. With two outputs every non-zero step selects the other one.
  [[nodiscard]] std::optional<size_t> cyclicOutputIndex(std::span<const OutputBox> boxes, size_t reference, int step);

} // namespace umbriel
