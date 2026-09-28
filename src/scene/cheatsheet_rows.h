#pragma once

// Cheatsheet content, independent of drawing: turns the configured keybinds into display rows, merging binds that share
// an action, marking repeats with a ditto, splitting a spawn command into binary and arguments, and collapsing the
// per-digit workspace binds into one row. Needs no pango, cairo, or running compositor.

#include "config/keybind_parse.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  // Column packing. The body is a list of lines split into columns. The atoms are blocks, one group plus the blank
  // spacer before it, so a group is never split and the only freedom is where the breaks between blocks go.

  // Columns a greedy fill needs when none may exceed `limit` lines. Greedy is optimal: filling each column as far as it
  // goes can never need more columns than holding back would.
  [[nodiscard]] int columnsNeededFor(std::span<const int> blockSizes, int limit);

  // The shortest the tallest column can be, given `numCols` of them, found by binary search: the column count a limit
  // requires only falls as the limit rises, so the smallest limit that fits is optimal. The lower bound is the largest
  // block, since each group is held whole.
  [[nodiscard]] int balancedColumnHeight(std::span<const int> blockSizes, int numCols);

  // Display columns occupied by a generated chord in the cheatsheet's
  // monospace font.
  [[nodiscard]] size_t cheatsheetChordColumns(std::string_view chord);

  struct CheatsheetRow {
    std::string chord;  // display chord(s)
    std::string action; // display action (full label for non-spawn, args-only for spawn)
    KeybindAction actionType = KeybindAction::None;
    std::string submap;      // source submap (empty = top-level)
    std::string submapAfter; // optional transition after the action
    std::string spawnBinary; // basename of spawn command (empty for non-spawn)
    std::string spawnArgs;   // args portion of spawn command
    // For workspace collapse detection.
    uint32_t keysym = 0;
    std::string workspaceName;
    uint32_t modifiers = 0;
    bool useMod = false;
  };

  // Group assignment.
  enum class Group : int {
    Apps = 0,
    Screencasting,
    Focus,
    MoveSize,
    Windows,
    Scratchpad,
    Workspaces,
    Overview,
    System,
    SubmapBase = 100, // submaps start here
  };

  // Plain text, not markup: the caller escapes it for pango, so a pre-escaped title would show its entities on screen.
  [[nodiscard]] const char* groupTitle(Group group);
  [[nodiscard]] Group groupForAction(KeybindAction action);

  // Display order of the non-submap groups, shared by the cheatsheet overlay and `umbriel msg --help` so the two never
  // present the same actions in a different shape. Submap groups follow in first-seen order and are not listed here.
  [[nodiscard]] std::span<const Group> fixedGroupOrder();

  // One row per chord. Binds sharing an action collapse into a group whose first
  // row carries the label and whose others carry a ditto mark.
  [[nodiscard]] std::vector<CheatsheetRow> buildCheatsheetRows(std::span<const Keybind> keybinds);

} // namespace umbriel
