#include "scene/cheatsheet_rows.h"

#include "check.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <xkbcommon/xkbcommon.h>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}
// clang-format on

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using umbriel::buildCheatsheetRows;
using umbriel::CheatsheetRow;
using umbriel::Keybind;
using umbriel::KeybindAction;

namespace {

  Keybind bind(KeybindAction action, uint32_t keysym, uint32_t modifiers = 0) {
    Keybind result;
    result.useMod = true;
    result.modifiers = modifiers;
    result.keysym = keysym;
    result.action = action;
    return result;
  }

  Keybind spawnBind(uint32_t keysym, const std::string& command) {
    Keybind result = bind(KeybindAction::Spawn, keysym);
    umbriel::SpawnArg spawn;
    spawn.command = command;
    result.payload = std::move(spawn);
    return result;
  }

  Keybind workspaceBind(uint32_t keysym, const std::string& name, uint32_t modifiers = 0) {
    Keybind result = bind(KeybindAction::WorkspaceSwitch, keysym, modifiers);
    umbriel::WorkspaceArg workspace;
    workspace.name = name;
    result.payload = std::move(workspace);
    return result;
  }

  Keybind layoutBind(uint32_t keysym, umbriel::LayoutMode mode) {
    Keybind result = bind(KeybindAction::WorkspaceSetLayout, keysym);
    result.payload = umbriel::LayoutModeArg{.mode = mode};
    return result;
  }

  // The ditto mark a merged group's second and later rows carry.
  const std::string kDitto = "\xe2\x80\xb3";

  size_t countRows(const std::vector<CheatsheetRow>& rows, KeybindAction action) {
    return static_cast<size_t>(std::ranges::count_if(rows, [action](const CheatsheetRow& row) {
      return row.actionType == action;
    }));
  }

} // namespace

UMBRIEL_TEST(anEmptyBindListProducesNoRows) { CHECK(buildCheatsheetRows({}).empty()); }

UMBRIEL_TEST(bindsWithNoActionAreSkipped) {
  const std::vector<Keybind> binds = {Keybind{}, bind(KeybindAction::WindowClose, XKB_KEY_q)};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK(rows[0].actionType == KeybindAction::WindowClose);
}
UMBRIEL_TEST(modifierOnlyBindHasNoTrailingKeysym) {
  Keybind modifier;
  modifier.useMod = true;
  modifier.modifierOnly = true;
  modifier.action = KeybindAction::WindowClose;

  const auto rows = buildCheatsheetRows(std::vector<Keybind>{modifier});
  CHECK_EQ(rows.size(), size_t{1});
  CHECK_EQ(rows[0].chord, std::string{"Mod"});
}

UMBRIEL_TEST(oneRowPerBindWhenActionsDiffer) {
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowClose, XKB_KEY_q),
      bind(KeybindAction::ToggleFloating, XKB_KEY_t),
      bind(KeybindAction::OverviewToggle, XKB_KEY_o),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{3});
  for (const CheatsheetRow& row : rows) {
    CHECK(row.action != kDitto);
    CHECK(!row.chord.empty());
  }
}

UMBRIEL_TEST(masterLayoutModeIsNamedInTheActionLabel) {
  const std::vector<Keybind> binds = {layoutBind(XKB_KEY_m, umbriel::LayoutMode::Master)};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK(rows[0].action.contains("master"));
}

UMBRIEL_TEST(unnamedSubmapIsNamedInTheActionLabel) {
  Keybind unnamed = bind(KeybindAction::Submap, XKB_KEY_s);
  unnamed.payload = umbriel::SubmapArg{};
  const auto rows = buildCheatsheetRows(std::vector<Keybind>{unnamed});
  CHECK_EQ(rows.size(), size_t{1});
  CHECK_EQ(rows[0].action, std::string{"submap: unnamed"});
}

UMBRIEL_TEST(bindsSharingAnActionMergeWithADitto) {
  // Mod+H and Mod+Left both focus left; the second row shows the chord and a
  // ditto rather than repeating the label.
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h),
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_Left),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  CHECK(rows[0].action != kDitto);
  CHECK(!rows[0].action.empty());
  CHECK_EQ(rows[1].action, kDitto);
  CHECK(rows[0].chord != rows[1].chord);
}

UMBRIEL_TEST(differentModifiersDoNotMerge) {
  // Same action, different modifier: these are distinct entries, not a repeat.
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h),
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h, WLR_MODIFIER_SHIFT),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  CHECK(rows[0].action != kDitto);
  CHECK(rows[1].action != kDitto);
}

