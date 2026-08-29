#include "check.h"
#include "config/config.h"
#include "layout/scrolling.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <algorithm>
#include <optional>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
// clang-format on

using umbriel::Column;
using umbriel::Layout;
using umbriel::LayoutConstraints;
using umbriel::ResolvedLayoutConfig;
using umbriel::ScrollingDirection;
using umbriel::ScrollingLayout;
using umbriel::View;

namespace {

  // The layout treats View as an opaque identity, so tests never need a real
  // one. These are addresses, never dereferenced.
  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x1000 + (id * 0x10))); }

  // Mirrors the shipped defaults except expandSingleColumn: geometry tests pin
  // false so they exercise the resting-width math, and the expand_single_column
  // behavior is covered by its own tests that flip the value back to true.
  //   totalGap = gap + 2 * border = 12,  edgePad = gap + border = 10
  ResolvedLayoutConfig defaultConfig() {
    ResolvedLayoutConfig config;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    config.scrolling.defaultWidthFraction = 0.5;
    config.scrolling.centerUnderfullStrip = true;
    config.scrolling.expandSingleColumn = false;
    config.widthPresets = {1.0 / 3, 0.5, 2.0 / 3};
    return config;
  }

  constexpr int kViewport = 1260;        // 1280 output minus 2 * edgePad
  constexpr int kVerticalViewport = 700; // 720 output minus 2 * edgePad
  constexpr wlr_box kUsable{0, 0, 1280, 720};

  // Fixture holding the config alive: Layout keeps a raw pointer to it.
  struct Fixture {
    ResolvedLayoutConfig config = defaultConfig();
    ScrollingLayout layout;

    explicit Fixture(ScrollingDirection direction = ScrollingDirection::Horizontal) {
      config.scrolling.direction = direction;
      layout.setConfig(&config);
    }

    void addColumns(int count) {
      for (int i = 0; i < count; ++i) {
        layout.insertView(stub(i), i);
      }
    }
  };

} // namespace

// column bookkeeping
UMBRIEL_TEST(insertAndRemoveTracksColumns) {
  Fixture fixture;
  fixture.addColumns(3);
  CHECK_EQ(fixture.layout.columns().size(), size_t{3});
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 2);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);

  fixture.layout.removeView(stub(1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columnOf(stub(1)), -1);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 1);
}

UMBRIEL_TEST(insertIsIdempotentPerView) {
  Fixture fixture;
  fixture.layout.insertView(stub(0), 0);
  fixture.layout.insertView(stub(0), 0);
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
}

UMBRIEL_TEST(unknownViewHasNoColumnOrRow) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK_EQ(fixture.layout.columnOf(stub(99)), -1);
  CHECK_EQ(fixture.layout.rowOf(stub(99)), -1);
  CHECK_EQ(fixture.layout.columnOf(nullptr), -1);
}

UMBRIEL_TEST(moveColumnReorders) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.moveColumn(0, 2);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 2);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 1);
}

// consume and expel
UMBRIEL_TEST(consumeLeftStacksIntoThePreviousColumn) {
  Fixture fixture;
  fixture.addColumns(2);

  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);
  // The emptied source column is dropped, not left behind.
  CHECK_EQ(fixture.layout.columns()[0].views.size(), size_t{2});
  CHECK_EQ(fixture.layout.columns()[0].heightWeights.size(), size_t{2});
}

UMBRIEL_TEST(consumeLeftFailsOnTheFirstColumn) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(!fixture.layout.consume(stub(0), -1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
}

UMBRIEL_TEST(consumeRightStacksIntoTheNextColumn) {
  Fixture fixture;
  fixture.addColumns(2);

  CHECK(fixture.layout.consume(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 1);
}

UMBRIEL_TEST(expelRightUndoesConsumeLeft) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK(fixture.layout.expel(stub(1), 1));

  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
}

UMBRIEL_TEST(expelLeftUndoesConsumeRight) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(0), 1));
  CHECK(fixture.layout.expel(stub(0), -1));

  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
}

UMBRIEL_TEST(expelRightFailsOnASingleViewColumn) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(!fixture.layout.expel(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
}

// height math
UMBRIEL_TEST(setHeightFractionResizesAStackedRow) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));

  CHECK(fixture.layout.setHeightFraction(stub(1), 0.7));
  CHECK(std::fabs(fixture.layout.heightFraction(stub(1)) - 0.7) < 1e-9);
  fixture.layout.arrange(kUsable);

  const int stackCross = kUsable.height - 2 * fixture.config.edgePad - fixture.config.totalGap;
  CHECK(std::abs(fixture.layout.targetBox(stub(1)).height - static_cast<int>(std::lround(0.7 * stackCross))) <= 1);
}

UMBRIEL_TEST(setHeightFractionRejectsASoloRow) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(!fixture.layout.setHeightFraction(stub(0), 0.7));
  CHECK(std::fabs(fixture.layout.heightFraction(stub(0)) - 1.0) < 1e-9);
}

UMBRIEL_TEST(moveViewVerticalReordersWithinAColumn) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);

  CHECK(fixture.layout.moveViewVertical(stub(1), -1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 1);

  // Already at the top edge.
  CHECK(!fixture.layout.moveViewVertical(stub(1), -1));
}

UMBRIEL_TEST(swapViewsAcrossColumnsKeepsGeometryWithTheSlots) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3.0));
  CHECK(fixture.layout.setWidthFraction(1, 2.0 / 3.0));
  fixture.layout.arrange(kUsable);
  const wlr_box firstSlot = fixture.layout.targetBox(stub(0));
  const wlr_box secondSlot = fixture.layout.targetBox(stub(1));

  CHECK(fixture.layout.swapViews(stub(0), stub(1)));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).x, firstSlot.x);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).width, firstSlot.width);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 1);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).x, secondSlot.x);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, secondSlot.width);
}

// width math
UMBRIEL_TEST(halfWidthColumnMatchesTheGapAwareFormula) {
  Fixture fixture;
  fixture.addColumns(1);
  // round(0.5 * (1260 + 12)) - 12 = 636 - 12 = 624
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 624);
}

// expand_single_column
UMBRIEL_TEST(expandSingleColumnFalseKeepsTheConfiguredWidth) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 624);
}

UMBRIEL_TEST(expandSingleColumnTrueFillsALoneColumn) {
  Fixture fixture;
  fixture.config.scrolling.expandSingleColumn = true;
  fixture.addColumns(1);
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport);
}

UMBRIEL_TEST(expandSingleColumnTrueHonorsClientMaxWidth) {
  Fixture fixture;
  fixture.config.scrolling.expandSingleColumn = true;
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.maxWidth = 300}; });
  fixture.addColumns(1);
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 300);
}

