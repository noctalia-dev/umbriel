#include "check.h"
#include "config/config.h"
#include "layout/master.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <algorithm>
#include <optional>
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}
// clang-format on

using umbriel::Layout;
using umbriel::MasterStackLayout;
using umbriel::ResolvedLayoutConfig;
using umbriel::View;

namespace {

  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x3000 + (id * 0x10))); }

  ResolvedLayoutConfig masterConfig() {
    ResolvedLayoutConfig config;
    config.mode = umbriel::LayoutMode::Master;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    config.master.position = umbriel::MasterPosition::Left;
    config.master.defaultWidthFraction = 0.55;
    config.master.newOnTop = true;
    return config;
  }

  constexpr wlr_box kUsable{0, 0, 1280, 720};

  struct Fixture {
    ResolvedLayoutConfig config = masterConfig();
    MasterStackLayout layout;

    Fixture() { layout.setConfig(&config); }

    void addViews(int count) {
      for (int i = 0; i < count; ++i) {
        layout.insertView(stub(i), i);
      }
    }

    [[nodiscard]] size_t viewCount() const {
      size_t count = 0;
      for (const auto& column : layout.columns()) {
        count += column.views.size();
      }
      return count;
    }
  };

} // namespace

UMBRIEL_TEST(firstViewFillsTheContentArea) {
  Fixture fixture;
  fixture.addViews(1);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.x, 10);
  CHECK_EQ(box.y, 10);
  CHECK_EQ(box.width, 1260);
  CHECK_EQ(box.height, 700);
}

UMBRIEL_TEST(secondViewCreatesTheStack) {
  Fixture fixture;
  fixture.addViews(2);
  fixture.layout.arrange(kUsable);
  const wlr_box master = fixture.layout.targetBox(stub(0));
  const wlr_box stack = fixture.layout.targetBox(stub(1));
  CHECK_EQ(master.x, 10);
  CHECK_EQ(master.width, 686);
  CHECK_EQ(master.height, 700);
  CHECK_EQ(stack.x, 708);
  CHECK_EQ(stack.width, 562);
  CHECK_EQ(stack.height, 700);
}

UMBRIEL_TEST(newViewsJoinTheTopOfTheStack) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(2)).y < fixture.layout.targetBox(stub(1)).y);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
}

UMBRIEL_TEST(newViewsJoinTheBottomOfTheStackWhenConfigured) {
  Fixture fixture;
  fixture.config.master.newOnTop = false;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(2)).y > fixture.layout.targetBox(stub(1)).y);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
}

UMBRIEL_TEST(stackRowsSplitTheHeightWithGaps) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  const wlr_box top = fixture.layout.targetBox(stub(2));
  const wlr_box bottom = fixture.layout.targetBox(stub(1));
  CHECK_EQ(top.y, 10);
  CHECK_EQ(top.height, 344);
  CHECK_EQ(bottom.y, 366);
  CHECK_EQ(bottom.height, 344);
  CHECK_EQ(bottom.y - (top.y + top.height), 12);
}

UMBRIEL_TEST(setHeightFractionUpdatesAreaAndColumnWeights) {
  Fixture fixture;
  fixture.addViews(3);
  CHECK(fixture.layout.setHeightFraction(stub(2), 0.7));
  CHECK(std::fabs(fixture.layout.heightFraction(stub(2)) - 0.7) < 1e-9);

  const auto& weights = fixture.layout.columns()[1].heightWeights;
  CHECK_EQ(weights.size(), size_t{2});
  CHECK(std::fabs(weights[0] / (weights[0] + weights[1]) - 0.7) < 1e-9);
}

UMBRIEL_TEST(setHeightFractionRejectsAViewAloneInItsArea) {
  Fixture fixture;
  fixture.addViews(3);
  CHECK(!fixture.layout.setHeightFraction(stub(0), 0.7));
  CHECK(std::fabs(fixture.layout.heightFraction(stub(0)) - 1.0) < 1e-9);
}

UMBRIEL_TEST(insertIsIdempotentPerView) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.insertViewIntoColumn(stub(1), 1, 0);
  CHECK_EQ(fixture.viewCount(), size_t{3});
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
}

UMBRIEL_TEST(unknownViewHasNoColumn) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK_EQ(fixture.layout.columnOf(stub(99)), -1);
  CHECK_EQ(fixture.layout.rowOf(stub(99)), -1);
}

