#include "check.h"
#include "config/config.h"
#include "layout/master.h"
#include "layout/scrolling.h"
#include "layout/tab_state.h"
#include "scene/tab_bar_geometry.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <memory>
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}
// clang-format on

using umbriel::Column;
using umbriel::ColumnDisplay;
using umbriel::ColumnTabs;
using umbriel::MasterStackLayout;
using umbriel::NewTabPosition;
using umbriel::ResolvedLayoutConfig;
using umbriel::ScrollingDirection;
using umbriel::ScrollingLayout;
using umbriel::TabBarPosition;
using umbriel::TabGroup;
using umbriel::TabState;
using umbriel::View;

namespace {

  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x5000 + (id * 0x10))); }

  // totalGap = gap + 2 * border = 12, edgePad = gap + border = 10, so a tab bar reserves 24 + 8 = 32.
  ResolvedLayoutConfig tabConfig(umbriel::LayoutMode mode) {
    ResolvedLayoutConfig config;
    config.mode = mode;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    config.scrolling.defaultExtentFraction = 0.5;
    config.extentPresets = {1.0 / 3, 0.5, 2.0 / 3};
    config.tabs.barHeight = 24;
    return config;
  }

  constexpr wlr_box kUsable{0, 0, 1280, 720};
  constexpr int kReserve = 32;

  struct ScrollingFixture {
    ResolvedLayoutConfig config = tabConfig(umbriel::LayoutMode::Scrolling);
    ScrollingLayout layout;

    explicit ScrollingFixture(ScrollingDirection direction = ScrollingDirection::Horizontal) {
      config.scrolling.direction = direction;
      layout.setConfig(&config);
    }

    // One column holding views 0..count-1, top to bottom.
    void stack(int count) {
      layout.insertView(stub(0), 0);
      for (int i = 1; i < count; ++i) {
        layout.insertView(stub(i), 1);
        layout.consume(stub(i), -1);
      }
    }

    [[nodiscard]] const Column& column(int index = 0) const { return layout.columns()[static_cast<size_t>(index)]; }
  };

  struct MasterFixture {
    ResolvedLayoutConfig config = tabConfig(umbriel::LayoutMode::Master);
    MasterStackLayout layout;

    MasterFixture() { layout.setConfig(&config); }

    void addViews(int count) {
      for (int i = 0; i < count; ++i) {
        layout.insertView(stub(i), i);
      }
    }
  };

  // The window a column's first tab group shows, else null.
  View* shownTab(const Column& column) {
    return column.tabs.empty() ? nullptr : column.views[column.tabs.groups().front().active];
  }

  bool tabbed(const Column& column) { return !column.tabs.empty(); }

  bool sameBox(const wlr_box& a, const wlr_box& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }

} // namespace

// tab selection bookkeeping
UMBRIEL_TEST(firstRowOfAnEmptyColumnIsSelected) {
  TabState tabs;
  tabs.rowInserted(0, 1);
  CHECK_EQ(tabs.active(), size_t{0});
}

UMBRIEL_TEST(insertingAtOrBeforeTheSelectionKeepsItsView) {
  TabState tabs;
  tabs.select(1, 3);
  tabs.rowInserted(1, 4);
  CHECK_EQ(tabs.active(), size_t{2});
  tabs.rowInserted(0, 5);
  CHECK_EQ(tabs.active(), size_t{3});
  tabs.rowInserted(5, 6);
  CHECK_EQ(tabs.active(), size_t{3});
}

UMBRIEL_TEST(erasingTheSelectionSelectsItsPredecessor) {
  TabState tabs;
  tabs.select(2, 4);
  tabs.rowErased(2, 3);
  CHECK_EQ(tabs.active(), size_t{1});
  // The first row has no predecessor, so the row that slides into its place shows.
  tabs.select(0, 3);
  tabs.rowErased(0, 2);
  CHECK_EQ(tabs.active(), size_t{0});
}

UMBRIEL_TEST(erasingAroundTheSelectionKeepsItsView) {
  TabState tabs;
  tabs.select(2, 4);
  tabs.rowErased(0, 3);
  CHECK_EQ(tabs.active(), size_t{1});
  tabs.rowErased(2, 2);
  CHECK_EQ(tabs.active(), size_t{1});
  tabs.rowErased(1, 1);
  CHECK_EQ(tabs.active(), size_t{0});
  tabs.rowErased(0, 0);
  CHECK_EQ(tabs.active(), size_t{0});
}

UMBRIEL_TEST(swapsCarryTheSelectionWithItsView) {
  TabState tabs;
  tabs.select(1, 3);
  tabs.rowsSwapped(1, 2);
  CHECK_EQ(tabs.active(), size_t{2});
  tabs.rowsSwapped(0, 2);
  CHECK_EQ(tabs.active(), size_t{0});
  tabs.rowsSwapped(1, 2);
  CHECK_EQ(tabs.active(), size_t{0});
}

UMBRIEL_TEST(displayChangesReportAndClampTheSelection) {
  TabState tabs;
  CHECK(tabs.select(7, 3));
  CHECK_EQ(tabs.active(), size_t{2});
  CHECK(!tabs.select(2, 3));
  CHECK(tabs.setTabbed(true, 2));
  CHECK_EQ(tabs.active(), size_t{1});
  CHECK(!tabs.setTabbed(true, 2));
  CHECK(tabs.select(0, 2));
}