UMBRIEL_TEST(spawnIsSplitIntoBinaryAndArguments) {
  const std::vector<Keybind> binds = {spawnBind(XKB_KEY_Return, "/usr/bin/foot -e htop")};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  // The binary is shown separately so several spawns can group under it, and
  // the row itself carries the arguments.
  CHECK_EQ(rows[0].spawnBinary, std::string{"foot"});
  CHECK_EQ(rows[0].spawnArgs, std::string{"-e htop"});
}

UMBRIEL_TEST(spawnWithNoArgumentsFallsBackToTheBinary) {
  const std::vector<Keybind> binds = {spawnBind(XKB_KEY_Return, "foot")};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK_EQ(rows[0].spawnBinary, std::string{"foot"});
  CHECK(rows[0].spawnArgs.empty());
  CHECK_EQ(rows[0].action, std::string{"foot"});
}

UMBRIEL_TEST(spawnsWithDifferentCommandsDoNotMerge) {
  const std::vector<Keybind> binds = {
      spawnBind(XKB_KEY_Return, "foot"),
      spawnBind(XKB_KEY_b, "firefox"),
  };
  CHECK_EQ(buildCheatsheetRows(binds).size(), size_t{2});
}

UMBRIEL_TEST(perDigitWorkspaceBindsCollapseToOneRow) {
  // The defaults bind workspaces 1-9 on both the number row and the keypad. Eighteen rows of near-identical text would
  // swamp the sheet, so they collapse to a single "1…9" row.
  std::vector<Keybind> binds;
  for (int i = 0; i < 9; ++i) {
    const auto digit = static_cast<uint32_t>(XKB_KEY_1 + i);
    const auto keypad = static_cast<uint32_t>(XKB_KEY_KP_1 + i);
    binds.push_back(workspaceBind(digit, std::to_string(i + 1)));
    binds.push_back(workspaceBind(keypad, std::to_string(i + 1)));
  }

  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(countRows(rows, KeybindAction::WorkspaceSwitch), size_t{1});
  CHECK(rows.size() == 1);
  if (!rows.empty()) {
    CHECK(rows[0].chord.contains("1"));
    CHECK(rows[0].action.contains("1-9"));
  }
}

UMBRIEL_TEST(anIncompleteWorkspaceRunIsLeftAlone) {
  // Only three digits bound: collapsing would misreport the range, so the rows
  // stay as they are.
  std::vector<Keybind> binds;
  binds.reserve(3);
  for (int i = 0; i < 3; ++i) {
    binds.push_back(workspaceBind(static_cast<uint32_t>(XKB_KEY_1 + i), std::to_string(i + 1)));
  }
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(countRows(rows, KeybindAction::WorkspaceSwitch), size_t{3});
}

UMBRIEL_TEST(workspaceRunsKeepPostActionSubmapTransitions) {
  std::vector<Keybind> binds;
  for (int i = 0; i < 9; ++i) {
    Keybind workspace = workspaceBind(static_cast<uint32_t>(XKB_KEY_1 + i), std::to_string(i + 1));
    workspace.submapAfter = umbriel::SubmapArg{.name = "inner"};
    binds.push_back(std::move(workspace));
  }

  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK(rows[0].action.contains("inner"));
}

UMBRIEL_TEST(submapBindsCarryTheirSubmap) {
  Keybind inSubmap = bind(KeybindAction::WindowFocusLeft, XKB_KEY_h);
  inSubmap.submap = "resize";
  const std::vector<Keybind> binds = {bind(KeybindAction::WindowFocusLeft, XKB_KEY_h), inSubmap};

  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  // A submap bind never merges with the same action at top level.
  const bool haveTopLevel = std::ranges::any_of(rows, [](const CheatsheetRow& r) { return r.submap.empty(); });
  const bool haveSubmap = std::ranges::any_of(rows, [](const CheatsheetRow& r) { return r.submap == "resize"; });
  CHECK(haveTopLevel);
  CHECK(haveSubmap);
}

UMBRIEL_TEST(postActionSubmapsRemainDistinctAndVisible) {
  Keybind resets = bind(KeybindAction::WindowFocusLeft, XKB_KEY_h);
  resets.submapAfter = umbriel::SubmapArg{.name = "reset"};
  Keybind enters = bind(KeybindAction::WindowFocusLeft, XKB_KEY_Left);
  enters.submapAfter = umbriel::SubmapArg{.name = "inner"};

  const std::vector<CheatsheetRow> rows = buildCheatsheetRows(std::vector<Keybind>{resets, enters});
  CHECK_EQ(rows.size(), size_t{2});
  CHECK(rows[0].action.contains("inner") || rows[1].action.contains("inner"));
  CHECK(rows[0].action.contains("reset") || rows[1].action.contains("reset"));
}