UMBRIEL_TEST(expandSingleColumnTrueSizesTheFirstConfigureFull) {
  Fixture fixture;
  fixture.config.scrolling.expandSingleColumn = true;
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, std::nullopt, nullptr);
  CHECK_EQ(initial.width, 1260);
  CHECK_EQ(initial.height, 700);
}

UMBRIEL_TEST(expandSingleColumnTrueReexpandsTheLastSurvivor) {
  Fixture fixture;
  fixture.config.scrolling.expandSingleColumn = true;
  fixture.addColumns(2);
  fixture.layout.removeView(stub(1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport);
}

UMBRIEL_TEST(twoHalfColumnsTileExactlyAcrossTheViewport) {
  Fixture fixture;
  fixture.addColumns(2);
  const int a = fixture.layout.columnWidth(0, kViewport);
  const int b = fixture.layout.columnWidth(1, kViewport);
  // Sum of widths plus the single inter-column gap fills the viewport exactly.
  CHECK_EQ(a + b + fixture.config.totalGap, kViewport);
}

UMBRIEL_TEST(threeThirdColumnsTileExactlyAcrossTheViewport) {
  Fixture fixture;
  fixture.addColumns(3);
  for (int i = 0; i < 3; ++i) {
    CHECK(fixture.layout.setWidthFraction(i, 1.0 / 3));
  }
  int total = 0;
  for (int i = 0; i < 3; ++i) {
    total += fixture.layout.columnWidth(i, kViewport);
  }
  // Two inter-column gaps for three columns.
  CHECK_EQ(total + 2 * fixture.config.totalGap, kViewport);
}

UMBRIEL_TEST(columnXAccumulatesWidthsAndGaps) {
  Fixture fixture;
  fixture.addColumns(2);
  const int first = fixture.layout.columnWidth(0, kViewport);
  CHECK_EQ(fixture.layout.columnX(0, kViewport), 0);
  CHECK_EQ(fixture.layout.columnX(1, kViewport), first + fixture.config.totalGap);
}

UMBRIEL_TEST(aNarrowRowIsCentered) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  const int width = fixture.layout.columnWidth(0, kViewport);
  // Leftover space is split evenly rather than piled on the right.
  CHECK_EQ(fixture.layout.columnX(0, kViewport), (kViewport - width) / 2);
}

UMBRIEL_TEST(disablingUnderfullCenteringLeftAlignsTheWholeStrip) {
  Fixture fixture;
  fixture.config.scrolling.centerUnderfullStrip = false;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  CHECK(fixture.layout.setWidthFraction(1, 1.0 / 3));
  CHECK_EQ(fixture.layout.columnX(0, kViewport), 0);
}

UMBRIEL_TEST(horizontalResizeRecentersAnUnderfullStripImmediately) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  CHECK(fixture.layout.setWidthFraction(1, 1.0 / 3));
  fixture.layout.arrange(kUsable);

  const int firstXBefore = fixture.layout.targetBox(stub(0)).x;
  const wlr_box secondBefore = fixture.layout.targetBox(stub(1));
  const int rightBefore = secondBefore.x + secondBefore.width;

  auto resize = fixture.layout.beginResize(stub(1), WLR_EDGE_RIGHT, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(100.0, 0.0, kUsable);
  fixture.layout.arrange(kUsable);

  const int firstXAfter = fixture.layout.targetBox(stub(0)).x;
  const wlr_box secondAfter = fixture.layout.targetBox(stub(1));
  CHECK_EQ(firstXAfter, firstXBefore - 100);
  CHECK_EQ(secondAfter.x + secondAfter.width, rightBefore + 100);
  CHECK(
      std::abs(
          (firstXAfter - fixture.config.edgePad)
          - (kUsable.width - fixture.config.edgePad - secondAfter.x - secondAfter.width)
      )
      <= 1
  );
}

UMBRIEL_TEST(middleOfPartiallyOffscreenColumnGrabsNothing) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 0.5));
  CHECK(fixture.layout.setWidthFraction(1, 0.667));
  fixture.layout.arrange(kUsable);

  CHECK_EQ(fixture.layout.resizeEdgesAt(stub(1), 1000, 360), 0U);
}

UMBRIEL_TEST(rightThirdGrabsTheRightEdgeEvenNearACorner) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 0.5));
  CHECK(fixture.layout.setWidthFraction(1, 0.667));
  fixture.layout.arrange(kUsable);

  CHECK_EQ(fixture.layout.resizeEdgesAt(stub(1), 1210, 100), static_cast<uint32_t>(WLR_EDGE_RIGHT | WLR_EDGE_TOP));
}

UMBRIEL_TEST(resizeEdgesComeFromTileThirds) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 0.5));
  CHECK(fixture.layout.setWidthFraction(1, 0.667));
  fixture.layout.arrange(kUsable);

  CHECK_EQ(fixture.layout.resizeEdgesAt(stub(1), 1220, 360), static_cast<uint32_t>(WLR_EDGE_RIGHT));
  CHECK_EQ(fixture.layout.resizeEdgesAt(stub(1), 700, 360), static_cast<uint32_t>(WLR_EDGE_LEFT));
  CHECK_EQ(fixture.layout.resizeEdgesAt(stub(1), 700, 650), static_cast<uint32_t>(WLR_EDGE_LEFT | WLR_EDGE_BOTTOM));
}

UMBRIEL_TEST(firstColumnLeftEdgeStaysFixedWithoutCentering) {
  Fixture fixed;
  fixed.config.scrolling.centerUnderfullStrip = false;
  fixed.addColumns(1);
  fixed.layout.arrange(kUsable);
  CHECK_EQ(fixed.layout.resizeEdgesAt(stub(0), 60, 360), 0U);
  CHECK_EQ(fixed.layout.resizeEdgesAt(stub(0), 60, 60), static_cast<uint32_t>(WLR_EDGE_TOP));

  Fixture centered;
  centered.addColumns(1);
  centered.layout.arrange(kUsable);
  CHECK_EQ(centered.layout.resizeEdgesAt(stub(0), 60, 360), static_cast<uint32_t>(WLR_EDGE_LEFT));
}

UMBRIEL_TEST(widthFractionsAreClamped) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 5.0));
  CHECK(fixture.layout.widthFraction(0) <= 1.0);
  CHECK(fixture.layout.setWidthFraction(0, -1.0));
  CHECK(fixture.layout.widthFraction(0) > 0.0);
  // Out-of-range column indices are rejected, not clamped into a neighbour.
  CHECK(!fixture.layout.setWidthFraction(7, 0.5));
  CHECK(!fixture.layout.setWidthFraction(-1, 0.5));
}