// tab bar geometry
UMBRIEL_TEST(slotsAndGapsTileTheBarExactly) {
  for (const int width : {100, 101, 103, 640}) {
    for (const size_t count : {size_t{1}, size_t{3}, size_t{7}}) {
      int covered = 0;
      for (size_t index = 0; index < count; ++index) {
        const umbriel::TabSlot slot = umbriel::tabSlot(width, count, index, 2);
        covered += slot.width;
        if (index + 1 < count) {
          CHECK_EQ(umbriel::tabSlot(width, count, index + 1, 2).x, slot.x + slot.width + 2);
        } else {
          CHECK_EQ(slot.x + slot.width, width);
        }
      }
      CHECK_EQ(covered, width - 2 * static_cast<int>(count - 1));
    }
  }
}

UMBRIEL_TEST(aBarTooNarrowForItsGapsDropsThem) {
  const umbriel::TabSlot last = umbriel::tabSlot(4, 4, 3, 2);
  CHECK_EQ(last.x, 3);
  CHECK_EQ(last.width, 1);
}

UMBRIEL_TEST(hitTestingGivesAGapToTheTabBeforeIt) {
  // Slots of 32 at 0, 34, and 68 across 100 pixels.
  CHECK_EQ(umbriel::tabIndexAt(100, 3, 0.0, 2), 0);
  CHECK_EQ(umbriel::tabIndexAt(100, 3, 33.5, 2), 0);
  CHECK_EQ(umbriel::tabIndexAt(100, 3, 34.0, 2), 1);
  CHECK_EQ(umbriel::tabIndexAt(100, 3, 99.9, 2), 2);
  CHECK_EQ(umbriel::tabIndexAt(100, 3, -0.1, 2), -1);
  CHECK_EQ(umbriel::tabIndexAt(100, 3, 100.0, 2), -1);
  CHECK_EQ(umbriel::tabIndexAt(100, 0, 10.0, 2), -1);
}

UMBRIEL_TEST(aDropLandsBesideTheTabUnderIt) {
  CHECK_EQ(umbriel::tabInsertionAt(100, 3, 5.0, 2), size_t{0});
  CHECK_EQ(umbriel::tabInsertionAt(100, 3, 20.0, 2), size_t{1});
  CHECK_EQ(umbriel::tabInsertionAt(100, 3, 40.0, 2), size_t{1});
  CHECK_EQ(umbriel::tabInsertionAt(100, 3, 95.0, 2), size_t{3});
  CHECK_EQ(umbriel::tabInsertionAt(100, 3, 500.0, 2), size_t{3});
}

UMBRIEL_TEST(aStripWithinItsLimitShowsEveryTab) {
  CHECK(umbriel::tabStrip(4, 3, 0, 2) == (umbriel::TabStrip{.first = 0, .count = 4}));
  CHECK(umbriel::tabStrip(4, 3, 4, 2) == (umbriel::TabStrip{.first = 0, .count = 4}));
  CHECK(umbriel::tabStrip(0, 0, 3, 0) == (umbriel::TabStrip{.first = 0, .count = 0}));
}

UMBRIEL_TEST(aScrollingStripMovesOnlyAsFarAsTheActiveTabNeeds) {
  // Eight tabs, five slots: stepping right inside the strip leaves it where it is.
  CHECK(umbriel::tabStrip(8, 4, 5, 0) == (umbriel::TabStrip{.first = 0, .count = 5}));
  // One past its end moves it by one, not by a page.
  CHECK(umbriel::tabStrip(8, 5, 5, 0) == (umbriel::TabStrip{.first = 1, .count = 5}));
  // Back to its start pulls it back just enough.
  CHECK(umbriel::tabStrip(8, 1, 5, 3) == (umbriel::TabStrip{.first = 1, .count = 5}));
  // Inside it again, it stays.
  CHECK(umbriel::tabStrip(8, 4, 5, 3) == (umbriel::TabStrip{.first = 3, .count = 5}));
}

UMBRIEL_TEST(aScrollingStripStaysWithinItsTabs) {
  // A strip left past the end by closed tabs is pulled back to show a full run.
  CHECK(umbriel::tabStrip(6, 5, 4, 9) == (umbriel::TabStrip{.first = 2, .count = 4}));
  // An active index past the end is treated as the last tab.
  CHECK(umbriel::tabStrip(6, 40, 4, 0) == (umbriel::TabStrip{.first = 2, .count = 4}));
}

UMBRIEL_TEST(aScrollingBarPutsAButtonAtEachEnd) {
  // 400 wide, 24 tall, gap 2: square 24-pixel buttons, the slots between them.
  CHECK(
      umbriel::tabBarParts(400, 24, 2, true)
      == (umbriel::TabBarParts{.buttonWidth = 24, .slotsX = 26, .slotsWidth = 348})
  );
  // A bar showing every tab has no buttons.
  CHECK(
      umbriel::tabBarParts(400, 24, 2, false)
      == (umbriel::TabBarParts{.buttonWidth = 0, .slotsX = 0, .slotsWidth = 400})
  );
  // A wide side bar keeps its buttons short.
  CHECK_EQ(umbriel::tabBarParts(600, 160, 0, true).buttonWidth, umbriel::kTabCycleButtonMaxWidth);
  // A thin indicator still gets buttons wide enough to hit.
  CHECK_EQ(umbriel::tabBarParts(400, 4, 0, true).buttonWidth, umbriel::kTabCycleButtonMinWidth);
}

