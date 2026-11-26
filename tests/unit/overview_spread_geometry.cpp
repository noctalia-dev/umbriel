#include "check.h"
#include "overview/spread_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

using umbriel::SpreadBox;
using umbriel::spreadFit;
using umbriel::spreadInterpolate;
using umbriel::spreadLayout;
using umbriel::spreadOrigin;

UMBRIEL_TEST(originsUseLogicalWorkspaceAxesAndPreserveMonitorOffsets) {
  const SpreadBox output{1080, 420, 2560, 1080};
  const SpreadBox window{1120, 460, 1200, 900};
  const auto above = spreadOrigin(window, output, -2, false);
  CHECK_EQ(above.x, 1120);
  CHECK_EQ(above.y, -1700);
  const auto below = spreadOrigin(window, output, 1, false);
  CHECK_EQ(below.y, 1540);
  const auto left = spreadOrigin(window, output, -1, true);
  CHECK_EQ(left.x, -1440);
  CHECK_EQ(left.y, 460);
  const auto right = spreadOrigin(window, output, 2, true);
  CHECK_EQ(right.x, 6240);
  CHECK_EQ(right.width, window.width);
  const auto portrait = spreadOrigin({10, 30, 900, 1500}, {0, 0, 1080, 1920}, 1, false);
  CHECK_EQ(portrait.x, 10);
  CHECK_EQ(portrait.y, 1950);
}

UMBRIEL_TEST(transitionLandsExactlyAndPreservesAspectAtEveryFrame) {
  const SpreadBox origin{-2400, -1080, 1600, 900};
  const auto target = spreadFit(origin, {1200, 600, 500, 400});
  CHECK_EQ(target.width, 500);
  CHECK_NEAR(target.height, 281.25, 0.00001);
  for (int frame = 0; frame <= 165; ++frame) {
    const auto pose = spreadInterpolate(origin, target, frame / 165.0);
    CHECK_NEAR(pose.width / pose.height, 1600.0 / 900, 0.00001);
    CHECK(pose.x >= origin.x && pose.x <= target.x);
  }
  CHECK_EQ(spreadInterpolate(origin, target, 0).x, origin.x);
  CHECK_EQ(spreadInterpolate(origin, target, 1).x, target.x);
  CHECK_EQ(spreadInterpolate(origin, target, -0.2).width, origin.width);
  CHECK_EQ(spreadInterpolate(origin, target, 1.2).width, target.width);
}

UMBRIEL_TEST(mixedWindowsFitWithoutOverlapOnPortraitAndLandscapeOutputs) {
  for (const SpreadBox output :
       {SpreadBox{1080, 420, 2560, 1080}, SpreadBox{-1080, -840, 1080, 1920}, SpreadBox{0, 0, 1707, 1067}}) {
    for (const size_t count : {size_t{1}, size_t{4}, size_t{7}, size_t{30}, size_t{100}}) {
      std::vector<SpreadBox> origins;
      origins.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        origins.push_back(
            {static_cast<double>(i % 3) * 1000, static_cast<double>(i) * 900, i % 2 == 0 ? 1600.0 : 600.0,
             i % 2 == 0 ? 900.0 : 1300.0}
        );
      }
      const auto layout = spreadLayout(output, origins);
      CHECK_EQ(layout.cells.size(), count);
      auto indices = layout.order;
      std::ranges::sort(indices);
      for (size_t i = 0; i < count; ++i) {
        CHECK_EQ(indices[i], i);
        const auto fit = spreadFit(origins[i], layout.cells[i]);
        CHECK(fit.width > 0 && fit.height > 0);
        CHECK(fit.x >= output.x && fit.y >= output.y);
        CHECK(fit.x + fit.width <= output.x + output.width);
        CHECK(fit.y + fit.height <= output.y + output.height);
        CHECK_NEAR(fit.width / fit.height, origins[i].width / origins[i].height, 0.00001);
        for (size_t j = 0; j < i; ++j) {
          const auto other = spreadFit(origins[j], layout.cells[j]);
          CHECK(
              fit.x + fit.width <= other.x
              || other.x + other.width <= fit.x
              || fit.y + fit.height <= other.y
              || other.y + other.height <= fit.y
          );
        }
      }
      CHECK(spreadLayout(output, origins).order == layout.order);
    }
  }
  CHECK(spreadLayout({0, 0, 100, 100}, {}).cells.empty());
}

UMBRIEL_TEST(assignmentMinimizesTravelAcrossWorkspacesAndMonitors) {
  // Deliberately enumerate the lower workspace before the upper workspace, plus a second monitor to the left.
  const std::array<SpreadBox, 4> origins{
      {{1500, 2200, 700, 800}, {1120, -900, 1800, 900}, {0, 50, 900, 1500}, {2400, 500, 900, 700}}
  };
  const auto layout = spreadLayout({1080, 420, 2560, 1080}, origins);
  const auto travel = [&](const std::array<size_t, 4>& order) {
    double distance = 0;
    for (size_t i = 0; i < order.size(); ++i) {
      const auto& cell = layout.cells[order[i]];
      distance += std::hypot(
          origins[i].x + origins[i].width / 2 - cell.x - cell.width / 2,
          origins[i].y + origins[i].height / 2 - cell.y - cell.height / 2
      );
    }
    return distance;
  };
  std::array<size_t, 4> permutation{0, 1, 2, 3};
  const double chosen = travel(permutation);
  do {
    CHECK(chosen <= travel(permutation) + 0.00001);
  } while (std::ranges::next_permutation(permutation).found);
  CHECK(layout.cells[1].y <= layout.cells[0].y);
  const std::array<SpreadBox, 4> vertical{
      {{300, 2200, 1000, 600}, {300, -900, 1000, 600}, {0, 200, 1000, 600}, {1000, 200, 1000, 600}}
  };
  const auto twoRows = spreadLayout({0, 0, 1920, 1080}, vertical);
  CHECK_EQ(twoRows.columns, size_t{2});
  CHECK(twoRows.cells[1].y < twoRows.cells[0].y);
}