UMBRIEL_TEST(cycleWidthWalksThePresets) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));

  CHECK(fixture.layout.cycleWidth(0, 1));
  const double second = fixture.layout.widthFraction(0);
  CHECK(std::fabs(second - 0.5) < 1e-6);

  CHECK(fixture.layout.cycleWidth(0, 1));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 2.0 / 3) < 1e-6);

  // Wraps back to the first preset.
  CHECK(fixture.layout.cycleWidth(0, 1));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 1.0 / 3) < 1e-6);
}

UMBRIEL_TEST(cycleWidthBackWalksThePresetsInReverse) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 2.0 / 3));

  CHECK(fixture.layout.cycleWidth(0, -1));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 0.5) < 1e-6);

  CHECK(fixture.layout.cycleWidth(0, -1));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 1.0 / 3) < 1e-6);

  // Wraps back to the last preset.
  CHECK(fixture.layout.cycleWidth(0, -1));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 2.0 / 3) < 1e-6);
}

UMBRIEL_TEST(toggleFullWidthRestoresThePreviousFraction) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  const double before = fixture.layout.widthFraction(0);

  // The return value is "is now full width", not "succeeded". Unlike
  // setWidthFraction, an out-of-range index and a restore both return false.
  CHECK(fixture.layout.toggleFullWidth(0));
  CHECK(fixture.layout.isFullWidth(0));
  CHECK(fixture.layout.widthFraction(0) > before);

  CHECK(!fixture.layout.toggleFullWidth(0));
  CHECK(!fixture.layout.isFullWidth(0));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - before) < 1e-6);

  CHECK(!fixture.layout.toggleFullWidth(9));
}

UMBRIEL_TEST(clearFullWidthStateDropsTheRestorePoint) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.toggleFullWidth(0));
  CHECK(fixture.layout.isFullWidth(0));
  fixture.layout.clearFullWidthState(0);
  CHECK(!fixture.layout.isFullWidth(0));
}

// constraints
UMBRIEL_TEST(clientMinWidthWidensAColumn) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  const int unconstrained = fixture.layout.columnWidth(0, kViewport);

  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.minWidth = 900}; });
  CHECK(fixture.layout.columnWidth(0, kViewport) > unconstrained);
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 900);
}

UMBRIEL_TEST(clientMaxWidthNarrowsAColumn) {
  Fixture fixture;
  fixture.addColumns(1);
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.maxWidth = 300}; });
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 300);
}

UMBRIEL_TEST(constraintsNeverExceedTheViewport) {
  Fixture fixture;
  fixture.addColumns(1);
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.minWidth = 99999}; });
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport);
}

UMBRIEL_TEST(fullscreenColumnFillsTheUsableAreaIgnoringFractions) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  fixture.layout.setConstraints([](const View* view) { return LayoutConstraints{.fullscreen = view == stub(0)}; });
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport + 2 * fixture.config.edgePad);
  CHECK(fixture.layout.isFullWidth(0));
  // The other column is untouched.
  CHECK(!fixture.layout.isFullWidth(1));
}

UMBRIEL_TEST(maximizedToEdgesColumnFillsTheUsableAreaIgnoringFractions) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  fixture.layout.setConstraints([](const View* view) {
    return LayoutConstraints{.maximizedToEdges = view == stub(0)};
  });
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport + 2 * fixture.config.edgePad);
  CHECK(fixture.layout.isFullWidth(0));
  CHECK(!fixture.layout.isFullWidth(1));
}

UMBRIEL_TEST(preservedFullWidthSurvivesEdgeMaximizeToggle) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 0.5));
  CHECK(fixture.layout.toggleFullWidth(0));

  fixture.layout.setConstraints([](const View* view) {
    return LayoutConstraints{.maximizedToEdges = view == stub(0)};
  });
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport + 2 * fixture.config.edgePad);

  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{}; });
  CHECK(fixture.layout.isFullWidth(0));
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), kViewport);
  CHECK(!fixture.layout.toggleFullWidth(0));
  CHECK(std::fabs(fixture.layout.widthFraction(0) - 0.5) < 1e-6);
}

UMBRIEL_TEST(unsetConstraintsMeanUnconstrained) {
  Fixture fixture;
  fixture.addColumns(1);
  // No setConstraints call at all: the layout must still produce sane geometry.
  CHECK_EQ(fixture.layout.columnWidth(0, kViewport), 624);
  CHECK(!fixture.layout.isFullWidth(0));
}

// initial sizing
UMBRIEL_TEST(initialSizeMatchesWhatArrangeWillAssign) {
  // The invariant: the size a view is configured with before it joins the layout must equal the size the layout gives
  // it once it has. Any drift and the client's first buffer is wrong and the window resizes on first paint.
  Fixture fixture;
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, std::nullopt, nullptr);

  fixture.addColumns(1);
  fixture.layout.arrange(kUsable);
  const wlr_box arranged = fixture.layout.targetBox(stub(0));

  CHECK_EQ(initial.width, arranged.width);
  CHECK_EQ(initial.height, arranged.height);
}

UMBRIEL_TEST(initialSizeHonoursARuleWidthFraction) {
  Fixture fixture;
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, 1.0 / 3, nullptr);

  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFraction(0, 1.0 / 3));
  fixture.layout.arrange(kUsable);

  CHECK_EQ(initial.width, fixture.layout.targetBox(stub(0)).width);
  CHECK(initial.width < fixture.layout.initialSize(kUsable, std::nullopt, nullptr).width);
}

UMBRIEL_TEST(initialSizeUsesTheDefaultFractionWhenNoRuleApplies) {
  Fixture fixture;
  CHECK_EQ(fixture.layout.initialSize(kUsable, std::nullopt, nullptr).width, 624);
  CHECK_EQ(fixture.layout.initialSize(kUsable, std::nullopt, nullptr).height, 700);
}

UMBRIEL_TEST(initialSizeLeavesTheScrollAxisUnconstrainedWhenNoDefaultIsSet) {
  Fixture horizontal;
  horizontal.config.scrolling.defaultWidthFraction.reset();
  const Layout::InitialSize horizontalSize = horizontal.layout.initialSize(kUsable, std::nullopt, nullptr);
  CHECK_EQ(horizontalSize.width, 0);
  CHECK_EQ(horizontalSize.height, 700);

  Fixture vertical(ScrollingDirection::Vertical);
  vertical.config.scrolling.defaultWidthFraction.reset();
  const Layout::InitialSize verticalSize = vertical.layout.initialSize(kUsable, std::nullopt, nullptr);
  CHECK_EQ(verticalSize.width, 1260);
  CHECK_EQ(verticalSize.height, 0);
}

UMBRIEL_TEST(mappedClientWidthCanBecomeTheColumnWidth) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setWidthFromPixels(0, kViewport, 800));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 800);
}