UMBRIEL_TEST(pointsOnAScrollingBarFindItsButtonsAndShownTabs) {
  // Tabs 2 and 3 of 6 show in two 173-pixel slots between 24-pixel buttons.
  const umbriel::TabStrip strip{.first = 2, .count = 2};
  const auto at = [&](double x) { return umbriel::tabBarPointAt(400, 24, 2, strip, 6, x); };
  CHECK(at(10.0) == (umbriel::TabBarPoint{.part = umbriel::TabBarPart::PreviousTab, .index = 0}));
  CHECK(at(390.0) == (umbriel::TabBarPoint{.part = umbriel::TabBarPart::NextTab, .index = 0}));
  CHECK(at(30.0) == (umbriel::TabBarPoint{.part = umbriel::TabBarPart::Tab, .index = 2}));
  CHECK(at(370.0) == (umbriel::TabBarPoint{.part = umbriel::TabBarPart::Tab, .index = 3}));
  // The gap between a button and the slots is nothing.
  CHECK(at(25.0) == umbriel::TabBarPoint{});
  CHECK(at(-1.0) == umbriel::TabBarPoint{});
}

UMBRIEL_TEST(dropsOnAScrollingBarLandAmongItsShownTabs) {
  const umbriel::TabStrip strip{.first = 2, .count = 2};
  // The leading half of the first shown slot inserts before tab 2, the trailing half of the last after tab 3.
  CHECK_EQ(umbriel::tabBarDropAt(400, 24, 2, strip, 6, 40.0).row, size_t{2});
  CHECK_EQ(umbriel::tabBarDropAt(400, 24, 2, strip, 6, 360.0).row, size_t{4});
  CHECK_EQ(umbriel::tabBarDropAt(400, 24, 2, strip, 6, 40.0).boundary, 26);
}

// tab groups
UMBRIEL_TEST(groupsFollowRowsInsertedAndErasedAroundThem) {
  ColumnTabs tabs;
  CHECK(tabs.form(1, 2, 2));
  // Before the group pushes it along, keeping the tab it shows.
  tabs.rowInserted(0);
  CHECK(tabs.groups().front() == (TabGroup{.first = 2, .count = 2, .active = 3, .bar = std::nullopt}));
  // Strictly inside joins it; at its end does not, unless asked.
  tabs.rowInserted(3);
  CHECK_EQ(tabs.groups().front().count, size_t{3});
  CHECK_EQ(tabs.groups().front().active, size_t{4});
  tabs.rowInserted(5);
  CHECK_EQ(tabs.groups().front().count, size_t{3});
  tabs.rowInserted(5, 0);
  CHECK_EQ(tabs.groups().front().count, size_t{4});
  // Erasing the shown tab shows its predecessor; erasing the last tab ends the group.
  tabs.rowErased(4);
  CHECK_EQ(tabs.groups().front().active, size_t{3});
  for (int i = 0; i < 3; ++i) {
    tabs.rowErased(2);
  }
  CHECK(tabs.empty());
}

UMBRIEL_TEST(groupsCountAsOneUnit) {
  ColumnTabs tabs;
  CHECK(tabs.form(1, 3, 1));
  CHECK_EQ(tabs.unitCount(5), size_t{3});
  CHECK_EQ(tabs.unitStart(3), size_t{1});
  CHECK_EQ(tabs.unitEnd(1), size_t{4});
  CHECK_EQ(tabs.unitEnd(4), size_t{5});
  CHECK_EQ(tabs.unitShown(3), size_t{1});
  CHECK(tabs.hidden(2));
  CHECK(!tabs.hidden(1));
  CHECK(!tabs.hidden(4));
  // Groups never overlap.
  CHECK(!tabs.form(3, 2, 3));
}

UMBRIEL_TEST(tabsLeaveAndJoinAtTheirGroupsEnds) {
  ColumnTabs tabs;
  CHECK(tabs.form(1, 3, 3));
  CHECK(!tabs.leave(2));
  CHECK(tabs.leave(3));
  CHECK(tabs.groups().front() == (TabGroup{.first = 1, .count = 2, .active = 2, .bar = std::nullopt}));
  CHECK(tabs.join(0, 0));
  CHECK_EQ(tabs.groups().front().first, size_t{0});
  CHECK(!tabs.join(4, 0));
  CHECK(tabs.join(3, 0));
  CHECK_EQ(tabs.groups().front().count, size_t{4});
}

UMBRIEL_TEST(swappedUnitsCarryTheirGroups) {
  ColumnTabs tabs;
  CHECK(tabs.form(0, 2, 1));
  // The group at rows 0-1 trades places with the row at 2.
  tabs.unitsSwapped(0, 2, 3);
  CHECK(tabs.groups().front() == (TabGroup{.first = 1, .count = 2, .active = 2, .bar = std::nullopt}));
}

// scrolling
UMBRIEL_TEST(tabsShareTheColumnBelowTheirBar) {
  ScrollingFixture fixture;
  fixture.stack(3);
  CHECK(fixture.layout.setTabbed(stub(0), true));
  fixture.layout.arrange(kUsable);
  const wlr_box first = fixture.layout.targetBox(stub(0));
  CHECK_EQ(first.y, 10 + kReserve);
  CHECK_EQ(first.height, 700 - kReserve);
  CHECK(sameBox(fixture.layout.targetBox(stub(1)), first));
  CHECK(sameBox(fixture.layout.targetBox(stub(2)), first));
}

UMBRIEL_TEST(aBottomBarReservesBelowTheTabs) {
  ScrollingFixture fixture;
  fixture.config.tabs.barPosition = TabBarPosition::Bottom;
  fixture.stack(2);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(1));
  CHECK_EQ(box.y, 10);
  CHECK_EQ(box.height, 700 - kReserve);
}