UMBRIEL_TEST(monitorModeDistributesOrGathersWithoutChangingWindowMembership) {
  const std::array<SpreadBox, 3> outputs{{{-1080, -300, 1080, 1920}, {0, 0, 1920, 1080}, {1920, 0, 1200, 900}}};
  // Include virtual origins from hidden workspaces: destination follows ownership, not the nearest screen.
  const std::array<SpreadBox, 4> origins{
      {{-1050, -2100, 900, 1700}, {80, 2200, 1400, 900}, {-1050, 1650, 900, 1500}, {40, -1000, 1700, 900}}
  };
  const std::array<size_t, 4> owners{0, 1, 0, 1};
  for (bool gather : {false, true}) {
    for (size_t invoking : {size_t{0}, size_t{1}}) {
      const auto layout = umbriel::spreadLayoutOnOutputs(outputs, origins, owners, invoking, gather);
      CHECK_EQ(layout.cells.size(), origins.size());
      auto order = layout.order;
      std::ranges::sort(order);
      CHECK(order == std::vector<size_t>({0, 1, 2, 3}));
      for (size_t i = 0; i < origins.size(); ++i) {
        const auto& output = outputs[gather ? invoking : owners[i]];
        const auto box = spreadFit(origins[i], layout.cells[i]);
        CHECK(box.x >= output.x && box.y >= output.y);
        CHECK(box.x + box.width <= output.x + output.width);
        CHECK(box.y + box.height <= output.y + output.height);
        CHECK_NEAR(box.width / box.height, origins[i].width / origins[i].height, 0.00001);
      }
    }
  }
  CHECK(umbriel::spreadLayoutOnOutputs(outputs, {}, {}, 0, false).order.empty());
}

UMBRIEL_TEST(spatialNavigationCrossesMonitorsAndStopsAtDesktopEdges) {
  const std::array<SpreadBox, 4> cells{
      {{-900, -500, 600, 500}, {-900, 200, 600, 500}, {150, 200, 600, 500}, {950, 200, 600, 500}}
  };
  CHECK_EQ(umbriel::spreadNeighbor(cells, 0, 0, 1), size_t{1});
  CHECK_EQ(umbriel::spreadNeighbor(cells, 1, 1, 0), size_t{2});
  CHECK_EQ(umbriel::spreadNeighbor(cells, 2, 1, 0), size_t{3});
  CHECK_EQ(umbriel::spreadNeighbor(cells, 2, -1, 0), size_t{1});
  CHECK_EQ(umbriel::spreadNeighbor(cells, 3, 1, 0), size_t{3});
  CHECK_EQ(umbriel::spreadNeighbor(cells, 1, 0, -1), size_t{0});
}

UMBRIEL_TEST(sizeLimitAndCaptionReservationAffectFit) {
  const SpreadBox origin{0, 0, 800, 600};
  const auto small = spreadFit(origin, {0, 0, 1600, 1200}, 0.5);
  CHECK_EQ(small.width, 400);
  CHECK_EQ(small.height, 300);
  const std::array<SpreadBox, 1> origins{{{0, 0, 2400, 1800}}};
  const auto withCaption = spreadLayout({0, 0, 1200, 900}, origins);
  const auto withoutCaption = spreadLayout({0, 0, 1200, 900}, origins, {.caption = false});
  CHECK(withoutCaption.cells[0].height > withCaption.cells[0].height);
}

UMBRIEL_TEST(insetWallpaperUsesPanelWorkAreaWithoutMovingWindows) {
  const SpreadBox output{-1080, 420, 1920, 1080};
  // A left panel and a top bar both shift the visual center.
  const SpreadBox usable{-1000, 460, 1840, 1040};
  const auto atStart = umbriel::spreadWallpaperBox(output, usable, {}, 0);
  CHECK_EQ(atStart.x, output.x);
  CHECK_EQ(atStart.width, output.width);
  const auto empty = umbriel::spreadWallpaperBox(output, usable, {}, 1);
  CHECK_NEAR(empty.x + empty.width / 2, usable.x + usable.width / 2, 0.00001);
  CHECK_NEAR(empty.y + empty.height / 2, usable.y + usable.height / 2, 0.00001);
  CHECK_NEAR(empty.width, usable.width * 0.70, 0.00001);
  const std::array<SpreadBox, 2> windows{{{-920, 570, 810, 700}, {20, 570, 680, 700}}};
  const auto filled = umbriel::spreadWallpaperBox(output, usable, windows, 1);
  for (const auto& window : windows) {
    CHECK(filled.x <= window.x - 16);
    CHECK(filled.y <= window.y - 16);
    CHECK(filled.x + filled.width >= window.x + window.width + 16);
    CHECK(filled.y + filled.height >= window.y + window.height + 16);
  }
  CHECK(filled.x >= usable.x && filled.x + filled.width <= usable.x + usable.width);
  const auto halfway = umbriel::spreadWallpaperBox(output, usable, windows, 0.5);
  CHECK_NEAR(halfway.x, (output.x + filled.x) / 2, 0.00001);
  CHECK_NEAR(halfway.height, (output.height + filled.height) / 2, 0.00001);
  CHECK_EQ(umbriel::spreadWallpaperBox(output, usable, windows, 2).width, filled.width);
  CHECK_EQ(umbriel::spreadWallpaperBox(output, usable, windows, -1).width, output.width);
}

int main() { return RUN_TESTS(); }