// scrolling
UMBRIEL_TEST(maxScrollIsZeroWhileEverythingFits) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK_EQ(fixture.layout.maxScroll(kViewport), 0);
}

UMBRIEL_TEST(maxScrollGrowsWithOverflowingColumns) {
  Fixture fixture;
  fixture.addColumns(6);
  CHECK(fixture.layout.maxScroll(kViewport) > 0);
}

// re-anchoring when a column closes off-screen: Removing a column closes the space it held, so everything to its right
// moves left by its width plus a gap. When that space was off-screen to the left, the user did not ask to see it
// happen: a window closing several columns back should not shift the one being read. These pin the compensation that
// keeps the visible strip still, and the cases where there is deliberately none.

// Screen position of a column: layout coordinate minus the scroll offset.
int screenX(const ScrollingLayout& layout, int column) {
  return layout.columnX(column, kViewport) - static_cast<int>(std::lround(layout.scroll()));
}

UMBRIEL_TEST(removingAColumnLeftOfTheViewportDoesNotMoveTheVisibleOnes) {
  Fixture fixture;
  fixture.addColumns(3);
  // Scrolled fully right: column 0 (0..624) sits entirely left of the viewport,
  // which starts at 636.
  fixture.layout.setScroll(636);
  const int before = screenX(fixture.layout, 1);

  const double shift = fixture.layout.scrollShiftForColumnRemoval(0, kViewport);
  fixture.layout.removeView(stub(0));
  fixture.layout.setScroll(fixture.layout.scroll() - shift);

  // That survivor is index 0 now. It must be exactly where it was on screen.
  CHECK_EQ(screenX(fixture.layout, 0), before);
}

UMBRIEL_TEST(theShiftIsTheColumnWidthPlusOneGap) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(636);
  // 624 wide + 12 of gap: the whole span the column occupied.
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(0, kViewport), 636.0);
}

UMBRIEL_TEST(aColumnAtTheViewportEdgeIsNotCompensated) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(0);
  // Nothing of it is hidden, so the strip closing up is in plain sight and is
  // exactly what closing a visible window should look like.
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(0, kViewport), 0.0);
}

UMBRIEL_TEST(aPartlyHiddenColumnIsCompensatedByTheHiddenPart) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(300);
  // The compensation grows with how much is off-screen rather than switching on
  // at the edge, so there is no jump as the column scrolls out of view.
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(0, kViewport), 300.0);
}

UMBRIEL_TEST(aColumnRightOfTheViewportIsNotCompensated) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(0);
  // Nothing left of the viewport moves when a column further right closes.
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(2, kViewport), 0.0);
}

UMBRIEL_TEST(losingOneRowOfAStackShiftsNothing) {
  Fixture fixture;
  fixture.addColumns(3);
  // Stack view 1 onto column 0, so removing it leaves the column standing.
  fixture.layout.consume(stub(1), -1);
  fixture.layout.setScroll(636);
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(0, kViewport), 0.0);
}

UMBRIEL_TEST(anOutOfRangeColumnShiftsNothing) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(636);
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(-1, kViewport), 0.0);
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(99, kViewport), 0.0);
  CHECK_EQ(fixture.layout.scrollShiftForColumnRemoval(0, 0), 0.0);
}

UMBRIEL_TEST(setScrollStoresVerbatimAndDoesNotClamp) {
  // Pinning current behavior, not endorsing it: setScroll is a raw setter, and every caller is responsible for clamping
  // to [0, maxScroll] itself. See the hand-rolled clamp in Server::executeKeybindAction (layout-scroll-*), which exists
  // because overscroll here parks the strip past an edge and seeds sub-pixel scroll residue. The clamp belongs in the
  // layout.
  Fixture fixture;
  fixture.addColumns(6);

  fixture.layout.setScroll(-500);
  CHECK_EQ(fixture.layout.scroll(), -500.0);

  fixture.layout.setScroll(1e9);
  CHECK_EQ(fixture.layout.scroll(), 1e9);

  // maxScroll is the bound callers are expected to apply.
  CHECK(fixture.layout.maxScroll(kViewport) > 0);
}

UMBRIEL_TEST(ensureVisibleStaysWithinTheScrollRange) {
  // ensureVisible does clamp, unlike setScroll.
  Fixture fixture;
  fixture.addColumns(6);
  for (int column = 0; column < 6; ++column) {
    fixture.layout.ensureVisible(column, kViewport);
    CHECK(fixture.layout.scroll() >= 0.0);
    CHECK(fixture.layout.scroll() <= static_cast<double>(fixture.layout.maxScroll(kViewport)));
  }
}

UMBRIEL_TEST(ensureVisibleReachesBothEnds) {
  Fixture fixture;
  fixture.addColumns(6);

  fixture.layout.ensureVisible(5, kViewport);
  const double atLast = fixture.layout.scroll();
  CHECK(atLast > 0.0);

  fixture.layout.ensureVisible(0, kViewport);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
}

UMBRIEL_TEST(snapVisibleTargetsRequestedColumn) {
  Fixture fixture;
  fixture.addColumns(6);
  constexpr int column = 3;

  fixture.layout.setScroll(0);
  fixture.layout.snapVisible(column, kViewport);

  CHECK_EQ(fixture.layout.scroll(), static_cast<double>(fixture.layout.columnX(column, kViewport)));
}

UMBRIEL_TEST(snapVisibleCentersFullWidthColumn) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setConstraints([](const View* view) { return LayoutConstraints{.fullscreen = view == stub(1)}; });
  constexpr int column = 1;
  const auto expected = static_cast<double>(fixture.layout.columnX(column, kViewport) + fixture.config.edgePad);

  fixture.layout.setScroll(0);
  fixture.layout.snapVisible(column, kViewport);

  CHECK_EQ(fixture.layout.scroll(), expected);
}

UMBRIEL_TEST(centerFocusedCentersAnInteriorColumn) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.config.scrolling.centerFocused = true;

  fixture.layout.ensureVisible(1, kViewport);

  const int centeredX = fixture.layout.columnX(1, kViewport)
      + fixture.layout.columnWidth(1, kViewport) / 2
      - static_cast<int>(std::lround(fixture.layout.scroll()));
  CHECK_EQ(centeredX, kViewport / 2);
  CHECK(fixture.layout.centeredRest());
}

UMBRIEL_TEST(centerFocusedAllowsEdgeColumnsToOverscroll) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.config.scrolling.centerFocused = true;

  fixture.layout.ensureVisible(0, kViewport);
  CHECK(fixture.layout.scroll() < 0.0);

  fixture.layout.ensureVisible(2, kViewport);
  CHECK(fixture.layout.scroll() > static_cast<double>(fixture.layout.maxScroll(kViewport)));
}

