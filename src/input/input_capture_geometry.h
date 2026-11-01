#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace umbriel {

  struct InputCaptureZone {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double physicalScale = 1.0;
    bool operator==(const InputCaptureZone&) const = default;
  };

  struct InputCaptureBarrier {
    uint32_t id = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
  };

  struct InputCaptureCrossing {
    uint32_t id = 0;
    double fraction = 0;
  };

  // Portal endpoints include the full last pixel. A barrier must fit on one
  // zone's exterior boundary and cannot overlap an interior shared edge.
  [[nodiscard]] bool
  validInputCaptureBarrier(std::span<const InputCaptureZone> zones, const InputCaptureBarrier& barrier);
  [[nodiscard]] std::optional<InputCaptureCrossing> inputCaptureCrossing(
      std::span<const InputCaptureZone> zones, std::span<const InputCaptureBarrier> barriers, double x, double y,
      double dx, double dy
  );

} // namespace umbriel
