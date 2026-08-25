#include "check.h"
#include "config/config.h"
#include "layout/dwindle.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <optional>
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}
// clang-format on

using umbriel::DwindleLayout;
using umbriel::Layout;
using umbriel::LayoutConstraints;
using umbriel::ResolvedLayoutConfig;
using umbriel::View;

namespace {

  // The layout treats View as an opaque identity; these are addresses, never
  // dereferenced.
  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x2000 + (id * 0x10))); }

  ResolvedLayoutConfig dwindleConfig() {
    ResolvedLayoutConfig config;
    config.mode = umbriel::LayoutMode::Dwindle;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    return config;
  }

  constexpr wlr_box kUsable{0, 0, 1280, 720};
  constexpr wlr_box kPortraitUsable{0, 0, 1080, 1920};

  struct Fixture {
    ResolvedLayoutConfig config = dwindleConfig();
    DwindleLayout layout;

    Fixture() { layout.setConfig(&config); }

    // Deliberately no arrange() between inserts: Workspace::applyConfig
    // batch-inserts on a fresh layout when the layout mode changes.
    void addLeaves(int count) {
      for (int i = 0; i < count; ++i) {
        layout.insertView(stub(i), i);
      }
    }
  };

  bool overlaps(const wlr_box& a, const wlr_box& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

} // namespace

// tree bookkeeping
UMBRIEL_TEST(firstLeafFillsTheUsableArea) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);

  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.x, kUsable.x + fixture.config.edgePad);
  CHECK_EQ(box.y, kUsable.y + fixture.config.edgePad);
  CHECK_EQ(box.width, kUsable.width - 2 * fixture.config.edgePad);
  CHECK_EQ(box.height, kUsable.height - 2 * fixture.config.edgePad);
}

UMBRIEL_TEST(insertBuildsOneLeafPerView) {
  Fixture fixture;
  fixture.addLeaves(4);
  CHECK_EQ(fixture.layout.columns().size(), size_t{4});
  for (int i = 0; i < 4; ++i) {
    CHECK(fixture.layout.columnOf(stub(i)) >= 0);
  }
}

UMBRIEL_TEST(insertIsIdempotentPerView) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.insertView(stub(0), 0);
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
}

UMBRIEL_TEST(batchInsertWithoutArrangeKeepsEveryView) {
  // Workspace::applyConfig creates a fresh layout and re-inserts every tiled view in a loop, arranging only afterwards.
  // insertView locates its target leaf through the flat-column cache, so if a structural edit leaves that cache stale,
  // every view after the first is silently dropped.
  Fixture fixture;
  for (int i = 0; i < 5; ++i) {
    fixture.layout.insertView(stub(i), static_cast<int>(fixture.layout.columns().size()));
  }
  CHECK_EQ(fixture.layout.columns().size(), size_t{5});

  fixture.layout.arrange(kUsable);
  for (int i = 0; i < 5; ++i) {
    CHECK(fixture.layout.columnOf(stub(i)) >= 0);
    CHECK(fixture.layout.targetBox(stub(i)).width > 0);
  }
}

UMBRIEL_TEST(swapOperationsRefreshTheColumnMapping) {
  // consumeLeft, expelRight, moveViewVertical, and moveColumn reassign leaf views. The column mapping must be current
  // straight afterwards, without waiting for the next arrange().
  Fixture fixture;
  fixture.addLeaves(2);
  const int first = fixture.layout.columnOf(stub(0));
  const int second = fixture.layout.columnOf(stub(1));
  CHECK(first >= 0);
  CHECK(second >= 0);

  fixture.layout.moveColumn(first, second);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), second);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), first);
}

UMBRIEL_TEST(unknownViewHasNoColumn) {
  Fixture fixture;
  fixture.addLeaves(2);
  CHECK_EQ(fixture.layout.columnOf(stub(99)), -1);
  CHECK_EQ(fixture.layout.columnOf(nullptr), -1);
}