UMBRIEL_TEST(disablingCenterFocusedReturnsTheFocusedColumnToTheScrollRange) {
  Fixture fixture;
  fixture.addColumns(3);
  fixture.config.scrolling.centerFocused = true;
  fixture.layout.reconcileFocusedColumn(2, kViewport);
  CHECK(fixture.layout.scroll() > static_cast<double>(fixture.layout.maxScroll(kViewport)));

  fixture.config.scrolling.centerFocused = false;
  fixture.layout.reconcileFocusedColumn(2, kViewport);

  CHECK_EQ(fixture.layout.scroll(), static_cast<double>(fixture.layout.maxScroll(kViewport)));
  CHECK(!fixture.layout.centeredRest());
}

UMBRIEL_TEST(ensureVisibleIsANoOpForAnAlreadyVisibleColumn) {
  Fixture fixture;
  fixture.addColumns(6);
  fixture.layout.setScroll(0);
  fixture.layout.ensureVisible(0, kViewport);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
  CHECK_EQ(fixture.layout.scrollAmountToEnsureVisible(0, kViewport), 0.0);
}

UMBRIEL_TEST(ensureVisibleKeepsAFlushRightColumnInPlace) {
  // Two half-width columns plus a third: the second column is fully visible flush against the viewport edge, so
  // focusing it must not nudge the strip to reveal a sliver of the third column.
  Fixture fixture;
  fixture.addColumns(3);
  fixture.layout.setScroll(0);
  CHECK_EQ(fixture.layout.columnX(1, kViewport) + fixture.layout.columnWidth(1, kViewport), kViewport);
  fixture.layout.ensureVisible(1, kViewport);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
  CHECK_EQ(fixture.layout.scrollAmountToEnsureVisible(1, kViewport), 0.0);
}

UMBRIEL_TEST(ensureVisibleKeepsAFlushLeftColumnInPlace) {
  // Mirrors the flush-right case at the other end: at max scroll the second
  // column sits flush against the left edge, still fully visible.
  Fixture fixture;
  fixture.addColumns(3);
  const double maxScroll = fixture.layout.maxScroll(kViewport);
  fixture.layout.setScroll(maxScroll);
  CHECK_EQ(fixture.layout.columnX(1, kViewport), static_cast<int>(maxScroll));
  fixture.layout.ensureVisible(1, kViewport);
  CHECK_EQ(fixture.layout.scroll(), maxScroll);
  CHECK_EQ(fixture.layout.scrollAmountToEnsureVisible(1, kViewport), 0.0);
}

UMBRIEL_TEST(centerColumnLetsTheFirstColumnRestPastStripStart) {
  Fixture fixture;
  fixture.addColumns(3);
  const double expected = static_cast<double>(fixture.layout.columnX(0, kViewport))
      - (kViewport - fixture.layout.columnWidth(0, kViewport)) / 2.0;

  CHECK(fixture.layout.centerColumn(0, kViewport));
  CHECK(expected < 0.0);
  CHECK_EQ(fixture.layout.scroll(), expected);
  CHECK(fixture.layout.centeredRest());

  fixture.layout.ensureVisible(0, kViewport);
  CHECK_EQ(fixture.layout.scroll(), expected);
  CHECK_EQ(fixture.layout.scrollAmountToEnsureVisible(0, kViewport), 0.0);
}

UMBRIEL_TEST(centerColumnLetsTheLastColumnRestPastStripEnd) {
  Fixture fixture;
  fixture.addColumns(3);
  const double expected = static_cast<double>(fixture.layout.columnX(2, kViewport))
      - (kViewport - fixture.layout.columnWidth(2, kViewport)) / 2.0;

  CHECK(fixture.layout.centerColumn(2, kViewport));
  CHECK(expected > static_cast<double>(fixture.layout.maxScroll(kViewport)));
  CHECK_EQ(fixture.layout.scroll(), expected);
  CHECK(fixture.layout.centeredRest());

  fixture.layout.ensureVisible(2, kViewport);
  CHECK_EQ(fixture.layout.scroll(), expected);
}

UMBRIEL_TEST(rawScrollDoesNotInheritAColumnCenterRest) {
  Fixture fixture;
  fixture.addColumns(3);
  CHECK(fixture.layout.centerColumn(0, kViewport));

  fixture.layout.setScroll(-126.0);
  CHECK(!fixture.layout.centeredRest());
  fixture.layout.ensureVisible(0, kViewport);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
}

UMBRIEL_TEST(revealingAHiddenColumnEndsTheCenteredRest) {
  Fixture fixture;
  fixture.addColumns(3);
  CHECK(fixture.layout.centerColumn(0, kViewport));

  fixture.layout.ensureVisible(2, kViewport);
  CHECK(!fixture.layout.centeredRest());
  CHECK(fixture.layout.scroll() >= 0.0);
  CHECK(fixture.layout.scroll() <= static_cast<double>(fixture.layout.maxScroll(kViewport)));
}

UMBRIEL_TEST(centerColumnRejectsInvalidGeometryWithoutMoving) {
  Fixture fixture;
  fixture.addColumns(2);
  fixture.layout.setScroll(123.0);

  CHECK(!fixture.layout.centerColumn(-1, kViewport));
  CHECK(!fixture.layout.centerColumn(2, kViewport));
  CHECK(!fixture.layout.centerColumn(0, 0));
  CHECK_EQ(fixture.layout.scroll(), 123.0);
  CHECK(!fixture.layout.centeredRest());
}

UMBRIEL_TEST(ensureVisibleGivesBackLeftOverscroll) {
  // Raw gesture overscroll is not a centered resting position. Even though column 0 remains fully visible,
  // ensureVisible must return the strip to its normal range.
  Fixture fixture;
  fixture.addColumns(3);
  const double overscroll = -126.0; // The gesture caps overscroll at 0.1 * viewport.
  fixture.layout.setScroll(overscroll);
  // Pin the path under test: the column is fully visible, so the reveal branch never runs.
  const auto left = static_cast<double>(fixture.layout.columnX(0, kViewport));
  const auto right =
      static_cast<double>(fixture.layout.columnX(0, kViewport) + fixture.layout.columnWidth(0, kViewport) - kViewport);
  CHECK(overscroll <= left);
  CHECK(overscroll >= right);

  fixture.layout.ensureVisible(0, kViewport);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
}

