#include "input/gesture_physics.h"

#include "check.h"
#include "input/swipe_tracker.h"

using umbriel::GesturePhysics;
using umbriel::SwipeTracker;

namespace {

  // One step of finger travel: the distance kSwipeWorkspacePx gives a workspace.
  constexpr double kStep = 300.0;

  // Feeds `count` readings of `dx` every 10 ms, then the zero-delta sample a release leaves in the tracker. `holdMs`
  // is the pause between the last movement and that release sample, and zero means the fingers were still moving.
  SwipeTracker gestures(double dx, int count, uint32_t holdMs = 0) {
    SwipeTracker tracker;
    uint32_t timeMsec = 10;
    for (int i = 0; i < count; ++i, timeMsec += 10) {
      tracker.push(dx, timeMsec);
    }
    tracker.push(0.0, timeMsec + holdMs);
    return tracker;
  }

} // namespace

UMBRIEL_TEST(overscrollIsContinuousAndBoundedAtBothEnds) {
  const double limit = GesturePhysics::kOverscrollLimit;
  CHECK_EQ(GesturePhysics::rubberBand(0.5, 0.0, 1.0, limit), 0.5);
  CHECK(GesturePhysics::rubberBand(1.5, 0.0, 1.0, limit) > 1.0);
  CHECK(GesturePhysics::rubberBand(1.5, 0.0, 1.0, limit) < 1.0 + limit);
  CHECK(GesturePhysics::rubberBand(1000.0, 0.0, 1.0, limit) < 1.0 + limit);
  CHECK(GesturePhysics::rubberBand(-1.0, 0.0, 1.0, limit) < 0.0);
  CHECK(GesturePhysics::rubberBand(-1.0, 0.0, 1.0, limit) > -limit);
  // The same travel past either end moves the position by the same amount.
  const double pastEnd = GesturePhysics::rubberBand(1.25, 0.0, 1.0, limit) - 1.0;
  const double pastStart = 0.0 - GesturePhysics::rubberBand(-0.25, 0.0, 1.0, limit);
  CHECK(std::abs(pastEnd - pastStart) < 1e-12);
}

UMBRIEL_TEST(releaseSpeedIsDampedToWhatTheContentStillMoves) {
  const double limit = GesturePhysics::kOverscrollLimit;
  CHECK_EQ(GesturePhysics::rubberBandDerivative(0.5, 0.0, 1.0, limit), 1.0);
  CHECK(GesturePhysics::rubberBandDerivative(1.5, 0.0, 1.0, limit) < 1.0);
  CHECK(GesturePhysics::rubberBandDerivative(1.5, 0.0, 1.0, limit) > 0.0);
  CHECK(GesturePhysics::rubberBandDerivative(1000.0, 0.0, 1.0, limit) < 0.05);
  // Past either end, the same distance damps the same way.
  CHECK_EQ(
      GesturePhysics::rubberBandDerivative(1.5, 0.0, 1.0, limit),
      GesturePhysics::rubberBandDerivative(-0.5, 0.0, 1.0, limit)
  );
}

UMBRIEL_TEST(aStepLandsOnTheNearestStepThatExists) {
  CHECK_EQ(GesturePhysics::stepTarget(2.49, 0, 4), 2);
  CHECK_EQ(GesturePhysics::stepTarget(2.51, 0, 4), 3);
  CHECK_EQ(GesturePhysics::stepTarget(-10.0, 0, 4), 0);
  CHECK_EQ(GesturePhysics::stepTarget(10.0, 0, 4), 4);
  CHECK_EQ(GesturePhysics::stepTarget(10.0, 0, 0), 0);
  // A switch only ever reaches the neighbouring workspace.
  CHECK_EQ(GesturePhysics::stepTarget(-0.4, -1, 1), 0);
  CHECK_EQ(GesturePhysics::stepTarget(-0.6, -1, 1), -1);
}

UMBRIEL_TEST(zoomScalingKeepsTravelReadableAtAnyZoom) {
  CHECK_EQ(GesturePhysics::zoomScale(1), 1.0);
  CHECK(GesturePhysics::zoomScale(0.5) > 1);
  CHECK(GesturePhysics::zoomScale(0.5) < 2);
}