UMBRIEL_TEST(everyLeafIsOnRowZero) {
  // Dwindle has no row concept; the whole tree is flattened to columns.
  Fixture fixture;
  fixture.addLeaves(3);
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(fixture.layout.rowOf(stub(i)), 0);
  }
}

UMBRIEL_TEST(removingCollapsesTheSiblingIntoTheParent) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.removeView(stub(1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});

  fixture.layout.arrange(kUsable);
  // The survivor reclaims the whole area rather than keeping half of a split.
  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.width, kUsable.width - 2 * fixture.config.edgePad);
  CHECK_EQ(box.height, kUsable.height - 2 * fixture.config.edgePad);
}

UMBRIEL_TEST(removingEveryViewEmptiesTheTree) {
  Fixture fixture;
  fixture.addLeaves(3);
  for (int i = 0; i < 3; ++i) {
    fixture.layout.removeView(stub(i));
  }
  CHECK_EQ(fixture.layout.columns().size(), size_t{0});
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 0);
}

UMBRIEL_TEST(removingAnUnknownViewIsHarmless) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.removeView(stub(99));
  fixture.layout.removeView(nullptr);
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
}

// tiling geometry
UMBRIEL_TEST(twoLeavesSplitTheAreaWithoutOverlapping) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));

  CHECK(!overlaps(first, second));
  CHECK(first.width > 0);
  CHECK(second.width > 0);
  // A 1280x720 area splits horizontally first, so the two share a top edge.
  CHECK_EQ(first.y, second.y);
  CHECK_EQ(first.height, second.height);
}

UMBRIEL_TEST(leavesNeverOverlapOrLeaveTheUsableArea) {
  Fixture fixture;
  fixture.addLeaves(6);
  fixture.layout.arrange(kUsable);

  const wlr_box bounds{
      kUsable.x + fixture.config.edgePad,
      kUsable.y + fixture.config.edgePad,
      kUsable.width - 2 * fixture.config.edgePad,
      kUsable.height - 2 * fixture.config.edgePad,
  };

  for (int i = 0; i < 6; ++i) {
    const wlr_box box = fixture.layout.targetBox(stub(i));
    CHECK(box.width > 0);
    CHECK(box.height > 0);
    CHECK(box.x >= bounds.x);
    CHECK(box.y >= bounds.y);
    CHECK(box.x + box.width <= bounds.x + bounds.width);
    CHECK(box.y + box.height <= bounds.y + bounds.height);

    for (int j = i + 1; j < 6; ++j) {
      CHECK(!overlaps(box, fixture.layout.targetBox(stub(j))));
    }
  }
}

UMBRIEL_TEST(splitsFollowTheLongerEdgeOnALandscapeArea) {
  Fixture fixture;
  fixture.addLeaves(3);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  const wlr_box third = fixture.layout.targetBox(stub(2));

  // 1260x700 is wider than tall, so the first split is horizontal (side by
  // side). Each 624x700 half is taller than wide, so the second stacks.
  CHECK(first.x != second.x);
  CHECK_EQ(second.x, third.x);
  CHECK(second.y != third.y);
}

UMBRIEL_TEST(splitsFollowTheLongerEdgeOnAPortraitArea) {
  Fixture fixture;
  fixture.addLeaves(3);
  fixture.layout.arrange(kPortraitUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  const wlr_box third = fixture.layout.targetBox(stub(2));

  // 1060x1900 is taller than wide, so the first split stacks. Each 1060x944
  // half is wider than tall, so the second goes side by side.
  CHECK_EQ(first.x, second.x);
  CHECK(first.y != second.y);
  CHECK(second.x != third.x);
  CHECK_EQ(second.y, third.y);
}

UMBRIEL_TEST(aResolvedSplitKeepsItsAxisWhenTheAreaChangesShape) {
  // The axis is chosen once, on the first arrange after the split. Re-deriving
  // it on every arrange would let a resize rotate a subtree the user never
  // touched.
  Fixture fixture;
  fixture.addLeaves(3);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).x, fixture.layout.targetBox(stub(2)).x);

  // Squeeze leaf 0 so the subtree holding leaves 1 and 2 becomes wider than
  // tall: 1128x700 instead of 624x700.
  CHECK(fixture.layout.setResizeBoundary(stub(0), WLR_EDGE_RIGHT, 0.1));
  fixture.layout.arrange(kUsable);

  const wlr_box second = fixture.layout.targetBox(stub(1));
  const wlr_box third = fixture.layout.targetBox(stub(2));
  CHECK(second.width > second.height);
  CHECK_EQ(second.x, third.x);
  CHECK(second.y != third.y);
}

