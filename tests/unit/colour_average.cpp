#include "check.h"
#include "scene/color.h"

#include <array>
#include <cstdint>

using umbriel::averagePixels;
using umbriel::averagePixelsRect;
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
  // Premultiplied half-transparent red is (128, 0, 0, 128) and is still red; a second alpha weighting would halve it.
  const std::array<uint32_t, 2> strip{argb(128, 0, 0, 128), argb(128, 0, 0, 128)};
  const auto mean = averagePixels(strip.data(), strip.size(), PixelOrder::Argb);
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 1.0, 0.004);
  CHECK_NEAR((*mean)[3], 128.0 / 255.0, 0.004);
}

UMBRIEL_TEST(averagePixelsIgnoresTransparentPixels) {
  // A cleared (premultiplied zero) pixel adds no hue, only its alpha.
  const std::array<uint32_t, 3> strip{argb(0, 0, 0, 0), argb(0, 255, 0, 255), argb(0, 255, 0, 255)};
  const auto mean = averagePixels(strip.data(), strip.size(), PixelOrder::Argb);
  CHECK(mean.has_value());
  CHECK_NEAR((*mean)[0], 0.0, 0.002);
  CHECK_NEAR((*mean)[1], 1.0, 0.002);
  CHECK_NEAR((*mean)[3], 2.0 / 3.0, 0.002);
}

UMBRIEL_TEST(averagePixelsFollowsTheReadOrder) {
  // The renderer may hand back ABGR; the X variants carry no alpha.
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

UMBRIEL_TEST(averagePixelsRectAveragesEachRegionOnItsOwnRows) {
  // Two rows of four: the left half red, the right half green. A strided read must not run the rows together, and the
  // two regions are what a mirrored bar's title and button row are read as.
  const std::array<uint32_t, 8> pixels{
      argb(255, 0, 0, 255), argb(255, 0, 0, 255), argb(0, 255, 0, 255), argb(0, 255, 0, 255),
      argb(255, 0, 0, 255), argb(255, 0, 0, 255), argb(0, 255, 0, 255), argb(0, 255, 0, 255),
  };
  const auto left = averagePixelsRect(pixels.data(), 4, 2, 2, PixelOrder::Argb);
  CHECK(left.has_value());
  CHECK_NEAR((*left)[0], 1.0, 0.002);
  CHECK_NEAR((*left)[1], 0.0, 0.002);
  CHECK_NEAR((*left)[3], 1.0, 0.002);
  const auto right = averagePixelsRect(pixels.data() + 2, 4, 2, 2, PixelOrder::Argb);
  CHECK(right.has_value());
  CHECK_NEAR((*right)[0], 0.0, 0.002);
  CHECK_NEAR((*right)[1], 1.0, 0.002);
}

UMBRIEL_TEST(averagePixelsRectHasNothingToShowForAnEmptyRegion) {
  const std::array<uint32_t, 2> row{argb(255, 0, 0, 255), argb(0, 0, 0, 0)};
  CHECK(!averagePixelsRect(row.data(), 2, 0, 2, PixelOrder::Argb).has_value());
  CHECK(!averagePixelsRect(row.data(), 2, 2, 0, PixelOrder::Argb).has_value());
  // A stride narrower than the region would read the next row's pixels as this one's.
  CHECK(!averagePixelsRect(row.data(), 1, 2, 1, PixelOrder::Argb).has_value());
  const std::array<uint32_t, 1> clear{argb(0, 0, 0, 0)};
  CHECK(!averagePixelsRect(clear.data(), 1, 1, 1, PixelOrder::Argb).has_value());
}

int main() { return RUN_TESTS(); }