UMBRIEL_TEST(arrangeOnAnEmptyLayoutIsHarmless) {
  Fixture fixture;
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.columns().empty());
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 0);
}

UMBRIEL_TEST(removingAnUnknownViewIsHarmless) {
  Fixture fixture;
  fixture.addViews(2);
  fixture.layout.removeView(stub(99));
  fixture.layout.removeView(nullptr);
  CHECK_EQ(fixture.viewCount(), size_t{2});
}

UMBRIEL_TEST(removingTheLastMasterViewPromotesTheStackTop) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.removeView(stub(0));
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
}

UMBRIEL_TEST(expellingEveryMasterViewLeavesAFullWidthStack) {
  Fixture fixture;
  fixture.addViews(1);
  CHECK(fixture.layout.expel(stub(0), 1));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 1260);
}

UMBRIEL_TEST(consumeLeftPullsAWindowIntoTheMaster) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);
}

UMBRIEL_TEST(consumeRightMovesAMasterWindowIntoTheStack) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.consume(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 1);
}

UMBRIEL_TEST(aWindowOpenedWithAnEmptyMasterBecomesMaster) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.expel(stub(0), 1));
  fixture.layout.insertView(stub(2), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
}

UMBRIEL_TEST(positionRightMirrorsTheAreas) {
  Fixture fixture;
  fixture.config.master.position = umbriel::MasterPosition::Right;
  fixture.addViews(2);
  fixture.layout.arrange(kUsable);
  const wlr_box stack = fixture.layout.targetBox(stub(1));
  const wlr_box master = fixture.layout.targetBox(stub(0));
  CHECK_EQ(stack.x, 10);
  CHECK_EQ(stack.width, 562);
  CHECK_EQ(master.x, 584);
  CHECK_EQ(master.width, 686);
}

UMBRIEL_TEST(consumeUsesVisualDirectionWhenMasterIsRight) {
  Fixture fixture;
  fixture.config.master.position = umbriel::MasterPosition::Right;
  fixture.addViews(2);
  CHECK(fixture.layout.consume(stub(0), -1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 1);
}

UMBRIEL_TEST(widthFractionIsComplementaryAcrossColumns) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(std::abs(fixture.layout.widthFraction(0) - 0.55) < 1e-9);
  CHECK(std::abs(fixture.layout.widthFraction(1) - 0.45) < 1e-9);
  CHECK(std::abs(fixture.layout.widthFraction(0) + fixture.layout.widthFraction(1) - 1.0) < 1e-9);
}

UMBRIEL_TEST(setWidthFractionOnTheStackAdjustsTheComplement) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.setWidthFraction(1, 0.4));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 0.6) < 1e-9);
  CHECK(std::abs(fixture.layout.widthFraction(1) - 0.4) < 1e-9);
}

UMBRIEL_TEST(cycleWidthWalksThePresets) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.cycleWidth(0, 1));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 2.0 / 3.0) < 1e-9);
  CHECK(fixture.layout.cycleWidth(0, 1));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 1.0 / 3.0) < 1e-9);
}

UMBRIEL_TEST(toggleFullWidthExpandsAndRestores) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.toggleFullWidth(0));
  CHECK(fixture.layout.isFullWidth(0));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 0.9) < 1e-9);
  CHECK(!fixture.layout.toggleFullWidth(0));
  CHECK(!fixture.layout.isFullWidth(0));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 0.55) < 1e-9);
}

UMBRIEL_TEST(widthActionsFailWithASingleArea) {
  Fixture fixture;
  fixture.addViews(1);
  CHECK(!fixture.layout.cycleWidth(0, 1));
  CHECK(!fixture.layout.toggleFullWidth(0));
  CHECK(!fixture.layout.setWidthFraction(0, 0.7));
  CHECK(!fixture.layout.isFullWidth(0));
  CHECK(std::abs(fixture.layout.widthFraction(0) - 1.0) < 1e-9);
}

UMBRIEL_TEST(moveViewVerticalSwapsStackNeighbors) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.moveViewVertical(stub(1), -1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).y, 10);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
}

UMBRIEL_TEST(moveColumnSwapsTheAreas) {
  Fixture fixture;
  fixture.addViews(2);
  fixture.layout.moveColumn(0, 1);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 1);
}