UMBRIEL_TEST(groupTitlesArePlainTextNotMarkup) {
  // This layer is content; escaping belongs to whatever draws it. A title carrying its own entities gets escaped a
  // second time and the reader sees the entity: "Move &amp; size" was rendered exactly like that.
  for (int i = 0; i < 32; ++i) {
    const char* title = umbriel::groupTitle(static_cast<umbriel::Group>(i));
    if (title == nullptr) {
      continue;
    }
    const std::string_view text(title);
    CHECK(!text.contains("&amp;"));
    CHECK(!text.contains('<'));
    CHECK(!text.contains('>'));
  }
}

UMBRIEL_TEST(crossWorkspaceDirectionalActionsUseExpectedGroups) {
  CHECK(umbriel::groupForAction(KeybindAction::WindowFocusOrWorkspaceUp) == umbriel::Group::Focus);
  CHECK(umbriel::groupForAction(KeybindAction::WindowFocusOrWorkspaceDown) == umbriel::Group::Focus);
  CHECK(umbriel::groupForAction(KeybindAction::WindowMoveOrWorkspaceUp) == umbriel::Group::MoveSize);
  CHECK(umbriel::groupForAction(KeybindAction::WindowMoveOrWorkspaceDown) == umbriel::Group::MoveSize);
}

UMBRIEL_TEST(everyActionMapsToAGroupWithATitle) {
  // groupForAction has no default arm to fall through to, so a new action that
  // is never grouped would show up here.
  for (const auto& spec : umbriel::actionSpecs()) {
    const umbriel::Group group = umbriel::groupForAction(spec.action);
    const char* title = umbriel::groupTitle(group);
    CHECK(title != nullptr);
    CHECK(title != nullptr && title[0] != '\0');
  }
}

// column packing: Groups are indivisible, so the panel is as tall as the tallest column and the only lever is where the
// breaks fall. These pin that the split is the best one available rather than a plausible one.

UMBRIEL_TEST(oneColumnHoldsEverything) {
  const std::vector<int> blocks{4, 9, 2};
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 1), 15);
}

UMBRIEL_TEST(anEvenSplitIsFound) {
  const std::vector<int> blocks{5, 5, 5, 5};
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 2), 10);
}

UMBRIEL_TEST(noColumnIsShorterThanItsLargestGroup) {
  // 20 cannot be split, so no number of columns gets the tallest below it.
  const std::vector<int> blocks{2, 20, 2};
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 3), 20);
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 99), 20);
}

UMBRIEL_TEST(aShortGroupLetsItsNeighbourShareTheColumn) {
  // The naive lineCount/numCols target is 22/2 = 11, and snapping back to a group boundary would cut the first column
  // at 10 and leave 12 in the second. Packing 10 + 2 together is better, and is what this must find.
  const std::vector<int> blocks{10, 2, 10};
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 2), 12);
}

UMBRIEL_TEST(moreColumnsAreNeverTaller) {
  const std::vector<int> blocks{3, 7, 2, 8, 4, 6, 1};
  int previous = umbriel::balancedColumnHeight(blocks, 1);
  for (int columns = 2; columns <= 8; ++columns) {
    const int height = umbriel::balancedColumnHeight(blocks, columns);
    CHECK(height <= previous);
    previous = height;
  }
}

UMBRIEL_TEST(theAnswerIsActuallyAchievable) {
  // A height nobody can pack into is not an answer: the optimum must be
  // reachable, and one line less must not be.
  const std::vector<int> blocks{3, 7, 2, 8, 4, 6, 1};
  const int height = umbriel::balancedColumnHeight(blocks, 3);
  CHECK(umbriel::columnsNeededFor(blocks, height) <= 3);
  CHECK(umbriel::columnsNeededFor(blocks, height - 1) > 3);
}

UMBRIEL_TEST(degenerateInputsAreSafe) {
  const std::vector<int> empty;
  CHECK_EQ(umbriel::balancedColumnHeight(empty, 3), 0);
  const std::vector<int> blocks{5, 5};
  // Nonsense column counts fall back to one column rather than dividing by it.
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, 0), 10);
  CHECK_EQ(umbriel::balancedColumnHeight(blocks, -3), 10);
}

int main() { return RUN_TESTS(); }
