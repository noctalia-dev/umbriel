#include "view/window_region.h"

#include "check.h"

extern "C" {
#include <wlr/util/edges.h>
}

using namespace umbriel;

UMBRIEL_TEST(halvesShareABoundary) {
  const wlr_box area{8, 8, 1264, 704};
  const auto left = windowRegionBox(area, WindowRegion::Left, 8);
  const auto right = windowRegionBox(area, WindowRegion::Right, 8);
  CHECK_EQ(left.x, 8);
  CHECK_EQ(left.width, 628);
  CHECK_EQ(right.x, 644);
  CHECK_EQ(right.width, 628);
  CHECK_EQ(left.height, 704);
}

UMBRIEL_TEST(oddWidthGivesTheRemainderRight) {
  CHECK_EQ(windowRegionBox({8, 8, 1265, 704}, WindowRegion::Left, 8).width, 628);
  CHECK_EQ(windowRegionBox({8, 8, 1265, 704}, WindowRegion::Right, 8).width, 629);
}

UMBRIEL_TEST(quartersSplitBothAxes) {
  const auto box = windowRegionBox({8, 8, 1264, 704}, WindowRegion::BottomRight, 8);
  CHECK_EQ(box.x, 644);
  CHECK_EQ(box.y, 364);
  CHECK_EQ(box.width, 628);
  CHECK_EQ(box.height, 348);
}

UMBRIEL_TEST(cornerBeatsEdge) {
  CHECK(dropZoneAt({0, 0, 1280, 720}, 2, 40, 0) == WindowRegion::TopLeft);
  CHECK(dropZoneAt({0, 0, 1280, 720}, 1279, 719, 0) == WindowRegion::BottomRight);
}

UMBRIEL_TEST(verticalEdgeDropZonesSelectHalves) {
  CHECK(dropZoneAt({0, 0, 1280, 720}, 640, 0, 0) == WindowRegion::Top);
  CHECK(dropZoneAt({0, 0, 1280, 720}, 640, 719, 0) == WindowRegion::Bottom);
  CHECK(!dropZoneAt({0, 0, 1280, 720}, 640, 719, WLR_EDGE_BOTTOM));
  CHECK(!dropZoneAt({0, 0, 1280, 720}, 640, 0, WLR_EDGE_TOP));
}

UMBRIEL_TEST(verticalHalvesKeepFullWidthAndGiveRemainderToBottom) {
  const auto top = windowRegionBox({-600, 20, 1000, 705}, WindowRegion::Top, 8);
  const auto bottom = windowRegionBox({-600, 20, 1000, 705}, WindowRegion::Bottom, 8);
  CHECK_EQ(top.x, -600);
  CHECK_EQ(top.y, 20);
  CHECK_EQ(top.width, 1000);
  CHECK_EQ(top.height, 348);
  CHECK_EQ(bottom.x, -600);
  CHECK_EQ(bottom.y, 376);
  CHECK_EQ(bottom.width, 1000);
  CHECK_EQ(bottom.height, 349);
}

UMBRIEL_TEST(closedEdgesHaveNoDropZones) {
  CHECK(!dropZoneAt({0, 0, 1280, 720}, 1279, 360, WLR_EDGE_RIGHT));
  CHECK(!dropZoneAt({0, 0, 1280, 720}, 2, 2, WLR_EDGE_LEFT | WLR_EDGE_TOP));
}

UMBRIEL_TEST(regionsUseLogicalCoordinatesOnNegativeOutputs) {
  CHECK(dropZoneAt({-1024, -576, 1024, 576}, -1023, -300, 0) == WindowRegion::Left);
  CHECK(!dropZoneAt({-1024, -576, 1024, 576}, 0, -300, 0));
}

UMBRIEL_TEST(regionNamesRoundTrip) {
  for (const auto* const name :
       {"left", "right", "top", "bottom", "top_left", "top_right", "bottom_left", "bottom_right"}) {
    const auto region = windowRegionNamed(name);
    CHECK(region.has_value());
    if (region)
      CHECK_EQ(windowRegionName(*region), name);
  }
  CHECK(!windowRegionNamed("top-left"));
  CHECK(!windowRegionNamed(""));
}

int main() { return RUN_TESTS(); }