UMBRIEL_TEST(directionalFocusCrossesTheMasterStackBoundary) {
  Fixture fixture;
  fixture.config.totalGap = 11;
  fixture.addViews(4);
  fixture.layout.arrange(kUsable);
  const auto right = fixture.layout.focusHorizontalLeaf(stub(0), 1);
  CHECK(right.has_value());
  CHECK_EQ(*right, stub(2));
  const auto left = fixture.layout.focusHorizontalLeaf(stub(2), -1);
  CHECK(left.has_value());
  CHECK_EQ(*left, stub(0));
}

UMBRIEL_TEST(initialSizeMatchesTheArrangeThatFollows) {
  Fixture fixture;
  for (int id = 0; id < 3; ++id) {
    const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, 0.25, nullptr);
    fixture.layout.insertView(stub(id), id);
    fixture.layout.arrange(kUsable);
    const wlr_box arranged = fixture.layout.targetBox(stub(id));
    CHECK_EQ(initial.width, arranged.width);
    CHECK_EQ(initial.height, arranged.height);
  }
}

UMBRIEL_TEST(onlyTheBoundaryAndInnerRowEdgesAreResizable) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  const uint32_t all = WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
  CHECK_EQ(fixture.layout.sanitizeResizeEdges(stub(0), all), static_cast<uint32_t>(WLR_EDGE_RIGHT));
  CHECK_EQ(fixture.layout.sanitizeResizeEdges(stub(2), all), static_cast<uint32_t>(WLR_EDGE_LEFT | WLR_EDGE_BOTTOM));
  CHECK_EQ(fixture.layout.sanitizeResizeEdges(stub(1), all), static_cast<uint32_t>(WLR_EDGE_LEFT | WLR_EDGE_TOP));
}

UMBRIEL_TEST(aLoneViewHasNoResizableEdges) {
  Fixture fixture;
  fixture.addViews(1);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.sanitizeResizeEdges(stub(0), WLR_EDGE_LEFT | WLR_EDGE_RIGHT), uint32_t{0});
}

UMBRIEL_TEST(aResizeGrabMovesTheMasterBoundary) {
  Fixture fixture;
  fixture.addViews(2);
  fixture.layout.arrange(kUsable);
  const int before = fixture.layout.targetBox(stub(0)).width;
  auto resize = fixture.layout.beginResize(stub(0), WLR_EDGE_RIGHT, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(100.0, 0.0, kUsable);
  fixture.layout.arrange(kUsable);
  CHECK(std::abs(fixture.layout.targetBox(stub(0)).width - before - 100) <= 1);
}

UMBRIEL_TEST(aVerticalGrabTransfersHeightBetweenRows) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  const int topBefore = fixture.layout.targetBox(stub(2)).height;
  const int bottomBefore = fixture.layout.targetBox(stub(1)).height;
  auto resize = fixture.layout.beginResize(stub(2), WLR_EDGE_BOTTOM, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(0.0, 40.0, kUsable);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(2)).height > topBefore);
  CHECK(fixture.layout.targetBox(stub(1)).height < bottomBefore);
}

UMBRIEL_TEST(snapshotRestoresAreasRowsAndWidthState) {
  Fixture source;
  source.addViews(4);
  CHECK(source.layout.consume(stub(2), -1));
  source.layout.arrange(kUsable);
  CHECK(source.layout.moveViewVertical(stub(2), -1));
  source.layout.arrange(kUsable);

  auto resize = source.layout.beginResize(stub(2), WLR_EDGE_BOTTOM, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(0.0, 55.0, kUsable);
  const int masterColumn = source.layout.columnOf(stub(2));
  CHECK(source.layout.setWidthFraction(masterColumn, 0.70));
  CHECK(source.layout.toggleFullWidth(masterColumn));
  source.layout.arrange(kUsable);

  const auto capture = source.layout.captureState();
  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, capture.members));
  restored.layout.arrange(kUsable);

  CHECK_EQ(restored.layout.columns().size(), source.layout.columns().size());
  for (size_t column = 0; column < source.layout.columns().size(); ++column) {
    CHECK_EQ(restored.layout.columns()[column].views, source.layout.columns()[column].views);
    CHECK_EQ(restored.layout.columns()[column].heightWeights, source.layout.columns()[column].heightWeights);
  }
  for (int id = 0; id < 4; ++id) {
    const wlr_box expected = source.layout.targetBox(stub(id));
    const wlr_box actual = restored.layout.targetBox(stub(id));
    CHECK_EQ(actual.x, expected.x);
    CHECK_EQ(actual.y, expected.y);
    CHECK_EQ(actual.width, expected.width);
    CHECK_EQ(actual.height, expected.height);
  }

  const int restoredMaster = restored.layout.columnOf(stub(2));
  CHECK(!restored.layout.toggleFullWidth(restoredMaster));
  CHECK(std::fabs(restored.layout.widthFraction(restoredMaster) - 0.70) < 1e-9);
}