UMBRIEL_TEST(sideBarsReserveBesideTheTabs) {
  ScrollingFixture untabbed;
  untabbed.stack(2);
  untabbed.layout.arrange(kUsable);
  const wlr_box column = untabbed.layout.targetBox(stub(0));

  ScrollingFixture left;
  left.config.tabs.barPosition = TabBarPosition::Left;
  left.stack(2);
  left.layout.setTabbed(stub(0), true);
  left.layout.arrange(kUsable);
  const wlr_box leftBox = left.layout.targetBox(stub(1));
  CHECK_EQ(leftBox.x, column.x + kReserve);
  CHECK_EQ(leftBox.width, column.width - kReserve);
  CHECK_EQ(leftBox.y, 10);
  CHECK_EQ(leftBox.height, 700);

  ScrollingFixture right;
  right.config.tabs.barPosition = TabBarPosition::Right;
  right.stack(2);
  right.layout.setTabbed(stub(0), true);
  right.layout.arrange(kUsable);
  const wlr_box rightBox = right.layout.targetBox(stub(1));
  CHECK_EQ(rightBox.x, column.x);
  CHECK_EQ(rightBox.width, column.width - kReserve);
}

UMBRIEL_TEST(resizingATabbedColumnStartsFromTheColumnNotItsTabs) {
  // A side bar makes the tabs narrower than their column; a resize that started from a tab would shrink the column
  // by the bar the moment the pointer moved.
  ScrollingFixture fixture;
  fixture.config.tabs.barPosition = TabBarPosition::Left;
  fixture.stack(2);
  fixture.layout.insertView(stub(5), 1);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.arrange(kUsable);
  const double before = fixture.layout.widthFraction(0);
  std::unique_ptr<umbriel::ResizeGrab> grab = fixture.layout.beginResize(stub(0), WLR_EDGE_RIGHT, kUsable);
  CHECK(grab != nullptr);
  if (grab != nullptr) {
    grab->applyDelta(0.0, 0.0, kUsable);
  }
  CHECK(std::abs(fixture.layout.widthFraction(0) - before) < 0.001);
}

UMBRIEL_TEST(aLoneTabGivesUpAHiddenBarsSpace) {
  ScrollingFixture fixture;
  fixture.config.tabs.hideWhenSingle = true;
  fixture.stack(1);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).height, 700);
  fixture.layout.insertView(stub(1), 1);
  fixture.layout.consume(stub(1), -1);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).height, 700 - kReserve);
}

UMBRIEL_TEST(aHiddenBarGivesItsSpaceToTheTabs) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.setTabBar(stub(0), std::nullopt));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(1)).height, 700);
  CHECK(!fixture.layout.setTabBar(stub(0), false));
  // Bars the configuration hides come back for a group that asks for its own.
  ScrollingFixture hidden;
  hidden.config.tabs.barVisible = false;
  hidden.stack(2);
  hidden.layout.setTabbed(stub(0), true);
  hidden.layout.arrange(kUsable);
  CHECK_EQ(hidden.layout.targetBox(stub(0)).height, 700);
  CHECK(hidden.layout.setTabBar(stub(1), true));
  hidden.layout.arrange(kUsable);
  CHECK_EQ(hidden.layout.targetBox(stub(0)).height, 700 - kReserve);
  CHECK(!hidden.layout.setTabBar(stub(5), true));
}

UMBRIEL_TEST(untabbingRestoresTheStackedRows) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.arrange(kUsable);
  const wlr_box stacked = fixture.layout.targetBox(stub(1));
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.arrange(kUsable);
  fixture.layout.setTabbed(stub(1), false);
  fixture.layout.arrange(kUsable);
  CHECK(sameBox(fixture.layout.targetBox(stub(1)), stacked));
}

UMBRIEL_TEST(aVerticalStripTabsALaneAcrossItsTop) {
  ScrollingFixture fixture(ScrollingDirection::Vertical);
  fixture.stack(2);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.x, 10);
  CHECK_EQ(box.width, 1260);
  CHECK(sameBox(fixture.layout.targetBox(stub(1)), box));
}

UMBRIEL_TEST(selectingReportsOnlyAChangeOfTheShownTab) {
  ScrollingFixture fixture;
  fixture.stack(3);
  // A window outside every group has nothing to show it in.
  CHECK(!fixture.layout.selectTab(stub(1)));
  // Tabbing shows the window that asked.
  fixture.layout.setTabbed(stub(1), true);
  CHECK_EQ(shownTab(fixture.column()), stub(1));
  CHECK(!fixture.layout.selectTab(stub(1)));
  CHECK(fixture.layout.selectTab(stub(2)));
  CHECK(umbriel::hiddenTab(fixture.column(), stub(1)));
  CHECK(!umbriel::hiddenTab(fixture.column(), stub(2)));
}

UMBRIEL_TEST(closingTheShownTabShowsItsPredecessor) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.selectTab(stub(2));
  fixture.layout.removeView(stub(2));
  CHECK_EQ(shownTab(fixture.column()), stub(1));
  fixture.layout.removeView(stub(0));
  CHECK_EQ(shownTab(fixture.column()), stub(1));
}

UMBRIEL_TEST(consumeJoinsTabsWhereNewTabsGo) {
  ScrollingFixture fixture;
  fixture.config.tabs.newTabPosition = NewTabPosition::AfterActive;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.insertView(stub(3), 1);
  CHECK(fixture.layout.consume(stub(3), -1));
  CHECK_EQ(fixture.layout.rowOf(stub(3)), 1);
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(3)) != nullptr);
  CHECK_EQ(shownTab(fixture.column()), stub(0));
}

