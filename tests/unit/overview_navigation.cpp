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

UMBRIEL_TEST(releaseProjectionCarriesAFlickPastWhereTheFingersStopped) {
  OverviewNavigation nav;
  nav.update(0, 50, 10);
  nav.update(0, 50, 20);
  nav.update(0, 0, 30);
  CHECK(nav.velocity() > 0.0);
  CHECK(nav.projectedPosition() > nav.position());
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

int main() { return RUN_TESTS(); }
