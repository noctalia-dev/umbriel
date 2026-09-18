#include "layout/lifecycle_motion.h"

#include "check.h"

using umbriel::boxDistanceSquared;
using umbriel::boxIntersectionArea;
using umbriel::edgeLockedTarget;
using umbriel::edgeLockedTargetSized;
using umbriel::layoutPositionShouldSnap;
using umbriel::lifecycleArrangeShouldAnimate;

UMBRIEL_TEST(pendingOpeningRetainsAnimatedArrange) {
  CHECK(lifecycleArrangeShouldAnimate(true, false));
  CHECK(lifecycleArrangeShouldAnimate(true, true));
  CHECK(lifecycleArrangeShouldAnimate(false, true));
  CHECK(!lifecycleArrangeShouldAnimate(false, false));
}

UMBRIEL_TEST(unanimatedArrangePreservesMotionToTheSameTarget) {
  CHECK(!layoutPositionShouldSnap(true, false));
  CHECK(!layoutPositionShouldSnap(true, true));
  CHECK(!layoutPositionShouldSnap(false, true));
  CHECK(layoutPositionShouldSnap(false, false));
}

UMBRIEL_TEST(scrollingOpenStartsBesideTheOldStrip) {
  const wlr_box openingFinal{646, 10, 624, 700};
  const wlr_box neighbourFinal{10, 10, 624, 700};
  const wlr_box neighbourCurrent{328, 10, 624, 700};

  const auto start = edgeLockedTarget(openingFinal, neighbourFinal, neighbourCurrent);
  CHECK(start.has_value());
  CHECK_EQ(start->x, 964);
  CHECK_EQ(start->y, 10);
  CHECK_EQ(start->width, 624);
  CHECK_EQ(start->height, 700);
}

UMBRIEL_TEST(scrollingCloseFollowsTheReanchoredStrip) {
  const wlr_box closingCurrent{646, 10, 624, 700};
  const wlr_box neighbourCurrent{10, 10, 624, 700};
  const wlr_box neighbourFinal{328, 10, 624, 700};

  const auto target = edgeLockedTarget(closingCurrent, neighbourCurrent, neighbourFinal);
  CHECK(target.has_value());
  CHECK_EQ(target->x, 964);
  CHECK_EQ(target->y, 10);
}

UMBRIEL_TEST(dwindleOpenTracksTheShrinkingSplitEdge) {
  const wlr_box openingFinal{646, 10, 624, 700};
  const wlr_box neighbourFinal{10, 10, 624, 700};
  const wlr_box neighbourCurrent{10, 10, 1260, 700};

  const auto start = edgeLockedTarget(openingFinal, neighbourFinal, neighbourCurrent);
  CHECK(start.has_value());
  CHECK_EQ(start->x, 1282);
  CHECK_EQ(start->y, 10);
}

UMBRIEL_TEST(dwindleCloseTracksTheExpandingSplitEdge) {
  const wlr_box closingCurrent{646, 10, 624, 700};
  const wlr_box neighbourCurrent{10, 10, 624, 700};
  const wlr_box neighbourFinal{10, 10, 1260, 700};

  const auto target = edgeLockedTarget(closingCurrent, neighbourCurrent, neighbourFinal);
  CHECK(target.has_value());
  CHECK_EQ(target->x, 1282);
  CHECK_EQ(target->y, 10);
}

UMBRIEL_TEST(masterOpenTracksTheShrinkingMasterEdge) {
  const wlr_box openingFinal{713, 10, 557, 700};
  const wlr_box masterFinal{10, 10, 691, 700};
  const wlr_box masterCurrent{10, 10, 1260, 700};

  const auto start = edgeLockedTarget(openingFinal, masterFinal, masterCurrent);
  CHECK(start.has_value());
  CHECK_EQ(start->x, 1282);
  CHECK_EQ(start->y, 10);
}

UMBRIEL_TEST(masterCloseTracksTheExpandingMasterEdge) {
  const wlr_box closingCurrent{713, 10, 557, 700};
  const wlr_box masterCurrent{10, 10, 691, 700};
  const wlr_box masterFinal{10, 10, 1260, 700};

  const auto target = edgeLockedTarget(closingCurrent, masterCurrent, masterFinal);
  CHECK(target.has_value());
  CHECK_EQ(target->x, 1282);
  CHECK_EQ(target->y, 10);
}

UMBRIEL_TEST(masterStackOpeningUsesTheInitialClientHeight) {
  const wlr_box openingFinal{713, 10, 557, 347};
  const wlr_box previousStackFinal{713, 369, 557, 341};
  const wlr_box previousStackCurrent{713, 10, 557, 700};

  const auto start = edgeLockedTargetSized(openingFinal, previousStackFinal, previousStackCurrent, 1200, 700);
  CHECK(start.has_value());
  CHECK_EQ(start->x, 713);
  CHECK_EQ(start->y, -702);
  CHECK_EQ(start->width, 1200);
  CHECK_EQ(start->height, 700);
}

UMBRIEL_TEST(verticalMotionKeepsTheCrossAxisOffset) {
  const wlr_box subject{30, 372, 600, 338};
  const wlr_box anchorFrom{10, 10, 600, 350};
  const wlr_box anchorTo{40, 50, 600, 700};

  const auto target = edgeLockedTarget(subject, anchorFrom, anchorTo);
  CHECK(target.has_value());
  CHECK_EQ(target->x, 60);
  CHECK_EQ(target->y, 762);
}

UMBRIEL_TEST(rectangleDistanceSelectsTheAdjacentTile) {
  const wlr_box subject{646, 10, 624, 700};
  CHECK_EQ(boxDistanceSquared(subject, wlr_box{10, 10, 624, 700}), std::int64_t{144});
  CHECK_EQ(boxDistanceSquared(subject, wlr_box{-626, 10, 624, 700}), std::int64_t{419904});
  CHECK_EQ(boxDistanceSquared(subject, subject), std::int64_t{0});
}

UMBRIEL_TEST(intersectionIdentifiesTheTileVacatingARegion) {
  const wlr_box openingFinal{713, 10, 557, 347};
  const wlr_box unchangedMaster{10, 10, 691, 700};
  const wlr_box previousStack{713, 10, 557, 700};

  CHECK_EQ(boxIntersectionArea(openingFinal, unchangedMaster), std::int64_t{0});
  CHECK_EQ(boxIntersectionArea(openingFinal, previousStack), std::int64_t{193279});
}

int main() { return RUN_TESTS(); }