UMBRIEL_TEST(consumeBelowAStandaloneRowStandsAlone) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  fixture.layout.insertView(stub(3), 1);
  CHECK(fixture.layout.consume(stub(3), -1));
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(3)) == nullptr);
  CHECK_EQ(fixture.column().tabs.unitCount(fixture.column().views.size()), size_t{3});
}

// consume-from: the neighboring column's window joins the focused row, and focus stays where it was
UMBRIEL_TEST(consumeFromPullsTheNextColumnBelowTheFocusedRow) {
  ScrollingFixture fixture;
  fixture.layout.insertView(stub(0), 0);
  fixture.layout.insertView(stub(1), 1);
  fixture.layout.insertView(stub(2), 2);
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 1);
  // A second pull anchors on the focused window again, landing between it and the row pulled before.
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 2);
  // Nothing lies beyond either end.
  CHECK(!fixture.layout.consumeFrom(stub(0), 1));
  CHECK(!fixture.layout.consumeFrom(stub(0), -1));
}

UMBRIEL_TEST(consumeFromTheLeftPullsTheColumnInFront) {
  ScrollingFixture fixture;
  fixture.layout.insertView(stub(0), 0);
  fixture.layout.insertView(stub(1), 1);
  fixture.layout.insertView(stub(2), 2);
  CHECK(fixture.layout.consumeFrom(stub(2), -1));
  // The source column held one window and is gone, so the focused column ends up last.
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 1);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), 0);
}

UMBRIEL_TEST(consumeFromLeavesTheSourceColumnStandingWhileItKeepsRows) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.insertView(stub(2), 1);
  CHECK(fixture.layout.consumeFrom(stub(2), -1));
  // The source keeps its remaining row where it was, and the pulled one lands below the focused window.
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columns()[0].views.size(), size_t{1});
  CHECK_EQ(fixture.layout.columns()[0].views.front(), stub(1));
  CHECK_EQ(fixture.layout.columns()[1].views.front(), stub(2));
  CHECK_EQ(fixture.layout.columns()[1].views.back(), stub(0));
}

UMBRIEL_TEST(consumeFromPullsTheShownTabOfATabbedColumn) {
  ScrollingFixture fixture;
  fixture.layout.insertView(stub(0), 0);
  fixture.layout.insertView(stub(1), 1);
  fixture.layout.insertView(stub(2), 2);
  CHECK(fixture.layout.consume(stub(2), -1));
  CHECK(fixture.layout.setTabbed(stub(2), true));
  // The source column shows its second tab, so that is the window that comes over; the hidden one stays.
  CHECK_EQ(umbriel::columnEntry(fixture.layout.columns()[1]), stub(2));
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK_EQ(fixture.layout.columnOf(stub(2)), 0);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), 1);
  CHECK(tabbed(fixture.layout.columns()[1]));
  CHECK_EQ(shownTab(fixture.layout.columns()[1]), stub(1));
}

UMBRIEL_TEST(consumeFromAddsATabToTheFocusedGroup) {
  ScrollingFixture fixture;
  fixture.config.tabs.newTabPosition = NewTabPosition::AfterActive;
  fixture.stack(2);
  fixture.layout.insertView(stub(2), 1);
  CHECK(fixture.layout.setTabbed(stub(0), true));
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK(tabbed(fixture.column()));
  CHECK_EQ(fixture.column().tabs.groups().front().count, size_t{3});
  // Beside the active tab, per new_tab_position, and the focused window keeps showing.
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 1);
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 2);
  CHECK_EQ(shownTab(fixture.column()), stub(0));
}

UMBRIEL_TEST(consumeFromAppendsATabAtTheGroupsEndByDefault) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.insertView(stub(2), 1);
  CHECK(fixture.layout.setTabbed(stub(0), true));
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  // The pulled window is one more tab of the focused group, last, and the focused window keeps showing.
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(2)) != nullptr);
  CHECK_EQ(fixture.column().tabs.groups().front().count, size_t{3});
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 2);
  CHECK_EQ(shownTab(fixture.column()), stub(0));
}

UMBRIEL_TEST(consumeFromBelowAStandaloneRowStandsBeforeTheGroup) {
  ScrollingFixture fixture;
  fixture.stack(3);
  CHECK(fixture.layout.setTabbed(stub(1), true));
  // The last tab steps out ahead of the group and stands alone at the top.
  CHECK(fixture.layout.moveViewVertical(stub(2), -1));
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(2)) == nullptr);
  CHECK_EQ(fixture.layout.rowOf(stub(2)), 0);
  fixture.layout.insertView(stub(3), 1);
  CHECK(fixture.layout.consumeFrom(stub(2), 1));
  // The pulled window takes the row below it and stays out of the group it meets.
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(3)) == nullptr);
  CHECK_EQ(fixture.layout.rowOf(stub(3)), 1);
  CHECK_EQ(fixture.column().tabs.unitCount(fixture.column().views.size()), size_t{3});
  CHECK_EQ(shownTab(fixture.column()), stub(0));
}

UMBRIEL_TEST(movingATabStepsOutOfItsGroup) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(1), true);
  // Down from the middle: the tab leaves for the row below the group, which keeps its other tabs in order.
  CHECK(fixture.layout.moveViewVertical(stub(1), 1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 2);
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(1)) == nullptr);
  CHECK_EQ(fixture.column().views[0], stub(0));
  CHECK_EQ(fixture.column().views[1], stub(2));
  CHECK_EQ(fixture.column().tabs.groups().front().count, size_t{2});
  // Back up: it meets the group and joins it as its last tab, on show.
  CHECK(fixture.layout.moveViewVertical(stub(1), -1));
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(1)) != nullptr);
  CHECK_EQ(shownTab(fixture.column()), stub(1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 2);
}

