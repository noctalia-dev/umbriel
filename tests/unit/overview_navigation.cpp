#include "check.h"
#include "overview/navigation.h"

using umbriel::NavigationSource;
using umbriel::OverviewNavigation;

UMBRIEL_TEST(diagonalInputLocksOnceAndRetainsInitialTravel) {
  OverviewNavigation nav;
  nav.update(7, 5, 10);
  CHECK_EQ(nav.axis(), OverviewNavigation::Axis::Pending);
  nav.update(7, 5, 20);
  CHECK_EQ(nav.axis(), OverviewNavigation::Axis::Horizontal);
  CHECK_EQ(nav.position(), 14.0);
  nav.update(-4, 100, 30);
  CHECK_EQ(nav.axis(), OverviewNavigation::Axis::Horizontal);
  CHECK_EQ(nav.position(), 10.0);
  nav.reset();
  nav.update(0, -20, 40);
  CHECK_EQ(nav.axis(), OverviewNavigation::Axis::Vertical);
  CHECK_EQ(nav.position(), -20.0);
}

UMBRIEL_TEST(releaseAfterPauseDoesNotRetainFlickVelocity) {
  OverviewNavigation nav;
  nav.update(0, 20, 100);
  nav.update(0, 20, 120);
  CHECK(nav.projectedPosition() > nav.position());
  nav.update(0, 0, 400);
  CHECK_EQ(nav.projectedPosition(), 40.0);
}

UMBRIEL_TEST(workspaceSettlementIsBoundedAndAllowsMultipleRows) {
  CHECK_EQ(OverviewNavigation::workspaceTarget(-10, 4), 0);
  CHECK_EQ(OverviewNavigation::workspaceTarget(2.49, 4), 2);
  CHECK_EQ(OverviewNavigation::workspaceTarget(2.51, 4), 3);
  CHECK_EQ(OverviewNavigation::workspaceTarget(10, 4), 4);
  CHECK_EQ(OverviewNavigation::workspaceTarget(10, 0), 0);
}

UMBRIEL_TEST(travelCoversOneStepWhateverTheScreenMeasures) {
  for (const auto source : {NavigationSource::Scroll, NavigationSource::Swipe}) {
    const auto travel = OverviewNavigation::travelFor(source);
    // One step of travel is one workspace, and at zoom 1 one viewport of strip on any screen.
    CHECK_EQ(OverviewNavigation::travelScale(1.0, 1.0, 1.0, travel.workspace) * travel.workspace, 1.0);
    for (const double extent : {720.0, 1280.0, 2160.0, 3840.0}) {
      const double scale = OverviewNavigation::travelScale(extent, 1.0, 1.0, travel.viewport);
      CHECK(std::abs(scale * travel.viewport - extent) < 0.000001);
      // Half the factor, twice the travel for the same distance.
      CHECK(std::abs(OverviewNavigation::travelScale(extent, 1.0, 0.5, travel.viewport) * 2.0 - scale) < 0.000001);
    }
  }
  // Accelerated swipe deltas and unaccelerated scroll units are not the same distance.
  CHECK(
      OverviewNavigation::travelFor(NavigationSource::Swipe).workspace
      != OverviewNavigation::travelFor(NavigationSource::Scroll).workspace
  );
}

UMBRIEL_TEST(releaseProjectionAvoidsExtraWorkspaceWithoutLimitingLongSwipes) {
  const double units = OverviewNavigation::kScrollTravel.workspace;
  const double scale = OverviewNavigation::travelScale(1.0, 0.5, 1.0, units);
  CHECK_EQ(OverviewNavigation::workspaceTarget(OverviewNavigation::projectRelease(250, 1000) * scale, 5), 1);
  CHECK_EQ(OverviewNavigation::workspaceTarget(OverviewNavigation::projectRelease(250, 3500) * scale, 5), 2);
  CHECK_EQ(OverviewNavigation::workspaceTarget(OverviewNavigation::projectRelease(1000, 0) * scale, 5), 3);
  CHECK_EQ(OverviewNavigation::projectRelease(250, -1000), 130.0);
  CHECK_EQ(OverviewNavigation::projectRelease(250, 0), 250.0);
}

UMBRIEL_TEST(overscrollIsContinuousAndBoundedAtBothEnds) {
  const double limit = OverviewNavigation::kOverscroll;
  CHECK_EQ(OverviewNavigation::rubberBandDerivative(1.5, 3, limit), 1.0);
  CHECK(OverviewNavigation::rubberBandDerivative(-1, 3, limit) < 0.02);
  CHECK_EQ(
      OverviewNavigation::rubberBandDerivative(-1, 3, limit), OverviewNavigation::rubberBandDerivative(4, 3, limit)
  );
  CHECK_EQ(OverviewNavigation::rubberBand(1.5, 3, limit), 1.5);
  CHECK(OverviewNavigation::rubberBand(-1, 3, limit) > -limit);
  CHECK(OverviewNavigation::rubberBand(-1, 3, limit) < 0);
  CHECK(OverviewNavigation::rubberBand(4, 3, limit) > 3);
  CHECK(OverviewNavigation::rubberBand(1000, 3, limit) < 3 + limit);
  CHECK_EQ(OverviewNavigation::zoomScale(1), 1.0);
  CHECK(OverviewNavigation::zoomScale(0.5) > 1);
  CHECK(OverviewNavigation::zoomScale(0.5) < 2);
}

int main() { return RUN_TESTS(); }