UMBRIEL_TEST(ensureVisibleGivesBackRightOverscroll) {
  // Mirrors the left extremity: raw overscroll past max scroll is still temporary, even while the last column remains
  // fully visible.
  Fixture fixture;
  fixture.addColumns(3);
  const double maxScroll = fixture.layout.maxScroll(kViewport);
  const double overscroll = maxScroll + 126.0;
  fixture.layout.setScroll(overscroll);
  const auto left = static_cast<double>(fixture.layout.columnX(2, kViewport));
  const auto right =
      static_cast<double>(fixture.layout.columnX(2, kViewport) + fixture.layout.columnWidth(2, kViewport) - kViewport);
  CHECK(overscroll <= left);
  CHECK(overscroll >= right);

  fixture.layout.ensureVisible(2, kViewport);
  CHECK_EQ(fixture.layout.scroll(), maxScroll);
}

UMBRIEL_TEST(ensureVisibleAlignsInsertedHalfWidthColumnToTheRightEdge) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.setWidthFraction(0, 1.0));
  CHECK(fixture.layout.setWidthFraction(1, 1.0));

  // A new column opens immediately after the focused left browser, placing it
  // between the two existing full-width columns.
  fixture.layout.insertView(stub(2), 1);
  fixture.layout.ensureVisible(1, kViewport);

  const int rightEdge = fixture.layout.columnX(1, kViewport)
      + fixture.layout.columnWidth(1, kViewport)
      - static_cast<int>(std::lround(fixture.layout.scroll()));
  CHECK_EQ(rightEdge, kViewport);
}

// arrange and targetBox
UMBRIEL_TEST(arrangePlacesColumnsSideBySide) {
  Fixture fixture;
  fixture.addColumns(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));

  CHECK_EQ(first.y, second.y);
  CHECK_EQ(first.height, second.height);
  CHECK_EQ(first.height, kUsable.height - 2 * fixture.config.edgePad);
  CHECK_EQ(second.x, first.x + first.width + fixture.config.totalGap);
  CHECK_EQ(first.x, kUsable.x + fixture.config.edgePad);
}

UMBRIEL_TEST(arrangeStacksRowsWithinAColumn) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  fixture.layout.arrange(kUsable);

  const wlr_box upper = fixture.layout.targetBox(stub(0));
  const wlr_box lower = fixture.layout.targetBox(stub(1));

  CHECK_EQ(upper.x, lower.x);
  CHECK_EQ(upper.width, lower.width);
  CHECK_EQ(lower.y, upper.y + upper.height + fixture.config.totalGap);
  // Equal weights split the stack evenly, within rounding.
  CHECK(std::abs(upper.height - lower.height) <= 1);
}

UMBRIEL_TEST(arrangeHonorsClientMinHeight) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.minHeight = 600}; });
  fixture.layout.arrange(kUsable);

  CHECK(fixture.layout.targetBox(stub(0)).height >= 600);
  CHECK(fixture.layout.targetBox(stub(1)).height >= 600);
}

UMBRIEL_TEST(targetBoxOfAnUnknownViewIsEmpty) {
  Fixture fixture;
  fixture.addColumns(1);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(99));
  CHECK_EQ(box.width, 0);
  CHECK_EQ(box.height, 0);
}

UMBRIEL_TEST(arrangeShiftsColumnsByTheScrollOffset) {
  Fixture fixture;
  fixture.addColumns(6);
  fixture.layout.arrange(kUsable);
  const int unscrolled = fixture.layout.targetBox(stub(0)).x;

  fixture.layout.setScroll(100);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).x, unscrolled - 100);
}

UMBRIEL_TEST(arrangeOnAnEmptyLayoutIsHarmless) {
  Fixture fixture;
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columns().size(), size_t{0});
  CHECK_EQ(fixture.layout.maxScroll(kViewport), 0);
}

// row weights
UMBRIEL_TEST(heightWeightsDefaultToOneAndTrackViews) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK(std::fabs(fixture.layout.heightWeight(0, 0) - 1.0) < 1e-9);
  CHECK(std::fabs(fixture.layout.heightWeight(0, 1) - 1.0) < 1e-9);
  CHECK_EQ(fixture.layout.columns()[0].heightWeights.size(), size_t{2});
}

UMBRIEL_TEST(setHeightWeightChangesTheStackSplit) {
  Fixture fixture;
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  CHECK(fixture.layout.setHeightWeight(0, 0, 3.0));
  fixture.layout.arrange(kUsable);

  const wlr_box upper = fixture.layout.targetBox(stub(0));
  const wlr_box lower = fixture.layout.targetBox(stub(1));
  CHECK(upper.height > lower.height);
}

UMBRIEL_TEST(gapWeightsInsetTheStack) {
  Fixture fixture;
  fixture.addColumns(1);
  fixture.layout.arrange(kUsable);
  const int fullHeight = fixture.layout.targetBox(stub(0)).height;

  CHECK(fixture.layout.setTopGapWeight(0, 1.0));
  fixture.layout.arrange(kUsable);
  const wlr_box inset = fixture.layout.targetBox(stub(0));
  CHECK(inset.height < fullHeight);
  CHECK(inset.y > kUsable.y + fixture.config.edgePad);
  CHECK(std::fabs(fixture.layout.topGapWeight(0) - 1.0) < 1e-9);
}

UMBRIEL_TEST(insertAtBottomConsumesTheResizedGap) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setHeightWeight(0, 0, 0.25));
  CHECK(fixture.layout.setBottomGapWeight(0, 0.75));
  fixture.layout.arrange(kUsable);
  const wlr_box upperBefore = fixture.layout.targetBox(stub(0));

  fixture.layout.insertViewIntoColumn(stub(1), 0, 1);
  fixture.layout.arrange(kUsable);

  const wlr_box upperAfter = fixture.layout.targetBox(stub(0));
  const wlr_box lower = fixture.layout.targetBox(stub(1));
  CHECK_EQ(upperAfter.height, upperBefore.height);
  CHECK_EQ(lower.y, upperAfter.y + upperAfter.height + fixture.config.totalGap);
  CHECK_EQ(lower.y + lower.height, kUsable.y + kUsable.height - fixture.config.edgePad);
  CHECK(std::fabs(fixture.layout.bottomGapWeight(0)) < 1e-9);
}

UMBRIEL_TEST(insertAtTopConsumesTheResizedGap) {
  Fixture fixture;
  fixture.addColumns(1);
  CHECK(fixture.layout.setHeightWeight(0, 0, 0.25));
  CHECK(fixture.layout.setTopGapWeight(0, 0.75));
  fixture.layout.arrange(kUsable);
  const wlr_box lowerBefore = fixture.layout.targetBox(stub(0));

  fixture.layout.insertViewIntoColumn(stub(1), 0, 0);
  fixture.layout.arrange(kUsable);

  const wlr_box upper = fixture.layout.targetBox(stub(1));
  const wlr_box lowerAfter = fixture.layout.targetBox(stub(0));
  CHECK_EQ(lowerAfter.height, lowerBefore.height);
  CHECK_EQ(upper.y, kUsable.y + fixture.config.edgePad);
  CHECK_EQ(lowerAfter.y, upper.y + upper.height + fixture.config.totalGap);
  CHECK(std::fabs(fixture.layout.topGapWeight(0)) < 1e-9);
}