UMBRIEL_TEST(aDragThatStopsBeforeLettingGoFallsBack) {
  // 0.4 of a step, then a pause long enough to drain the window: nothing is moving any more, so the release rounds back
  // to the step the gesture started on.
  const auto release = GesturePhysics::release(gestures(30.0, 4, 200), kStep, 0.0, -1.0, 1.0);
  CHECK_EQ(release.target, 0);
  CHECK_EQ(release.velocity, 0.0);
}

UMBRIEL_TEST(aFlickLandsWhereItsProjectionSays) {
  // 0.2 of a step, released while still moving: the projection carries it over the halfway point, which the distance
  // alone never reached.
  const auto release = GesturePhysics::release(gestures(10.0, 6), kStep, 0.0, -1.0, 1.0);
  CHECK(release.position < 0.5);
  CHECK_EQ(release.target, 1);
  CHECK(release.velocity > 0.0);
}

UMBRIEL_TEST(aGestureCarriesThePositionItTookOver) {
  // A gesture that starts while a settle is still running begins from where the slide is on screen, so its landing is
  // relative to that: half a step behind the active workspace and released still lands on the previous one.
  const auto fromRest = GesturePhysics::release(gestures(0.0, 1, 200), kStep, 0.0, -1.0, 1.0);
  CHECK_EQ(fromRest.position, 0.0);
  CHECK_EQ(fromRest.target, 0);
  const auto carried = GesturePhysics::release(gestures(0.0, 1, 200), kStep, -0.5, -1.0, 1.0);
  CHECK_EQ(carried.position, -0.5);
  CHECK_EQ(carried.target, -1);
}

UMBRIEL_TEST(theReleaseKeepsTheSpeedTheGestureWasMovingAt) {
  const auto slow = GesturePhysics::release(gestures(5.0, 12), kStep, 0.0, -1.0, 1.0);
  const auto fast = GesturePhysics::release(gestures(20.0, 12), kStep, 0.0, -1.0, 1.0);
  CHECK(slow.velocity > 0.0);
  CHECK(fast.velocity > slow.velocity);
}

UMBRIEL_TEST(travelPastTheLastStepIsDampedAndSoIsItsRelease) {
  // Three steps of travel into the end of a one-step range: the position stops just past the end, and the release
  // carries far less speed than the same gesture would with somewhere to go.
  const auto bounded = GesturePhysics::release(gestures(45.0, 20), kStep, 0.0, 0.0, 1.0);
  const auto unbounded = GesturePhysics::release(gestures(45.0, 20), kStep, 0.0, 0.0, 10.0);
  CHECK(bounded.position > 1.0);
  CHECK(bounded.position < 1.0 + GesturePhysics::kOverscrollLimit);
  CHECK_EQ(bounded.target, 1);
  CHECK(bounded.velocity > 0.0);
  CHECK(bounded.velocity < unbounded.velocity);
}

UMBRIEL_TEST(theOverviewSettlesOnTheSideTheProjectionPointsTo) {
  // An open overview dragged a third of a step down and held still falls back open.
  const auto held = GesturePhysics::release(gestures(-20.0, 5, 200), kStep, 1.0, 0.0, 1.0);
  CHECK_EQ(held.target, 1);
  // The same drag released while the fingers were still moving crosses the halfway point and closes instead.
  const auto flicked = GesturePhysics::release(gestures(-20.0, 5), kStep, 1.0, 0.0, 1.0);
  CHECK_EQ(flicked.target, 0);
  // Dragged hard past the open end, the position is damped and never leaves the range by more than the limit.
  const auto pastEnd = GesturePhysics::release(gestures(30.0, 12, 200), kStep, 1.0, 0.0, 1.0);
  CHECK(pastEnd.position > 1.0);
  CHECK(pastEnd.position < 1.0 + GesturePhysics::kOverscrollLimit);
  CHECK_EQ(pastEnd.target, 1);
}

int main() { return RUN_TESTS(); }