UMBRIEL_TEST(gapsSeparateSiblings) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  CHECK_EQ(second.x - (first.x + first.width), fixture.config.totalGap);
}

UMBRIEL_TEST(targetBoxOfAnUnknownViewIsEmpty) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(99));
  CHECK_EQ(box.width, 0);
  CHECK_EQ(box.height, 0);
}

UMBRIEL_TEST(arrangeOnAnEmptyTreeIsHarmless) {
  Fixture fixture;
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columns().size(), size_t{0});
}

// initial sizing
UMBRIEL_TEST(initialSizeFillsTheAreaForTheFirstLeaf) {
  Fixture fixture;
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, std::nullopt);

  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);
  const wlr_box arranged = fixture.layout.targetBox(stub(0));

  // The first leaf owns the whole content area, so the first configure must
  // say so too.
  CHECK_EQ(initial.width, arranged.width);
  CHECK_EQ(initial.height, arranged.height);
}

UMBRIEL_TEST(initialSizeShrinksOnceTheTreeIsPopulated) {
  Fixture fixture;
  const Layout::InitialSize empty = fixture.layout.initialSize(kUsable, std::nullopt);
  fixture.addLeaves(1);
  const Layout::InitialSize populated = fixture.layout.initialSize(kUsable, std::nullopt);
  CHECK(populated.width < empty.width);
}

UMBRIEL_TEST(initialSizeMatchesTheSplitArrangeWillMake) {
  // The size a view is configured with before it joins the layout must equal
  // the size the layout gives it once it has, or the client's first buffer is
  // wrong and the window resizes on first paint.
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);
  const Layout::InitialSize initial = fixture.layout.initialSize(kUsable, std::nullopt);

  fixture.layout.insertView(stub(1), 1);
  fixture.layout.arrange(kUsable);
  const wlr_box arranged = fixture.layout.targetBox(stub(1));
  CHECK_EQ(initial.width, arranged.width);
  CHECK_EQ(initial.height, arranged.height);
}

UMBRIEL_TEST(initialSizeSplitsTheHeightOnAPortraitArea) {
  // Predicting a half-width column on a portrait output configures every new
  // window against the wrong axis.
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kPortraitUsable);
  const Layout::InitialSize initial = fixture.layout.initialSize(kPortraitUsable, std::nullopt);

  fixture.layout.insertView(stub(1), 1);
  fixture.layout.arrange(kPortraitUsable);
  const wlr_box arranged = fixture.layout.targetBox(stub(1));
  CHECK_EQ(initial.width, arranged.width);
  CHECK_EQ(initial.height, arranged.height);
  CHECK(initial.height < initial.width);
}

UMBRIEL_TEST(initialSizeIgnoresARuleWidthFraction) {
  // default_width is a viewport fraction, which means nothing to a splitting
  // layout; it must not change the answer.
  Fixture fixture;
  fixture.addLeaves(1);
  CHECK_EQ(fixture.layout.initialSize(kUsable, 1.0 / 3).width, fixture.layout.initialSize(kUsable, std::nullopt).width);
}

// hit testing
UMBRIEL_TEST(leafIndexAtFindsTheLeafUnderAPoint) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const int index = fixture.layout.leafIndexAt(first.x + first.width / 2.0, first.y + first.height / 2.0);
  CHECK(index >= 0);
  CHECK_EQ(fixture.layout.targetBoxByIndex(index).x, first.x);
}