// vertical scrolling
UMBRIEL_TEST(verticalArrangePlacesLanesTopToBottom) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  CHECK_EQ(first.x, 10);
  CHECK_EQ(first.y, 10);
  CHECK_EQ(first.width, 1260);
  CHECK_EQ(first.height, 344);
  CHECK_EQ(second.x, 10);
  CHECK_EQ(second.y, 366);
  CHECK_EQ(second.width, 1260);
  CHECK_EQ(second.height, 344);
}

UMBRIEL_TEST(verticalArrangeStacksViewsSideBySideWithinALane) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  fixture.layout.arrange(kUsable);

  const wlr_box left = fixture.layout.targetBox(stub(0));
  const wlr_box right = fixture.layout.targetBox(stub(1));
  CHECK_EQ(left.y, right.y);
  CHECK_EQ(left.height, right.height);
  CHECK_EQ(right.x, left.x + left.width + fixture.config.totalGap);
  CHECK(std::abs(left.width - right.width) <= 1);
  CHECK_EQ(left.height, 344);
}

UMBRIEL_TEST(verticalLaneExtentUsesTheGapAwareFormula) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(1);
  CHECK_EQ(fixture.layout.columnWidth(0, kVerticalViewport), 344);
}

UMBRIEL_TEST(verticalInitialSizeMatchesWhatArrangeWillAssign) {
  Fixture fixture(ScrollingDirection::Vertical);
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, std::nullopt, nullptr);
  CHECK_EQ(initial.width, 1260);
  CHECK_EQ(initial.height, 344);

  fixture.addColumns(1);
  fixture.layout.arrange(kUsable);
  const wlr_box arranged = fixture.layout.targetBox(stub(0));
  CHECK_EQ(initial.width, arranged.width);
  CHECK_EQ(initial.height, arranged.height);
}

UMBRIEL_TEST(clientMinHeightWidensAVerticalLane) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(1);
  const int unconstrained = fixture.layout.columnWidth(0, kVerticalViewport);
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.minHeight = 500}; });
  CHECK(fixture.layout.columnWidth(0, kVerticalViewport) > unconstrained);
  CHECK_EQ(fixture.layout.columnWidth(0, kVerticalViewport), 500);
}

UMBRIEL_TEST(clientMaxHeightNarrowsAVerticalLane) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(1);
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.maxHeight = 300}; });
  CHECK_EQ(fixture.layout.columnWidth(0, kVerticalViewport), 300);
}

UMBRIEL_TEST(clientMinWidthClampsWithinAVerticalLane) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(2);
  CHECK(fixture.layout.consume(stub(1), -1));
  fixture.layout.setConstraints([](const View*) { return LayoutConstraints{.minWidth = 900}; });
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(0)).width >= 900);
  CHECK(fixture.layout.targetBox(stub(1)).width >= 900);
}

UMBRIEL_TEST(verticalArrangeShiftsLanesByTheScrollOffset) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(3);
  fixture.layout.arrange(kUsable);
  const wlr_box unscrolled = fixture.layout.targetBox(stub(0));

  fixture.layout.setScroll(100);
  fixture.layout.arrange(kUsable);
  const wlr_box scrolled = fixture.layout.targetBox(stub(0));
  CHECK_EQ(scrolled.x, unscrolled.x);
  CHECK_EQ(scrolled.y, unscrolled.y - 100);
}

UMBRIEL_TEST(verticalFirstLaneTopEdgeStaysFixedWithoutCentering) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.config.scrolling.centerUnderfullStrip = false;
  fixture.addColumns(1);
  const uint32_t edges = fixture.layout.sanitizeResizeEdges(stub(0), WLR_EDGE_TOP | WLR_EDGE_LEFT);
  CHECK_EQ(edges & WLR_EDGE_TOP, uint32_t{0});
  CHECK((edges & WLR_EDGE_LEFT) != 0);
}

UMBRIEL_TEST(verticalResizeBottomEdgeGrowsTheLaneWithDy) {
  Fixture fixture(ScrollingDirection::Vertical);
  fixture.addColumns(1);
  fixture.layout.arrange(kUsable);
  const int before = fixture.layout.targetBox(stub(0)).height;

  auto resize = fixture.layout.beginResize(stub(0), WLR_EDGE_BOTTOM, kUsable);
  CHECK(resize != nullptr);
  resize->applyDelta(0.0, 40.0, kUsable);
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(0)).height > before);
}

UMBRIEL_TEST(directionFlipKeepsFractionsAndRearranges) {
  Fixture fixture;
  fixture.addColumns(2);
  const double firstFraction = fixture.layout.widthFraction(0);
  const double secondFraction = fixture.layout.widthFraction(1);
  fixture.layout.arrange(kUsable);
  const wlr_box horizontal = fixture.layout.targetBox(stub(0));

  fixture.config.scrolling.direction = ScrollingDirection::Vertical;
  fixture.layout.setConfig(&fixture.config);
  fixture.layout.arrange(kUsable);
  const wlr_box vertical = fixture.layout.targetBox(stub(0));

  CHECK(std::fabs(fixture.layout.widthFraction(0) - firstFraction) < 1e-9);
  CHECK(std::fabs(fixture.layout.widthFraction(1) - secondFraction) < 1e-9);
  CHECK_EQ(horizontal.width, 624);
  CHECK_EQ(horizontal.height, 700);
  CHECK_EQ(vertical.width, 1260);
  CHECK_EQ(vertical.height, 344);
}

