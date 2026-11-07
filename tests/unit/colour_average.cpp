#include "check.h"
#include "scene/color.h"

#include <array>
#include <cstdint>

using umbriel::averageArgb;

namespace {
  constexpr uint32_t argb(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
    return (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
  }
} // namespace

UMBRIEL_TEST(averageArgbIsTheMeanOfAnOpaqueStrip) {
  // A green edge over a red body averages to the midpoint of the two colours it covers.
  const std::array<uint32_t, 4> strip{
      argb(255, 0, 0, 255), argb(255, 0, 0, 255), argb(0, 255, 0, 255), argb(0, 255, 0, 255)
  };
  const auto mean = averageArgb(strip.data(), strip.size());
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 0.5, 0.002);
  CHECK_NEAR((*mean)[1], 0.5, 0.002);
  CHECK_NEAR((*mean)[2], 0.0, 0.002);
  CHECK_NEAR((*mean)[3], 1.0, 0.002);
}

UMBRIEL_TEST(averageArgbIgnoresTransparentPixels) {
  // A transparent pixel must not pull the hue toward black; only its own alpha contributes.
  const std::array<uint32_t, 3> strip{argb(0, 0, 0, 0), argb(0, 255, 0, 255), argb(0, 255, 0, 255)};
  const auto mean = averageArgb(strip.data(), strip.size());
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 0.0, 0.002);
  CHECK_NEAR((*mean)[1], 1.0, 0.002);
  CHECK_NEAR((*mean)[3], 2.0 / 3.0, 0.002);
}

UMBRIEL_TEST(averageArgbHasNothingToShowForAFullyTransparentStrip) {
  const std::array<uint32_t, 2> strip{argb(255, 0, 0, 0), argb(0, 0, 255, 0)};
  CHECK(!averageArgb(strip.data(), strip.size()).has_value());
  CHECK(!averageArgb(nullptr, 4).has_value());
  CHECK(!averageArgb(strip.data(), 0).has_value());
}

int main() { return RUN_TESTS(); }
