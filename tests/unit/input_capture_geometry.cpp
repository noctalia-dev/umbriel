#include "input/input_capture_geometry.h"

#include "check.h"

#include <array>
#include <limits>

using umbriel::InputCaptureBarrier;
using umbriel::inputCaptureCrossing;
using umbriel::InputCaptureZone;
using umbriel::validInputCaptureBarrier;

UMBRIEL_TEST(exteriorBarriersUseLogicalCoordinatesAndInclusivePixelEndpoints) {
  const std::array zones{InputCaptureZone{-1280, -200, 1280, 720}, InputCaptureZone{0, 0, 1536, 864}};
  CHECK(validInputCaptureBarrier(zones, {1, -1280, -200, -1280, 519}));
  CHECK(validInputCaptureBarrier(zones, {2, 1536, 100, 1536, 400}));
  CHECK(validInputCaptureBarrier(zones, {3, 0, 864, 1535, 864}));
  CHECK(!validInputCaptureBarrier(zones, {4, 0, 864, 1536, 864}));
  CHECK(!validInputCaptureBarrier(zones, {5, -100, 100, 100, 100}));
  CHECK(!validInputCaptureBarrier(zones, {0, 1536, 100, 1536, 400}));
  CHECK(!validInputCaptureBarrier(zones, {6, 0, 0, 100, 100}));
}

UMBRIEL_TEST(sharedEdgesAreRejectedWhileExposedEdgeSegmentsRemainAvailable) {
  const std::array zones{InputCaptureZone{0, 0, 100, 100}, InputCaptureZone{100, 30, 100, 70}};
  CHECK(!validInputCaptureBarrier(zones, {1, 100, 0, 100, 99}));
  CHECK(!validInputCaptureBarrier(zones, {2, 100, 30, 100, 99}));
  CHECK(validInputCaptureBarrier(zones, {3, 100, 0, 100, 29}));
  CHECK(!validInputCaptureBarrier(zones, {4, 100, 29, 100, 30}));
  const std::array mirrored{InputCaptureZone{0, 0, 100, 100}, InputCaptureZone{0, 0, 100, 100}};
  CHECK(validInputCaptureBarrier(mirrored, {5, 100, 0, 100, 99}));
}

UMBRIEL_TEST(crossingUsesUnclippedMotionAndPreservesOvershootFraction) {
  const std::array zones{InputCaptureZone{-100, -50, 200, 100}};
  const std::array barriers{InputCaptureBarrier{9, 100, -50, 100, 49}};
  const auto crossing = inputCaptureCrossing(zones, barriers, 90, 0, 40, 10);
  CHECK(crossing.has_value());
  if (!crossing) {
    return;
  }
  CHECK_EQ(crossing->id, 9U);
  CHECK_EQ(crossing->fraction, 0.25);
  CHECK(inputCaptureCrossing(zones, barriers, 90, 49, 20, 0).has_value());
  CHECK(!inputCaptureCrossing(zones, barriers, 90, 0, -40, 0).has_value());
  CHECK(!inputCaptureCrossing(zones, barriers, 90, 49, 40, 8).has_value());
}

UMBRIEL_TEST(reachingTheIncludedLeftPixelDoesNotYetLeaveTheZone) {
  const std::array zones{InputCaptureZone{0, 0, 100, 100}};
  const std::array left{InputCaptureBarrier{1, 0, 0, 0, 99}};
  CHECK(!inputCaptureCrossing(zones, left, 10, 50, -10, 0).has_value());
  CHECK(inputCaptureCrossing(zones, left, 10, 50, -11, 0).has_value());
  CHECK(inputCaptureCrossing(zones, left, 0, 50, -1, 0).has_value());
  CHECK(!inputCaptureCrossing(zones, left, 0, 50, 1, 0).has_value());
  const std::array right{InputCaptureBarrier{2, 100, 0, 100, 99}};
  CHECK(inputCaptureCrossing(zones, right, 90, 50, 10, 0).has_value());
}

UMBRIEL_TEST(theFirstCrossedBarrierWinsRegardlessOfRegistrationOrder) {
  const std::array zones{InputCaptureZone{0, 0, 100, 100}, InputCaptureZone{200, 0, 100, 100}};
  const std::array barriers{InputCaptureBarrier{2, 300, 0, 300, 99}, InputCaptureBarrier{1, 100, 0, 100, 99}};
  const auto crossing = inputCaptureCrossing(zones, barriers, 0, 50, 400, 0);
  CHECK(crossing.has_value());
  if (!crossing) {
    return;
  }
  CHECK_EQ(crossing->id, 1U);
  CHECK_EQ(crossing->fraction, 0.25);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(!inputCaptureCrossing(zones, barriers, 90, 20, nan, -40).has_value());
}

UMBRIEL_TEST(oneMotionCanCrossAdjacentOutputsBeforeReachingTheOuterBarrier) {
  const std::array zones{InputCaptureZone{0, 0, 100, 100}, InputCaptureZone{100, 0, 100, 100}};
  const std::array barriers{InputCaptureBarrier{7, 200, 0, 200, 99}};
  const auto crossing = inputCaptureCrossing(zones, barriers, 50, 50, 200, 0);
  CHECK(crossing.has_value());
  if (crossing) {
    CHECK_EQ(crossing->id, 7U);
    CHECK_EQ(crossing->fraction, 0.75);
  }
  CHECK(!inputCaptureCrossing(zones, barriers, 250, 50, 20, 0).has_value());
}

UMBRIEL_TEST(aSinglePixelCornerBarrierCanTriggerOnEitherExteriorEdge) {
  const std::array zones{InputCaptureZone{0, 0, 100, 100}};
  const std::array barriers{InputCaptureBarrier{1, 0, 0, 0, 0}};
  CHECK(validInputCaptureBarrier(zones, barriers.front()));
  CHECK(inputCaptureCrossing(zones, barriers, 0.25, 0.25, -1, 0).has_value());
  CHECK(inputCaptureCrossing(zones, barriers, 0.25, 0.25, 0, -1).has_value());
}

int main() { return RUN_TESTS(); }