UMBRIEL_TEST(snapshotRestoresColumnsStacksWidthsAndViewport) {
  Fixture source;
  source.addColumns(4);
  CHECK(source.layout.consume(stub(2), -1));
  CHECK(source.layout.moveViewVertical(stub(2), -1));
  source.layout.moveColumn(2, 0);

  const int resized = source.layout.columnOf(stub(0));
  const int stacked = source.layout.columnOf(stub(2));
  CHECK(source.layout.setWidthFraction(resized, 0.67));
  CHECK(source.layout.toggleFullWidth(resized));
  CHECK(source.layout.setHeightWeight(stacked, 0, 2.5));
  CHECK(source.layout.setTopGapWeight(stacked, 0.25));
  CHECK(source.layout.setBottomGapWeight(stacked, 0.75));
  source.layout.setScroll(137.0, true);
  source.layout.arrange(kUsable);

  const auto capture = source.layout.captureState();
  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, capture.members));
  restored.layout.setScroll(0.0);
  restored.layout.restoreSnapshotViewport(*capture.snapshot, kViewport, true);
  restored.layout.arrange(kUsable);

  CHECK_EQ(restored.layout.columns().size(), source.layout.columns().size());
  for (size_t column = 0; column < source.layout.columns().size(); ++column) {
    const Column& expected = source.layout.columns()[column];
    const Column& actual = restored.layout.columns()[column];
    CHECK_EQ(actual.views, expected.views);
    CHECK_EQ(actual.heightWeights, expected.heightWeights);
    CHECK_EQ(actual.topGapWeight, expected.topGapWeight);
    CHECK_EQ(actual.bottomGapWeight, expected.bottomGapWeight);
    CHECK_EQ(actual.widthFrac, expected.widthFrac);
    CHECK_EQ(actual.savedWidthFrac, expected.savedWidthFrac);
  }
  CHECK_EQ(restored.layout.scroll(), source.layout.scroll());
  CHECK_EQ(restored.layout.centeredRest(), source.layout.centeredRest());
  for (int id = 0; id < 4; ++id) {
    const wlr_box expected = source.layout.targetBox(stub(id));
    const wlr_box actual = restored.layout.targetBox(stub(id));
    CHECK_EQ(actual.x, expected.x);
    CHECK_EQ(actual.y, expected.y);
    CHECK_EQ(actual.width, expected.width);
    CHECK_EQ(actual.height, expected.height);
  }

  CHECK(!restored.layout.toggleFullWidth(restored.layout.columnOf(stub(0))));
  CHECK(std::fabs(restored.layout.widthFraction(restored.layout.columnOf(stub(0))) - 0.67) < 1e-9);
}

UMBRIEL_TEST(snapshotPrunesMissingRowsAndColumns) {
  Fixture source;
  source.addColumns(3);
  CHECK(source.layout.consume(stub(1), -1));
  source.layout.arrange(kUsable);
  source.layout.setScroll(636.0);
  const auto capture = source.layout.captureState();
  auto survivors = capture.members;
  std::erase_if(survivors, [](const auto& member) { return member.view == stub(1) || member.view == stub(2); });

  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, survivors));
  restored.layout.restoreSnapshotViewport(*capture.snapshot, kViewport, true);
  restored.layout.arrange(kUsable);
  CHECK_EQ(restored.layout.columns().size(), size_t{1});
  CHECK_EQ(restored.layout.columns()[0].views.size(), size_t{1});
  CHECK_EQ(restored.layout.columns()[0].views[0], stub(0));
  CHECK_EQ(restored.layout.scroll(), 0.0);
  const wlr_box survivor = restored.layout.targetBox(stub(0));
  CHECK(survivor.x < kUsable.x + kUsable.width);
  CHECK(survivor.x + survivor.width > kUsable.x);
}

UMBRIEL_TEST(snapshotKeepsTheAnchorAtTheSameRelativePositionOnANewViewport) {
  Fixture source;
  source.addColumns(4);
  source.layout.arrange(kUsable);
  source.layout.setScroll(500.0);
  source.layout.arrange(kUsable);
  const int anchor = 1;
  const double expectedFraction = (source.layout.columnX(anchor, kViewport)
                                   + source.layout.columnWidth(anchor, kViewport) / 2.0
                                   - source.layout.scroll())
      / kViewport;

  const auto capture = source.layout.captureState();
  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, capture.members));
  constexpr int newViewport = 800;
  constexpr wlr_box newUsable{0, 0, newViewport + 20, 720};
  restored.layout.restoreSnapshotViewport(*capture.snapshot, newViewport, false);
  restored.layout.arrange(newUsable);

  const double actualFraction = (restored.layout.columnX(anchor, newViewport)
                                 + restored.layout.columnWidth(anchor, newViewport) / 2.0
                                 - restored.layout.scroll())
      / newViewport;
  CHECK(std::fabs(actualFraction - expectedFraction) < 1e-9);
  CHECK(restored.layout.scroll() >= 0.0);
  CHECK(restored.layout.scroll() <= restored.layout.maxScroll(newViewport));
}

UMBRIEL_TEST(snapshotFallsBackToTheNearestLaneWhenTheCenteredLaneClosed) {
  Fixture source;
  source.addColumns(5);
  source.layout.arrange(kUsable);
  source.layout.setScroll(954.0);
  source.layout.arrange(kUsable);
  const double expectedFraction =
      (source.layout.columnX(1, kViewport) + source.layout.columnWidth(1, kViewport) / 2.0 - source.layout.scroll())
      / kViewport;

  const auto capture = source.layout.captureState();
  auto survivors = capture.members;
  std::erase_if(survivors, [](const auto& member) { return member.view == stub(2); });
  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, survivors));
  restored.layout.restoreSnapshotViewport(*capture.snapshot, kViewport, true);
  restored.layout.arrange(kUsable);

  const int anchor = restored.layout.columnOf(stub(1));
  const double actualFraction = (restored.layout.columnX(anchor, kViewport)
                                 + restored.layout.columnWidth(anchor, kViewport) / 2.0
                                 - restored.layout.scroll())
      / kViewport;
  CHECK(std::fabs(actualFraction - expectedFraction) < 1e-9);
  CHECK(restored.layout.scroll() > 0.0);
  CHECK(restored.layout.scroll() < restored.layout.maxScroll(kViewport));
}

UMBRIEL_TEST(snapshotRejectsRawScrollWhenLaneGeometryChanged) {
  Fixture source;
  source.addColumns(4);
  source.layout.arrange(kUsable);
  source.layout.setScroll(500.0);
  source.layout.arrange(kUsable);
  const double expectedFraction =
      (source.layout.columnX(1, kViewport) + source.layout.columnWidth(1, kViewport) / 2.0 - source.layout.scroll())
      / kViewport;

  const auto capture = source.layout.captureState();
  Fixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, capture.members));
  CHECK(restored.layout.setWidthFraction(0, 0.70));
  restored.layout.restoreSnapshotViewport(*capture.snapshot, kViewport, true);
  restored.layout.arrange(kUsable);

  const int anchor = restored.layout.columnOf(stub(1));
  const double actualFraction = (restored.layout.columnX(anchor, kViewport)
                                 + restored.layout.columnWidth(anchor, kViewport) / 2.0
                                 - restored.layout.scroll())
      / kViewport;
  CHECK(std::fabs(actualFraction - expectedFraction) < 1e-9);
  CHECK(restored.layout.scroll() != source.layout.scroll());
}

UMBRIEL_TEST(snapshotUsesMemberIdsInsteadOfCapturedViewPointers) {
  Fixture source;
  source.addColumns(3);
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
  CHECK_EQ(restored.layout.columns()[1].views[0], stub(11));
  CHECK_EQ(restored.layout.columns()[1].views[1], stub(12));
}

int main() { return RUN_TESTS(); }
