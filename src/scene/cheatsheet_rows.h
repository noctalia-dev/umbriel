#pragma once

// Cheatsheet content, independent of how it is drawn. Turning the configured keybinds into display rows is where the
// interesting logic lives: merging binds that share an action, marking repeats with a ditto, splitting a spawn command
// into binary and arguments, and collapsing the per-digit workspace binds into one row. None of that needs pango,
// cairo, or a running compositor, so it lives here and is tested directly.

#include "config/keybind_parse.h"

#include <span>
#include <string>
#include <vector>

namespace umbriel {

  // Column packing: The cheatsheet body is a list of lines split into columns. Groups stay whole: a group broken across
  // a column break reads as two unrelated fragments. So the atoms are runs of lines, one group plus the blank spacer
  // before it, and the only freedom is where the breaks between them go. Pure integer arithmetic over the run lengths,
  // so it is tested directly rather than inferred from a rendered panel.

  // Columns a greedy fill needs when none may exceed `limit` lines. Greedy is optimal: filling each column as far as it
  // goes can never need more columns than holding back would.
  [[nodiscard]] int columnsNeededFor(std::span<const int> blockSizes, int limit);

  // The shortest the tallest column can be, given `numCols` of them. Binary search on the answer. The column count a
  // limit requires only falls as the limit rises, so the smallest limit that still fits in `numCols` is the true
  // optimum, not a heuristic. The lower bound is the largest single block, since no column can be shorter than a group
  // it has to hold whole.
  [[nodiscard]] int balancedColumnHeight(std::span<const int> blockSizes, int numCols);

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
    Focus,
    MoveSize,
    Windows,
    Workspaces,
    Overview,
    System,
    SubmapBase = 100, // submaps start here
  };

  // Plain text, not markup. The caller escapes it for pango; a title that arrived pre-escaped would be escaped a second
  // time and the entity would show up on screen, which is what "Move &amp; size" used to do.
  [[nodiscard]] const char* groupTitle(Group group);
  [[nodiscard]] Group groupForAction(KeybindAction action);

  // One row per chord. Binds sharing an action collapse into a group whose first
  // row carries the label and whose others carry a ditto mark.
  [[nodiscard]] std::vector<CheatsheetRow> buildCheatsheetRows(std::span<const Keybind> keybinds);

} // namespace umbriel