UMBRIEL_TEST(aOneTabGroupTradesPlacesAsAUnit) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(1), 1));
  // Rows: a group holding 0 alone, then 1. The group moves down past 1 and stays a group.
  CHECK(fixture.layout.moveViewVertical(stub(0), 1));
  CHECK_EQ(fixture.layout.rowOf(stub(0)), 1);
  CHECK(fixture.column().tabs.groups().front() == (TabGroup{.first = 1, .count = 1, .active = 1, .bar = std::nullopt}));
  CHECK(!fixture.layout.moveViewVertical(stub(0), 1));
}

UMBRIEL_TEST(tabsReorderWithinTheirGroup) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(1), true);
  CHECK(fixture.layout.moveTab(stub(1), 1));
  CHECK_EQ(fixture.layout.rowOf(stub(1)), 2);
  CHECK_EQ(shownTab(fixture.column()), stub(1));
  CHECK(!fixture.layout.moveTab(stub(1), 1));
  CHECK(fixture.layout.swapViews(stub(1), stub(0)));
  CHECK_EQ(shownTab(fixture.column()), stub(1));
}

UMBRIEL_TEST(aTabGroupResizesAsOneRow) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  fixture.layout.arrange(kUsable);
  // Two units share 700 less one gap: 344 each. The group's tabs sit below its bar.
  const wlr_box tab = fixture.layout.targetBox(stub(0));
  CHECK_EQ(tab.y, 10 + kReserve);
  CHECK_EQ(tab.height, 344 - kReserve);
  CHECK(sameBox(fixture.layout.targetBox(stub(1)), tab));
  CHECK_EQ(fixture.layout.targetBox(stub(2)).y, 10 + 344 + 12);
  CHECK(fixture.layout.setHeightFraction(stub(0), 0.75));
  CHECK(std::abs(fixture.layout.heightFraction(stub(1)) - 0.75) < 0.001);
  const uint32_t edges = fixture.layout.sanitizeResizeEdges(stub(0), WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
  CHECK_EQ(edges, static_cast<uint32_t>(WLR_EDGE_TOP | WLR_EDGE_BOTTOM));
}

UMBRIEL_TEST(draggingTheBoundaryBelowAGroupResizesIt) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  fixture.layout.arrange(kUsable);
  std::unique_ptr<umbriel::ResizeGrab> grab = fixture.layout.beginResize(stub(1), WLR_EDGE_BOTTOM, kUsable);
  CHECK(grab != nullptr);
  if (grab != nullptr) {
    grab->applyDelta(0.0, 100.0, kUsable);
  }
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).height, 444 - kReserve);
  CHECK_EQ(fixture.layout.targetBox(stub(2)).height, 244);
}

UMBRIEL_TEST(tabbingTakesTheStandaloneRunAroundTheWindow) {
  ScrollingFixture fixture;
  fixture.stack(4);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(3), 1));
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  CHECK(fixture.layout.setTabbed(stub(3), true));
  const std::vector<TabGroup>& groups = fixture.column().tabs.groups();
  CHECK_EQ(groups.size(), size_t{2});
  CHECK(groups.back() == (TabGroup{.first = 2, .count = 2, .active = 3, .bar = std::nullopt}));
}

UMBRIEL_TEST(aDropJoinsTheGroupItNames) {
  ScrollingFixture fixture;
  fixture.stack(2);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.insertTab(stub(7), 0, 2, stub(0)));
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(7)) != nullptr);
  // A row at a group's end stands beside it unless the drop asks to join.
  fixture.layout.insertViewIntoColumn(stub(8), 0, 3);
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(8)) == nullptr);
  CHECK(!fixture.layout.insertTab(stub(9), 0, 5, stub(0)));
}

UMBRIEL_TEST(aDropAtAnAdjacentGroupsStartJoinsThatGroup) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  CHECK(fixture.layout.setTabbed(stub(2), true));
  CHECK(fixture.layout.insertTab(stub(7), 0, 2, stub(2)));
  const Column& column = fixture.column();
  CHECK_EQ(column.tabs.groups().front().count, size_t{2});
  CHECK(umbriel::tabGroupOf(column, stub(7)) == umbriel::tabGroupOf(column, stub(2)));
  CHECK(umbriel::tabGroupOf(column, stub(7)) != umbriel::tabGroupOf(column, stub(0)));
}

UMBRIEL_TEST(aDropAtAnAdjacentGroupsEndJoinsThatGroup) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  CHECK(fixture.layout.moveViewVertical(stub(2), 1));
  CHECK(fixture.layout.setTabbed(stub(2), true));
  CHECK(fixture.layout.insertTab(stub(7), 0, 2, stub(0)));
  const Column& column = fixture.column();
  CHECK_EQ(column.tabs.groups().back().count, size_t{1});
  CHECK(umbriel::tabGroupOf(column, stub(7)) == umbriel::tabGroupOf(column, stub(0)));
  CHECK(umbriel::tabGroupOf(column, stub(7)) != umbriel::tabGroupOf(column, stub(2)));
}

