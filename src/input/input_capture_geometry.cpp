#include "input/input_capture_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace umbriel {

  namespace {
    struct Edge {
      bool horizontal = false;
      int direction = 0;
      int64_t coordinate = 0;
      int64_t first = 0;
      int64_t last = 0;
    };

    std::optional<Edge> exteriorEdge(
        std::span<const InputCaptureZone> zones, const InputCaptureBarrier& barrier, bool verticalOnly = false
    ) {
      if (barrier.id == 0 || (barrier.x1 != barrier.x2 && barrier.y1 != barrier.y2)) {
        return std::nullopt;
      }
      for (const InputCaptureZone& zone : zones) {
        if (zone.width == 0 || zone.height == 0) {
          continue;
        }
        const int64_t right = int64_t{zone.x} + zone.width;
        const int64_t bottom = int64_t{zone.y} + zone.height;
        std::optional<Edge> edge;
        if (!verticalOnly
            && barrier.y1 == barrier.y2
            && std::min(barrier.x1, barrier.x2) >= zone.x
            && std::max(barrier.x1, barrier.x2) < right
            && (barrier.y1 == zone.y || barrier.y1 == bottom)) {
          edge = Edge{
              .horizontal = true,
              .direction = barrier.y1 == zone.y ? -1 : 1,
              .coordinate = barrier.y1,
              .first = std::min(barrier.x1, barrier.x2),
              .last = int64_t{std::max(barrier.x1, barrier.x2)} + 1,
          };
        } else if (
            barrier.x1 == barrier.x2
            && std::min(barrier.y1, barrier.y2) >= zone.y
            && std::max(barrier.y1, barrier.y2) < bottom
            && (barrier.x1 == zone.x || barrier.x1 == right)
        ) {
          edge = Edge{
              .horizontal = false,
              .direction = barrier.x1 == zone.x ? -1 : 1,
              .coordinate = barrier.x1,
              .first = std::min(barrier.y1, barrier.y2),
              .last = int64_t{std::max(barrier.y1, barrier.y2)} + 1,
          };
        }
        if (!edge) {
          continue;
        }
        const bool covered = std::ranges::any_of(zones, [&edge](const InputCaptureZone& other) {
          if (other.width == 0 || other.height == 0) {
            return false;
          }
          const int64_t first = edge->horizontal ? other.x : other.y;
          const int64_t last = first + (edge->horizontal ? other.width : other.height);
          if (first >= edge->last || last <= edge->first) {
            return false;
          }
          const int64_t near = edge->horizontal ? other.y : other.x;
          const int64_t far = near + (edge->horizontal ? other.height : other.width);
          return edge->direction < 0 ? near < edge->coordinate && far >= edge->coordinate
                                     : near <= edge->coordinate && far > edge->coordinate;
        });
        if (!covered) {
          return edge;
        }
      }
      return std::nullopt;
    }
  } // namespace

  bool validInputCaptureBarrier(std::span<const InputCaptureZone> zones, const InputCaptureBarrier& barrier) {
    return exteriorEdge(zones, barrier).has_value()
        || (barrier.x1 == barrier.x2 && barrier.y1 == barrier.y2 && exteriorEdge(zones, barrier, true).has_value());
  }

  std::optional<InputCaptureCrossing> inputCaptureCrossing(
      std::span<const InputCaptureZone> zones, std::span<const InputCaptureBarrier> barriers, double x, double y,
      double dx, double dy
  ) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dx) || !std::isfinite(dy)) {
      return std::nullopt;
    }
    if (std::ranges::none_of(zones, [x, y](const auto& zone) {
          return x >= zone.x
              && y >= zone.y
              && x < static_cast<double>(zone.x) + zone.width
              && y < static_cast<double>(zone.y) + zone.height;
        })) {
      return std::nullopt;
    }
    std::optional<InputCaptureCrossing> nearest;
    for (const InputCaptureBarrier& barrier : barriers) {
      const bool point = barrier.x1 == barrier.x2 && barrier.y1 == barrier.y2;
      const std::array edges{
          exteriorEdge(zones, barrier),
          point ? exteriorEdge(zones, barrier, true) : std::optional<Edge>{},
      };
      for (const auto& edge : edges) {
        if (!edge) {
          continue;
        }
        const double motion = edge->horizontal ? dy : dx;
        const double origin = edge->horizontal ? y : x;
        if (motion * edge->direction <= 0) {
          continue;
        }
        const double target = origin + motion;
        if (edge->direction < 0 ? target >= edge->coordinate : target < edge->coordinate) {
          continue;
        }
        const double fraction = (edge->coordinate - origin) / motion;
        if (fraction < 0 || fraction > 1) {
          continue;
        }
        const double across = edge->horizontal ? x + fraction * dx : y + fraction * dy;
        if (across < edge->first || across >= edge->last) {
          continue;
        }
        if (!nearest || fraction < nearest->fraction) {
          nearest = InputCaptureCrossing{.id = barrier.id, .fraction = fraction};
        }
      }
    }
    return nearest;
  }

} // namespace umbriel
