#include "output/direction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace umbriel {

  std::optional<size_t> adjacentOutputIndex(
      std::span<const OutputBox> boxes, size_t reference, OutputDirection direction, double refX, double refY
  ) {
    if (reference >= boxes.size()) {
      return std::nullopt;
    }

    const OutputBox& referenceBox = boxes[reference];
    const int64_t referenceCenterX = static_cast<int64_t>(referenceBox.x) * 2 + referenceBox.width;
    const int64_t referenceCenterY = static_cast<int64_t>(referenceBox.y) * 2 + referenceBox.height;
    double nearestDistance = std::numeric_limits<double>::infinity();
    std::optional<size_t> nearest;

    for (size_t index = 0; index < boxes.size(); ++index) {
      if (index == reference) {
        continue;
      }
      const OutputBox& candidate = boxes[index];
      const int64_t candidateCenterX = static_cast<int64_t>(candidate.x) * 2 + candidate.width;
      const int64_t candidateCenterY = static_cast<int64_t>(candidate.y) * 2 + candidate.height;
      const int64_t deltaX = candidateCenterX - referenceCenterX;
      const int64_t deltaY = candidateCenterY - referenceCenterY;
      const bool matches = (direction == OutputDirection::Left && deltaX < 0 && -deltaX >= std::abs(deltaY))
          || (direction == OutputDirection::Right && deltaX > 0 && deltaX >= std::abs(deltaY))
          || (direction == OutputDirection::Up && deltaY < 0 && -deltaY >= std::abs(deltaX))
          || (direction == OutputDirection::Down && deltaY > 0 && deltaY >= std::abs(deltaX));
      if (!matches) {
        continue;
      }

      const double closestX =
          std::clamp(refX, static_cast<double>(candidate.x), static_cast<double>(candidate.x + candidate.width));
      const double closestY =
          std::clamp(refY, static_cast<double>(candidate.y), static_cast<double>(candidate.y + candidate.height));
      const double distance = std::hypot(closestX - refX, closestY - refY);
      if (distance < nearestDistance) {
        nearestDistance = distance;
        nearest = index;
      }
    }
    return nearest;
  }

  std::optional<size_t> cyclicOutputIndex(std::span<const OutputBox> boxes, size_t reference, int step) {
    if (reference >= boxes.size() || boxes.size() < 2 || step == 0) {
      return std::nullopt;
    }

    // Centers, doubled to stay integral. Equal centers fall back to the input index, so the order is total even for
    // mirrored outputs.
    std::vector<size_t> order(boxes.size());
    for (size_t index = 0; index < order.size(); ++index) {
      order[index] = index;
    }
    std::ranges::sort(order, [boxes](size_t left, size_t right) {
      const int64_t leftX = static_cast<int64_t>(boxes[left].x) * 2 + boxes[left].width;
      const int64_t rightX = static_cast<int64_t>(boxes[right].x) * 2 + boxes[right].width;
      if (leftX != rightX) {
        return leftX < rightX;
      }
      const int64_t leftY = static_cast<int64_t>(boxes[left].y) * 2 + boxes[left].height;
      const int64_t rightY = static_cast<int64_t>(boxes[right].y) * 2 + boxes[right].height;
      if (leftY != rightY) {
        return leftY < rightY;
      }
      return left < right;
    });

    const auto position = std::ranges::find(order, reference);
    if (position == order.end()) {
      return std::nullopt;
    }
    const auto count = static_cast<int64_t>(order.size());
    const int64_t from = std::distance(order.begin(), position);
    const int64_t wrapped = ((from + step) % count + count) % count;
    return order[static_cast<size_t>(wrapped)];
  }

} // namespace umbriel
