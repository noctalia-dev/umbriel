#include "check.h"
#include "output/direction.h"

#include <array>

using umbriel::adjacentOutputIndex;
using umbriel::cyclicOutputIndex;
using umbriel::OutputBox;
using umbriel::OutputDirection;

UMBRIEL_TEST(scaledOutputsMayOverlapAtTheirSharedEdge) {
  // Reported layout: a 3840x2160 output at scale 1.6, with the scaled laptop
  // panel centered below it. Rounding leaves a two-logical-pixel overlap.
  constexpr std::array boxes{
      OutputBox{0, 0, 2400, 1350},
      OutputBox{416, 1348, 1646, 1066},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Down, 1200, 675), std::optional<size_t>{1});
  CHECK_EQ(adjacentOutputIndex(boxes, 1, OutputDirection::Up, 1239, 1881), std::optional<size_t>{0});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Left, 1200, 675), std::optional<size_t>{});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 1200, 675), std::optional<size_t>{});
}

UMBRIEL_TEST(horizontalNeighborsMayOverlapAfterRounding) {
  constexpr std::array boxes{
      OutputBox{0, 0, 1000, 800},
      OutputBox{998, 50, 800, 700},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 500, 400), std::optional<size_t>{1});
  CHECK_EQ(adjacentOutputIndex(boxes, 1, OutputDirection::Left, 1398, 400), std::optional<size_t>{0});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Down, 500, 400), std::optional<size_t>{});
}

UMBRIEL_TEST(nearestDirectionalOutputWins) {
  constexpr std::array boxes{
      OutputBox{0, 0, 1000, 800},
      OutputBox{1000, 0, 800, 800},
      OutputBox{1800, 0, 800, 800},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 500, 400), std::optional<size_t>{1});
}

UMBRIEL_TEST(outputCyclingFollowsLayoutOrderAndWraps) {
  // Declared out of layout order, so a cycle that followed the input order
  // would visit the middle monitor first.
  constexpr std::array boxes{
      OutputBox{1000, 0, 800, 800},
      OutputBox{1800, 0, 800, 800},
      OutputBox{0, 0, 1000, 800},
  };

  CHECK_EQ(cyclicOutputIndex(boxes, 2, 1), std::optional<size_t>{0});
  CHECK_EQ(cyclicOutputIndex(boxes, 0, 1), std::optional<size_t>{1});
  CHECK_EQ(cyclicOutputIndex(boxes, 1, 1), std::optional<size_t>{2});
  CHECK_EQ(cyclicOutputIndex(boxes, 2, -1), std::optional<size_t>{1});
  CHECK_EQ(cyclicOutputIndex(boxes, 0, -1), std::optional<size_t>{2});
}

UMBRIEL_TEST(stackedOutputsCycleTopToBottom) {
  // Same x: the secondary key orders them, so a vertical stack cycles downwards
  // and wraps back to the top.
  constexpr std::array boxes{
      OutputBox{0, 1080, 1920, 1080},
      OutputBox{0, 0, 1920, 1080},
  };

  CHECK_EQ(cyclicOutputIndex(boxes, 1, 1), std::optional<size_t>{0});
  CHECK_EQ(cyclicOutputIndex(boxes, 0, 1), std::optional<size_t>{1});
  // Two outputs: either step reaches the other one, so one keybind suffices.
  CHECK_EQ(cyclicOutputIndex(boxes, 0, -1), std::optional<size_t>{1});
  CHECK_EQ(cyclicOutputIndex(boxes, 1, -1), std::optional<size_t>{0});
}

UMBRIEL_TEST(outputCyclingNeedsASecondOutputAndAKnownReference) {
  constexpr std::array lone{OutputBox{0, 0, 1920, 1080}};
  CHECK_EQ(cyclicOutputIndex(lone, 0, 1), std::optional<size_t>{});

  constexpr std::array pair{
      OutputBox{0, 0, 1920, 1080},
      OutputBox{1920, 0, 1920, 1080},
  };
  CHECK_EQ(cyclicOutputIndex(pair, 2, 1), std::optional<size_t>{});
  CHECK_EQ(cyclicOutputIndex(pair, 0, 0), std::optional<size_t>{});
}

int main() { return RUN_TESTS(); }