UMBRIEL_TEST(leafIndexAtMissesOutsideTheArea) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.leafIndexAt(-500, -500), -1);
  CHECK_EQ(fixture.layout.leafIndexAt(99999, 99999), -1);
}

// resize boundaries
UMBRIEL_TEST(onlyInternalEdgesAreResizable) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  // Screen-facing edges are not resizable; the shared boundary is.
  const uint32_t firstEdges = fixture.layout.resizableEdges(stub(0));
  CHECK_EQ(firstEdges & WLR_EDGE_LEFT, uint32_t{0});
  CHECK(firstEdges & WLR_EDGE_RIGHT);

  const uint32_t secondEdges = fixture.layout.resizableEdges(stub(1));
  CHECK(secondEdges & WLR_EDGE_LEFT);
  CHECK_EQ(secondEdges & WLR_EDGE_RIGHT, uint32_t{0});
}

UMBRIEL_TEST(aLoneLeafHasNoResizableEdges) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.resizableEdges(stub(0)), uint32_t{0});
}

UMBRIEL_TEST(setResizeBoundaryMovesTheSharedEdge) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  const int before = fixture.layout.targetBox(stub(0)).width;

  double ratio = 0.0;
  double span = 0.0;
  CHECK(fixture.layout.resizeBoundary(stub(0), WLR_EDGE_RIGHT, &ratio, &span));
  CHECK(std::fabs(ratio - 0.5) < 1e-6);
  CHECK(span > 0.0);

  CHECK(fixture.layout.setResizeBoundary(stub(0), WLR_EDGE_RIGHT, 0.75));
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(0)).width > before);

  // The sibling gives up exactly what the first leaf gained.
  CHECK(!overlaps(fixture.layout.targetBox(stub(0)), fixture.layout.targetBox(stub(1))));
}

UMBRIEL_TEST(resizeBoundaryRejectsAScreenFacingEdge) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  double ratio = 0.0;
  double span = 0.0;
  CHECK(!fixture.layout.resizeBoundary(stub(0), WLR_EDGE_LEFT, &ratio, &span));
  CHECK(!fixture.layout.setResizeBoundary(stub(0), WLR_EDGE_LEFT, 0.75));
}

UMBRIEL_TEST(cycleWidthBackWalksThePresetsInReverse) {
  Fixture fixture;
  fixture.config.widthPresets = {1.0 / 3.0, 0.5, 2.0 / 3.0};
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const int span = kUsable.width - 2 * fixture.config.edgePad - fixture.config.totalGap;

  CHECK(fixture.layout.setWidthFraction(0, 2.0 / 3.0));

  CHECK(fixture.layout.cycleWidth(0, -1));
  fixture.layout.arrange(kUsable);
  CHECK(std::abs(fixture.layout.targetBox(stub(0)).width - static_cast<int>(span * 0.5)) <= 2);

  CHECK(fixture.layout.cycleWidth(0, -1));
  fixture.layout.arrange(kUsable);
  CHECK(std::abs(fixture.layout.targetBox(stub(0)).width - static_cast<int>(span * (1.0 / 3.0))) <= 2);

  // Wraps back to the last preset.
  CHECK(fixture.layout.cycleWidth(0, -1));
  fixture.layout.arrange(kUsable);
  CHECK(std::abs(fixture.layout.targetBox(stub(0)).width - static_cast<int>(span * (2.0 / 3.0))) <= 2);
}

// The scrolling-only API is no longer reachable from a DwindleLayout at all: scroll offsets, column positions, and row
// weights moved onto ScrollingLayout, so the question a previous test asked here ("does dwindle answer 0?") cannot be
// compiled any more. That is the point. Callers reach those through Workspace::scrollingLayout(), which is null for a
// dwindle workspace.

int main() { return RUN_TESTS(); }
