#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace umbriel {

  enum class GammaOutputMode : uint8_t {
    Sdr,
    Hdr,
  };

  struct GammaTransitionOps {
    std::function<void()> clear;
    std::function<bool()> apply;
  };

  // HDR uses the encoded SDR white point, leaving LUT curvature to the SDR path.
  std::array<float, 3> hdrWhitePointFromGamma(const uint16_t* table, size_t rampSize);

  // HDR bypasses SDR gamma tables. Returning to SDR applies the latest table,
  // with an explicit clear as the safe fallback if preparing it fails.
  [[nodiscard]] bool stageGammaForOutputMode(GammaOutputMode mode, const GammaTransitionOps& ops);

} // namespace umbriel
