#include "layout/layout_motion.h"

#include "check.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using umbriel::interpolateBox;
using umbriel::keepsSeparation;
using umbriel::MotionBox;

namespace {

  constexpr int kGap = 12;

  bool sameBox(const wlr_fbox& a, const auto& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }

  // True when the interiors intersect; touching edges do not count.
  bool overlaps(const wlr_fbox& a, const wlr_fbox& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) {
      return false;
    }
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

  // The largest gap separating two boxes along one axis; negative when they overlap on both.
  double separation(const wlr_fbox& a, const wlr_fbox& b) {
    return std::max({b.x - (a.x + a.width), a.x - (b.x + b.width), b.y - (a.y + a.height), a.y - (b.y + b.height)});
  }

  // Every pair of members stays disjoint, at least `minGap` apart, at every shared progress step.
  void checkDisjointThroughout(std::span<const MotionBox> members, int minGap = 0) {
    for (int step = 0; step <= 20; ++step) {
      const double progress = step * 0.05;
      for (size_t i = 0; i < members.size(); ++i) {
        for (size_t j = i + 1; j < members.size(); ++j) {
          const wlr_fbox a = interpolateBox(members[i].from, members[i].to, progress);
          const wlr_fbox b = interpolateBox(members[j].from, members[j].to, progress);
          CHECK(!overlaps(a, b));
          CHECK(separation(a, b) >= minGap - 0.000001);
        }
      }
    }
  }

  void checkAllKeepSeparation(std::span<const MotionBox> members) {
    for (size_t i = 0; i < members.size(); ++i) {
      for (size_t j = i + 1; j < members.size(); ++j) {
        CHECK(keepsSeparation(members[i], members[j]));
      }
    }
  }

  constexpr wlr_box kColumnA{0, 0, 400, 800};
  constexpr wlr_box kColumnB{400 + kGap, 0, 400, 800};
  constexpr wlr_box kColumnC{2 * (400 + kGap), 0, 400, 800};

} // namespace

UMBRIEL_TEST(interpolateBoxEndpointsAreExact) {
  const wlr_box from{3, 7, 100, 50};
  const wlr_box to{-40, 12, 5, 0};
  CHECK(sameBox(interpolateBox(from, to, 0.0), from));
  CHECK(sameBox(interpolateBox(from, to, 1.0), to));
}

UMBRIEL_TEST(interpolateBoxEdgesMoveMonotonically) {
  const wlr_box from{0, 0, 7, 7};
  const wlr_box to{100, 0, 13, 7};
  double lastLeft = from.x;
  double lastRight = from.x + from.width;
  for (int step = 1; step <= 20; ++step) {
    const wlr_fbox box = interpolateBox(from, to, step * 0.05);
    CHECK(box.x >= lastLeft);
    CHECK(box.x + box.width >= lastRight);
    lastLeft = box.x;
    lastRight = box.x + box.width;
  }
}

UMBRIEL_TEST(interpolateBoxSplitsPositionFromSize) {
  const wlr_box from{0, 0, 800, 900};
  const wlr_box to{400, 500, 400, 400};
  // A resize that has finished while the move is a quarter done: the final size at a quarter of the travel.
  CHECK(sameBox(interpolateBox(from, to, 0.25, 1.0), wlr_box{100, 125, 400, 400}));
  // A move that has finished while the resize lags: the final origin with a size still on its way.
  CHECK(sameBox(interpolateBox(from, to, 1.0, 0.5), wlr_box{400, 500, 600, 650}));
  for (int step = 0; step <= 20; ++step) {
    const double progress = step * 0.05;
    CHECK(sameBox(interpolateBox(from, to, progress, progress), interpolateBox(from, to, progress)));
  }
}

UMBRIEL_TEST(presentationEdgesReachEveryPhysicalPixel) {
  const wlr_box from{0, 0, 100, 50};
  const wlr_box to{2, 0, 102, 50};
  // At 150%, each third of this two-logical-pixel move is one physical pixel.
  for (int step = 0; step <= 3; ++step) {
    const double progress = step / 3.0;
    const wlr_fbox box = interpolateBox(from, to, progress, progress);
    CHECK(std::lround(box.x * 1.5) == step);
    CHECK(std::lround((box.x + box.width) * 1.5) == 150 + 2 * step);
  }
  const wlr_fbox a = interpolateBox(from, to, 0.2, 0.2);
  const wlr_fbox b = interpolateBox({100, 0, 50, 50}, {104, 0, 50, 50}, 0.2, 0.2);
  CHECK(std::abs(a.x + a.width - b.x) < 0.000001);
}

UMBRIEL_TEST(presentationMotionKeepsAlignedEndpointsAndRetargets) {
  constexpr double scale = 1.5;
  const wlr_box output{100, -20, 1706, 1066};
  const wlr_box from{110, -10, 100, 100};
  const wlr_box to{111, -9, 101, 101};
  const wlr_fbox nearEnd = umbriel::interpolatePresentationBox(from, to, 0.999, 0.999, scale, output, 2560, 1600);
  CHECK_EQ(std::lround((nearEnd.x - output.x) * scale), 17);
  CHECK_EQ(std::lround((nearEnd.y - output.y) * scale), 17);
  const wlr_fbox resumed = umbriel::interpolatePresentationBox(to, output, 0, 0, scale, output, 2560, 1600, &nearEnd);
  CHECK(sameBox(resumed, nearEnd));
  const wlr_fbox full = umbriel::interpolatePresentationBox(to, output, 0.99999, 0.99999, scale, output, 2560, 1600);
  CHECK_EQ(std::lround((full.x + full.width - output.x) * scale), 2560);
  CHECK_EQ(std::lround((full.y + full.height - output.y) * scale), 1600);
  CHECK(sameBox(umbriel::interpolatePresentationBox(to, output, 1, 1, scale, output, 2560, 1600), output));
}

UMBRIEL_TEST(establishedColumnsStaySeparateDuringInsertionReflow) {
  const std::vector<MotionBox> peers{
      {{0, 0, 600, 800}, kColumnA},
      {{600 + kGap, 0, 600, 800}, kColumnB},
  };
  checkDisjointThroughout(peers, kGap);
  checkAllKeepSeparation(peers);
}

UMBRIEL_TEST(establishedColumnsStaySeparateDuringClosingReflow) {
  const std::vector<MotionBox> peers{
      {kColumnA, {0, 0, 600, 800}},
      {kColumnC, {600 + kGap, 0, 600, 800}},
  };
  checkDisjointThroughout(peers, kGap);
  checkAllKeepSeparation(peers);
}

UMBRIEL_TEST(keepsSeparationFlagsRearrangements) {
  const int rowHeight = (800 - kGap) / 2;
  // A above B becomes A left of B (expel), C shifts right to make room.
  const MotionBox a{{0, 0, 400, rowHeight}, kColumnA};
  const MotionBox b{{0, rowHeight + kGap, 400, 800 - rowHeight - kGap}, kColumnB};
  const MotionBox c{kColumnB, kColumnC};
  CHECK(!keepsSeparation(a, b));
  CHECK(keepsSeparation(b, c));
  CHECK(keepsSeparation(a, c));
}

int main() { return RUN_TESTS(); }
