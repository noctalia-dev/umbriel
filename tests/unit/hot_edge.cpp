#include "input/hot_edge.h"

#include "check.h"

#include <array>

using umbriel::HotEdgeAccumulator;
using umbriel::hotEdgeHasNeighbor;
using umbriel::hotEdgeSide;
using umbriel::HotEdgeSide;
using umbriel::OutputBox;

UMBRIEL_TEST(overTravelSelectsTheEdgeItPointsAt) {
  CHECK(hotEdgeSide(-10, 0) == HotEdgeSide::Left);
  CHECK(hotEdgeSide(10, 0) == HotEdgeSide::Right);
  CHECK(hotEdgeSide(0, -10) == HotEdgeSide::Top);
  CHECK(hotEdgeSide(0, 10) == HotEdgeSide::Bottom);
  CHECK(hotEdgeSide(9, -3) == HotEdgeSide::Right);
  CHECK(hotEdgeSide(-3, 9) == HotEdgeSide::Bottom);
  CHECK(!hotEdgeSide(0, 0));
}

UMBRIEL_TEST(horizontalTiesCountAsLeftOrRight) {
  CHECK(hotEdgeSide(5, 5) == HotEdgeSide::Right);
  CHECK(hotEdgeSide(-5, -5) == HotEdgeSide::Left);
  CHECK(hotEdgeSide(5.5, 5.4) == HotEdgeSide::Right);
  CHECK(hotEdgeSide(5.4, 5.5) == HotEdgeSide::Bottom);
}

UMBRIEL_TEST(diagonalNearASharedEdgeIsSuppressedByTheNeighbor) {
  // A nibble near the top of the shared edge lands in the dead space above both outputs, so the clamp reports a
  // mostly horizontal over-travel.
  constexpr std::array boxes{
      OutputBox{0, 0, 1920, 1080},
      OutputBox{1920, 0, 1920, 1080},
  };
  const auto side = hotEdgeSide(16, -5);
  CHECK(side == HotEdgeSide::Right);
  CHECK(hotEdgeHasNeighbor(boxes, 0, *side, 1919, 0));
  CHECK(!hotEdgeHasNeighbor(boxes, 1, *side, 3839, 0));
  CHECK(hotEdgeHasNeighbor(boxes, 1, HotEdgeSide::Left, 1920, 0));
  CHECK(!hotEdgeHasNeighbor(boxes, 0, HotEdgeSide::Left, 0, 0));
  CHECK(!hotEdgeHasNeighbor(boxes, 0, HotEdgeSide::Top, 0, 0));
  CHECK(!hotEdgeHasNeighbor(boxes, 0, HotEdgeSide::Bottom, 0, 1079));
}

UMBRIEL_TEST(offsetOutputsStillCountAsNeighbors) {
  constexpr std::array boxes{
      OutputBox{0, 0, 1920, 1080},
      OutputBox{1920, 400, 1920, 1080},
  };
  CHECK(hotEdgeHasNeighbor(boxes, 0, HotEdgeSide::Right, 1919, 100));
  CHECK(!hotEdgeHasNeighbor(boxes, 0, HotEdgeSide::Left, 0, 100));
}

UMBRIEL_TEST(accumulatorFiresOncePerExcursion) {
  HotEdgeAccumulator accumulator;
  CHECK(!accumulator.push(HotEdgeSide::Left, 100, 300));
  CHECK(!accumulator.push(HotEdgeSide::Left, 199, 300));
  CHECK(accumulator.push(HotEdgeSide::Left, 1, 300));
  CHECK(!accumulator.push(HotEdgeSide::Left, 500, 300));
  accumulator.reset();
  CHECK(!accumulator.push(HotEdgeSide::Left, 299, 300));
  CHECK(accumulator.push(HotEdgeSide::Left, 1, 300));
}

UMBRIEL_TEST(accumulatorRestartsWhenTheEdgeChanges) {
  HotEdgeAccumulator accumulator;
  CHECK(!accumulator.push(HotEdgeSide::Left, 250, 300));
  CHECK(!accumulator.push(HotEdgeSide::Top, 100, 300));
  CHECK(!accumulator.push(HotEdgeSide::Left, 100, 300));
  CHECK(accumulator.push(HotEdgeSide::Left, 200, 300));
}

UMBRIEL_TEST(oneLargeMotionFires) {
  HotEdgeAccumulator accumulator;
  CHECK(accumulator.push(HotEdgeSide::Right, 1000, 300));
}

int main() { return RUN_TESTS(); }