UMBRIEL_TEST(rebuiltColumnsReplaceTheDestinationsDefaultTabs) {
  ScrollingFixture source;
  source.stack(3);
  source.layout.setTabbed(stub(0), true);
  source.layout.selectTab(stub(1));
  source.layout.setTabBar(stub(0), false);
  ScrollingFixture target;
  target.config.tabs.defaultDisplay = ColumnDisplay::Tabbed;
  target.layout.insertView(stub(0), 0);
  target.layout.insertViewIntoColumn(stub(1), 0, 1);
  target.layout.insertViewIntoColumn(stub(2), 0, 2);
  CHECK(target.layout.adoptTabs(0, source.column().tabs));
  CHECK_EQ(shownTab(target.column()), stub(1));
  CHECK_EQ(target.column().tabs.groups().front().count, size_t{3});
  CHECK(target.column().tabs.groups().front().bar == std::optional<bool>(false));
  CHECK(target.column().tabs.hidden(0));
  CHECK(!target.column().tabs.hidden(1));
  CHECK(target.column().tabs.hidden(2));
}

UMBRIEL_TEST(replacingDefaultTabsAllowsIndependentRowWeights) {
  ScrollingFixture target;
  target.config.tabs.defaultDisplay = ColumnDisplay::Tabbed;
  target.stack(3);
  CHECK_EQ(target.column().tabs.groups().front().count, size_t{3});
  CHECK(target.layout.adoptTabs(0, ColumnTabs{}));
  CHECK(target.layout.setHeightWeight(0, 0, 1.0));
  CHECK(target.layout.setHeightWeight(0, 1, 2.0));
  CHECK(target.layout.setHeightWeight(0, 2, 3.0));
  const std::vector<double>& weights = target.column().heightWeights;
  CHECK_EQ(weights[0], 1.0);
  CHECK_EQ(weights[1], 2.0);
  CHECK_EQ(weights[2], 3.0);
  target.layout.arrange(kUsable);
  CHECK(target.layout.targetBox(stub(0)).height < target.layout.targetBox(stub(1)).height);
  CHECK(target.layout.targetBox(stub(1)).height < target.layout.targetBox(stub(2)).height);
}

UMBRIEL_TEST(masterGroupsExportTotalWeightAndKeepUntabbedProportions) {
  MasterFixture source;
  source.addViews(3);
  View* first = source.layout.columns()[1].views[0];
  View* last = source.layout.columns()[1].views[1];
  CHECK(source.layout.setHeightFraction(first, 0.25));
  source.layout.arrange(kUsable);
  const wlr_box firstBox = source.layout.targetBox(first);
  const wlr_box lastBox = source.layout.targetBox(last);
  const double totalWeight = source.layout.columns()[1].heightWeights[0] + source.layout.columns()[1].heightWeights[1];
  CHECK(source.layout.setTabbed(first, true));
  CHECK(std::abs(source.layout.columns()[1].heightWeights[0] - totalWeight) < 1e-9);
  CHECK(std::abs(source.layout.columns()[1].heightWeights[1] - totalWeight) < 1e-9);
  CHECK(source.layout.setTabbed(first, false));
  source.layout.arrange(kUsable);
  CHECK(sameBox(source.layout.targetBox(first), firstBox));
  CHECK(sameBox(source.layout.targetBox(last), lastBox));
}

UMBRIEL_TEST(newColumnsTakeTheConfiguredDisplay) {
  ScrollingFixture fixture;
  fixture.config.tabs.defaultDisplay = ColumnDisplay::Tabbed;
  fixture.stack(2);
  CHECK(tabbed(fixture.column()));
  CHECK(umbriel::tabGroupOf(fixture.column(), stub(1)) != nullptr);
  CHECK(fixture.layout.expel(stub(1), 1));
  CHECK(tabbed(fixture.column(1)));
}

UMBRIEL_TEST(snapshotsKeepTabsAndLetMissingMembersLeave) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.setTabbed(stub(0), true);
  fixture.layout.selectTab(stub(2));
  umbriel::LayoutCapture capture = fixture.layout.captureState();
  // The shown tab does not come back: its predecessor shows, as after a close.
  std::erase_if(capture.members, [](const umbriel::LayoutMember& member) { return member.view == stub(2); });
  ScrollingFixture restored;
  CHECK(restored.layout.restoreState(*capture.snapshot, capture.members));
  CHECK(tabbed(restored.column()));
  CHECK_EQ(shownTab(restored.column()), stub(1));
}

UMBRIEL_TEST(enteringATabbedColumnLandsOnItsShownTab) {
  ScrollingFixture fixture;
  fixture.stack(3);
  fixture.layout.insertView(stub(3), 1);
  fixture.layout.setTabbed(stub(1), true);
  const std::vector<View*> peers = fixture.layout.focusPeers(stub(3), stub(0));
  CHECK_EQ(peers.size(), size_t{1});
  CHECK(!peers.empty() && peers.front() == stub(1));
  CHECK_EQ(umbriel::columnEntry(fixture.column()), stub(1));
}

// master
UMBRIEL_TEST(aTabbedStackSharesItsAreaBelowTheBar) {
  MasterFixture fixture;
  fixture.addViews(3);
  // Area 1 is the stack, right of the master.
  CHECK(fixture.layout.setTabbed(fixture.layout.columns()[1].views[0], true));
  fixture.layout.arrange(kUsable);
  const auto& stack = fixture.layout.columns()[1];
  const wlr_box first = fixture.layout.targetBox(stack.views[0]);
  CHECK_EQ(first.y, 10 + kReserve);
  CHECK_EQ(first.height, 700 - kReserve);
  CHECK(sameBox(fixture.layout.targetBox(stack.views[1]), first));
  // The master area is untouched.
  CHECK_EQ(fixture.layout.targetBox(fixture.layout.columns()[0].views[0]).height, 700);
}