UMBRIEL_TEST(snapshotPromotesTheStackWhenTheMasterIsMissing) {
  Fixture source;
  source.addViews(3);
  const auto capture = source.layout.captureState();
  auto survivors = capture.members;
  std::erase_if(survivors, [](const auto& member) { return member.view == stub(0); });

  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, survivors));
  CHECK_EQ(restored.layout.columns().size(), size_t{2});
  CHECK_EQ(restored.layout.columnOf(stub(2)), 0);
  CHECK_EQ(restored.layout.columnOf(stub(1)), 1);
}

UMBRIEL_TEST(snapshotUsesMemberIdsInsteadOfCapturedViewPointers) {
  Fixture source;
  source.addViews(3);
  CHECK(source.layout.consume(stub(2), -1));
  const auto capture = source.layout.captureState();
  auto remapped = capture.members;
  for (auto& member : remapped) {
    member.view = stub(10 + static_cast<int>(member.id));
  }
  std::ranges::reverse(remapped);

  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, remapped));
  CHECK_EQ(restored.layout.columns().size(), size_t{2});
  CHECK_EQ(restored.layout.columns()[0].views[0], stub(10));
  CHECK_EQ(restored.layout.columns()[0].views[1], stub(11));
  CHECK_EQ(restored.layout.columns()[1].views[0], stub(12));
}

UMBRIEL_TEST(promoteFromStackMovesTheTopViewAndItsWeight) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  auto resize = fixture.layout.beginResize(stub(2), WLR_EDGE_BOTTOM, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(0.0, 60.0, kUsable);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(2)).height > fixture.layout.targetBox(stub(1)).height);

  CHECK(fixture.layout.promoteFromStack());
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
  CHECK(fixture.layout.columns()[0].heightWeights[1] > fixture.layout.columns()[0].heightWeights[0]);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
  CHECK(fixture.layout.promoteFromStack());
  CHECK(!fixture.layout.promoteFromStack());
}

UMBRIEL_TEST(promotingTheLastStackViewMakesOneFullWidthMasterArea) {
  Fixture fixture;
  fixture.addViews(2);
  CHECK(fixture.layout.promoteFromStack());
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 1260);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).width, 1260);
}

UMBRIEL_TEST(demoteToStackMovesTheLastMasterToTheStackTop) {
  Fixture fixture;
  fixture.addViews(3);
  CHECK(fixture.layout.promoteFromStack());
  CHECK(fixture.layout.demoteToStack());
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);
  CHECK(!fixture.layout.demoteToStack());
}

UMBRIEL_TEST(swapViewsWithinAnAreaKeepsGeometryWithTheSlots) {
  Fixture fixture;
  fixture.addViews(3);
  fixture.layout.arrange(kUsable);
  auto resize = fixture.layout.beginResize(stub(2), WLR_EDGE_BOTTOM, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(0.0, 60.0, kUsable);
  fixture.layout.arrange(kUsable);
  const wlr_box firstSlot = fixture.layout.targetBox(stub(2));
  const wlr_box secondSlot = fixture.layout.targetBox(stub(1));

  CHECK(fixture.layout.swapViews(stub(2), stub(1)));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).y, firstSlot.y);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).height, firstSlot.height);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.targetBox(stub(2)).y, secondSlot.y);
  CHECK_EQ(fixture.layout.targetBox(stub(2)).height, secondSlot.height);
}

UMBRIEL_TEST(swapViewsAcrossAreasExchangesMembership) {
  Fixture fixture;
  fixture.addViews(3);
  CHECK(fixture.layout.swapViews(stub(0), stub(2)));
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 0);
  CHECK(!fixture.layout.swapViews(stub(0), stub(99)));
  CHECK(!fixture.layout.swapViews(stub(0), stub(0)));
}

int main() { return RUN_TESTS(); }
