#include "check.h"
#include "overview/navigation.h"

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

UMBRIEL_TEST(overscrollIsContinuousAndBoundedAtBothEnds) {
  CHECK_EQ(OverviewNavigation::rubberBand(1.5, 3, 0.15), 1.5);
  CHECK(OverviewNavigation::rubberBand(-1, 3, 0.15) > -0.15);
  CHECK(OverviewNavigation::rubberBand(-1, 3, 0.15) < 0);
  CHECK(OverviewNavigation::rubberBand(4, 3, 0.15) > 3);
  CHECK(OverviewNavigation::rubberBand(1000, 3, 0.15) < 3.15);
  CHECK_EQ(OverviewNavigation::zoomScale(1), 1.0);
  CHECK(OverviewNavigation::zoomScale(0.5) > 1);
  CHECK(OverviewNavigation::zoomScale(0.5) < 2);
}

int main() { return RUN_TESTS(); }
