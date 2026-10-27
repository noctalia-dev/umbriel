// The one preset-cycling rule, shared by every layout's width and height cycle
// and by the floating cycle. Layout fractions are stored and arrive exact;
// a float's basis is recovered from pixels and has to be snapped first.
#include "check.h"
#include "layout/layout.h"
#include "view/floating.h"

using umbriel::boundedPresets;
using umbriel::floatingFractionSize;
using umbriel::floatingSizeFraction;
using umbriel::nextFractionPreset;
using umbriel::presetSnappedFraction;

namespace {
  const std::vector<double> kPresets{1.0 / 3, 0.5, 2.0 / 3};

  // What the floating cycle does: snap the measured size onto the preset that
  // produced it, step, then resolve back to pixels.
  int cycledPixels(int size, int extent, int direction) {
    const double current = presetSnappedFraction(kPresets, size, extent);
    return floatingFractionSize(nextFractionPreset(kPresets, current, direction), extent);
  }
} // namespace

// The rule, on exact fractions
UMBRIEL_TEST(cyclingForwardPicksTheNextLargerPreset) {
  CHECK(nextFractionPreset(kPresets, 0.4, 1) == 0.5);
  CHECK(nextFractionPreset(kPresets, 0.5, 1) == 2.0 / 3);
}

UMBRIEL_TEST(cyclingForwardWrapsToTheSmallestPreset) { CHECK(nextFractionPreset(kPresets, 0.9, 1) == 1.0 / 3); }

UMBRIEL_TEST(cyclingBackwardPicksTheNextSmallerPreset) {
  CHECK(nextFractionPreset(kPresets, 0.6, -1) == 0.5);
  CHECK(nextFractionPreset(kPresets, 0.4, -1) == 1.0 / 3);
}

UMBRIEL_TEST(cyclingBackwardWrapsToTheLargestPreset) { CHECK(nextFractionPreset(kPresets, 0.2, -1) == 2.0 / 3); }

UMBRIEL_TEST(anExactPresetCyclesPastItself) {
  // The epsilon guard: a window sitting exactly on a preset moves to the next
  // one instead of re-selecting its own size.
  CHECK(nextFractionPreset(kPresets, 0.5, -1) == 1.0 / 3);
  CHECK(nextFractionPreset(kPresets, 0.5, 1) == 2.0 / 3);
}

UMBRIEL_TEST(cyclingWithoutPresetsKeepsTheCurrentFraction) {
  CHECK(nextFractionPreset({}, 0.42, 1) == 0.42);
  CHECK(nextFractionPreset({}, 0.42, -1) == 0.42);
}

// The float basis: pixels, snapped back onto the preset that produced them
UMBRIEL_TEST(aFloatSizeSnapsOntoThePresetThatProducedIt) {
  // A third of 700 is 233, which reads back as 0.3329: below 1/3, so an
  // unsnapped basis makes the rule hand back 1/3, the preset the window already
  // sits on, and the cycle stalls on the smallest size for good.
  CHECK_EQ(floatingFractionSize(1.0 / 3, 700), 233);
  CHECK(presetSnappedFraction(kPresets, 233, 700) == 1.0 / 3);
  CHECK_EQ(cycledPixels(233, 700, 1), 350);
  CHECK_EQ(cycledPixels(467, 700, -1), 350);
  // 1280 rounds the other way (427 reads back as 0.3336), which strands the
  // backward cycle instead.
  CHECK_EQ(floatingFractionSize(1.0 / 3, 1280), 427);
  CHECK_EQ(cycledPixels(427, 1280, -1), 853);
  CHECK_EQ(cycledPixels(427, 1280, 1), 640);
}

UMBRIEL_TEST(aFloatSizeNoPresetProducedKeepsItsMeasuredFraction) {
  // A pointer resize lands between presets. The cycle steps from where the
  // window actually is, not from a preset it never reached.
  CHECK(presetSnappedFraction(kPresets, 351, 700) == floatingSizeFraction(351, 700));
  CHECK_EQ(cycledPixels(351, 700, 1), 467);
  CHECK_EQ(cycledPixels(351, 700, -1), 350);
}

UMBRIEL_TEST(aFloatCycleWrapsAtTheEnds) {
  CHECK_EQ(cycledPixels(900, 1080, 1), 360);
  CHECK_EQ(cycledPixels(100, 1080, -1), 720);
}

UMBRIEL_TEST(boundedPresetsDropStepsAFloatCannotRealize) {
  const std::vector<double> presets{0.5, 1.0, 1.5, 2.0};
  const std::vector<double> floats{0.5, 1.0};
  CHECK_EQ(boundedPresets(presets, 1.0), floats);
  // Nothing to drop when every preset fits.
  CHECK_EQ(boundedPresets(floats, 1.0), floats);
}

UMBRIEL_TEST(aFloatCycleNeverStallsOnAWidePreset) {
  const auto floats = boundedPresets({0.5, 1.0, 1.5, 2.0}, 1.0);
  CHECK(nextFractionPreset(floats, 0.5, 1) == 1.0);
  // Wraps back to the smallest preset instead of parking on a 1.5 the float cannot reach.
  CHECK(nextFractionPreset(floats, 1.0, 1) == 0.5);
  CHECK(nextFractionPreset(floats, 1.0, -1) == 0.5);
}

int main() { return RUN_TESTS(); }
