#include "check.h"
#include "scene/color.h"

#include <array>
#include <cstdint>

using umbriel::averagePixels;
using umbriel::PixelOrder;

namespace {
  constexpr uint32_t argb(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
    return (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
  }
} // namespace

UMBRIEL_TEST(averagePixelsIsTheMeanOfAnOpaqueStrip) {
  // A green edge over a red body averages to the midpoint of the two colours it covers.
  const std::array<uint32_t, 4> strip{
      argb(255, 0, 0, 255), argb(255, 0, 0, 255), argb(0, 255, 0, 255), argb(0, 255, 0, 255)
  };
  const auto mean = averagePixels(strip.data(), strip.size(), PixelOrder::Argb);
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 0.5, 0.002);
  CHECK_NEAR((*mean)[1], 0.5, 0.002);
  CHECK_NEAR((*mean)[2], 0.0, 0.002);
  CHECK_NEAR((*mean)[3], 1.0, 0.002);
}

UMBRIEL_TEST(averagePixelsReadsPremultipliedPixelsOnce) {
  // A Wayland buffer stores premultiplied values: half-transparent red is (128, 0, 0, 128), and its colour is still
  // red. Weighting the components by alpha a second time would return half red.
  const std::array<uint32_t, 2> strip{argb(128, 0, 0, 128), argb(128, 0, 0, 128)};
  const auto mean = averagePixels(strip.data(), strip.size(), PixelOrder::Argb);
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 1.0, 0.004);
  CHECK_NEAR((*mean)[3], 128.0 / 255.0, 0.004);
}

UMBRIEL_TEST(averagePixelsIgnoresTransparentPixels) {
  // Premultiplied transparency is zero, so a cleared pixel adds no hue and only its own alpha to the total.
  const std::array<uint32_t, 3> strip{argb(0, 0, 0, 0), argb(0, 255, 0, 255), argb(0, 255, 0, 255)};
  const auto mean = averagePixels(strip.data(), strip.size(), PixelOrder::Argb);
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 0.0, 0.002);
  CHECK_NEAR((*mean)[1], 1.0, 0.002);
  CHECK_NEAR((*mean)[3], 2.0 / 3.0, 0.002);
}

UMBRIEL_TEST(averagePixelsFollowsTheReadOrder) {
  // The renderer may hand back ABGR, and the X variants carry no alpha at all.
  // In ABGR the red byte sits lowest, so opaque red packs as 0xFF0000FF.
  const std::array<uint32_t, 1> abgr{0xFF0000FFU};
  const auto abgrMean = averagePixels(abgr.data(), abgr.size(), PixelOrder::Abgr);
  CHECK(abgrMean.has_value());
  CHECK_NEAR((*abgrMean)[0], 1.0, 0.002);
  CHECK_NEAR((*abgrMean)[2], 0.0, 0.002);
  const std::array<uint32_t, 1> xrgb{argb(0, 0, 255, 0)};
  const auto opaque = averagePixels(xrgb.data(), xrgb.size(), PixelOrder::Xrgb);
  CHECK(opaque.has_value());
  CHECK_NEAR((*opaque)[2], 1.0, 0.002);
  CHECK_NEAR((*opaque)[3], 1.0, 0.002);
}

UMBRIEL_TEST(averagePixelsHasNothingToShowForAFullyTransparentStrip) {
  const std::array<uint32_t, 2> strip{argb(0, 0, 0, 0), argb(0, 0, 0, 0)};
  CHECK(!averagePixels(strip.data(), strip.size(), PixelOrder::Argb).has_value());
  CHECK(!averagePixels(nullptr, 4, PixelOrder::Argb).has_value());
  CHECK(!averagePixels(strip.data(), 0, PixelOrder::Argb).has_value());
}

int main() { return RUN_TESTS(); }