UMBRIEL_TEST(upAndDownWalkATabbedAreaWithoutWrapping) {
  MasterFixture fixture;
  fixture.addViews(4);
  fixture.layout.setTabbed(fixture.layout.columns()[1].views[0], true);
  fixture.layout.arrange(kUsable);
  const std::vector<View*> stack = fixture.layout.columns()[1].views;
  CHECK(fixture.layout.focusVerticalLeaf(stack[0], 1) == std::optional<View*>(stack[1]));
  CHECK(fixture.layout.focusVerticalLeaf(stack[0], -1) == std::optional<View*>(nullptr));
  CHECK(fixture.layout.focusVerticalLeaf(stack.back(), 1) == std::optional<View*>(nullptr));
}

UMBRIEL_TEST(movingWithinATabbedAreaReordersTabs) {
  MasterFixture fixture;
  fixture.addViews(4);
  const std::vector<View*> stack = fixture.layout.columns()[1].views;
  fixture.layout.setTabbed(stack[0], true);
  CHECK(fixture.layout.moveViewVertical(stack[0], 1));
  CHECK_EQ(fixture.layout.columns()[1].views[1], stack[0]);
  CHECK_EQ(shownTab(fixture.layout.columns()[1]), stack[0]);
  CHECK(fixture.layout.moveTab(stack[0], -1));
  CHECK_EQ(fixture.layout.columns()[1].views[0], stack[0]);
}

UMBRIEL_TEST(consumeFromPullsTheStackEntryBelowTheFocusedMasterRow) {
  MasterFixture fixture;
  // Rows in creation order, so the stack reads 1 then 2 below the master.
  fixture.config.master.newOnTop = false;
  fixture.addViews(3);
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
  CHECK_EQ(fixture.layout.columns()[0].views.size(), size_t{2});
  CHECK_EQ(fixture.layout.columns()[0].views[0], stub(0));
  CHECK_EQ(fixture.layout.columns()[0].views[1], stub(1));
  CHECK_EQ(fixture.layout.columns()[1].views.front(), stub(2));
}

UMBRIEL_TEST(consumeFromTheLeftEmptiesTheAreaItPullsFrom) {
  MasterFixture fixture;
  fixture.config.master.newOnTop = false;
  fixture.addViews(3);
  CHECK(fixture.layout.consumeFrom(stub(2), -1));
  // The master area gave up its only window, so only the stack it joined is left.
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
  CHECK_EQ(fixture.layout.columns()[0].views.size(), size_t{3});
  CHECK_EQ(fixture.layout.columns()[0].views[2], stub(0));
}

UMBRIEL_TEST(consumeFromAddsTheNeighboringAreaEntryAsATab) {
  MasterFixture fixture;
  fixture.config.master.newOnTop = false;
  fixture.config.tabs.newTabPosition = NewTabPosition::AfterActive;
  fixture.addViews(3);
  CHECK(fixture.layout.setTabbed(stub(0), true));
  CHECK(fixture.layout.consumeFrom(stub(0), 1));
  CHECK(tabbed(fixture.layout.columns()[0]));
  CHECK_EQ(fixture.layout.columns()[0].views.size(), size_t{2});
  CHECK_EQ(fixture.layout.columns()[0].views[1], stub(1));
  CHECK_EQ(shownTab(fixture.layout.columns()[0]), stub(0));
  CHECK_EQ(fixture.layout.columns()[1].views.front(), stub(2));
}

UMBRIEL_TEST(consumeFromDoesNothingWithoutThatNeighbor) {
  MasterFixture fixture;
  fixture.addViews(2);
  CHECK(!fixture.layout.consumeFrom(stub(0), -1));
  CHECK(!fixture.layout.consumeFrom(fixture.layout.columns()[1].views.front(), 1));
  CHECK(!fixture.layout.consumeFrom(stub(0), 2));
}

UMBRIEL_TEST(anEmptiedAreaTakesTheConfiguredDisplayAgain) {
  MasterFixture fixture;
  fixture.addViews(2);
  fixture.layout.setTabbed(fixture.layout.columns()[1].views[0], true);
  fixture.layout.removeView(fixture.layout.columns()[1].views[0]);
  fixture.layout.insertView(stub(9), 1);
  CHECK(!tabbed(fixture.layout.columns()[1]));
}

UMBRIEL_TEST(aNewMasterKeepsATabbedMasterArea) {
  MasterFixture fixture;
  fixture.config.master.newBecomesMaster = true;
  fixture.addViews(2);
  fixture.layout.setTabbed(fixture.layout.columns()[0].views[0], true);
  fixture.layout.insertView(stub(5), 0);
  CHECK(tabbed(fixture.layout.columns()[0]));
  CHECK_EQ(fixture.layout.columns()[0].views.front(), stub(5));
}

UMBRIEL_TEST(aJoiningStackRowIsSizedForTheTabbedArea) {
  MasterFixture fixture;
  fixture.addViews(2);
  fixture.layout.setTabbed(fixture.layout.columns()[1].views[0], true);
  const umbriel::Layout::InitialSize size =
      fixture.layout.initialSize(kUsable, false, std::nullopt, std::nullopt, nullptr);
  CHECK_EQ(size.height, 700 - kReserve);
}

UMBRIEL_TEST(aMasterAreaHidesItsBarOnRequest) {
  MasterFixture fixture;
  fixture.addViews(3);
  View* tab = fixture.layout.columns()[1].views[0];
  fixture.layout.setTabbed(tab, true);
  CHECK(fixture.layout.setTabBar(tab, false));
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(tab).height, 700);
}

int main() { return RUN_TESTS(); }
