#include "scene/cheatsheet_rows.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}
// clang-format on

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <string_view>

namespace {

  using umbriel::CheatsheetRow;
  using umbriel::Group;

  const char* wheelName(umbriel::WheelDirection dir) {
    switch (dir) {
    case umbriel::WheelDirection::Up:
      return "WheelUp";
    case umbriel::WheelDirection::Down:
      return "WheelDown";
    case umbriel::WheelDirection::Left:
      return "WheelLeft";
    case umbriel::WheelDirection::Right:
      return "WheelRight";
    default:
      return nullptr;
    }
  }

  const char* mouseButtonName(uint32_t button) {
    switch (button) {
    case BTN_LEFT:
      return "MouseLeft";
    case BTN_RIGHT:
      return "MouseRight";
    case BTN_MIDDLE:
      return "MouseMiddle";
    case BTN_SIDE:
      return "MouseBack";
    case BTN_EXTRA:
      return "MouseForward";
    default:
      return nullptr;
    }
  }

  std::string prettifyKeysym(const char* raw) {
    // Single lowercase ASCII letter -> uppercase.
    if (raw[0] != '\0' && raw[1] == '\0' && raw[0] >= 'a' && raw[0] <= 'z') {
      return std::string(1, static_cast<char>(raw[0] - 'a' + 'A'));
    }

    struct Alias {
      const char* from;
      const char* to;
    };
    static constexpr Alias kAliases[] = {
        {"Left", "\xe2\x86\x90"},  // ←
        {"Right", "\xe2\x86\x92"}, // →
        {"Up", "\xe2\x86\x91"},    // ↑
        {"Down", "\xe2\x86\x93"},  // ↓
        {"comma", ","},
        {"period", "."},
        {"slash", "/"},
        {"minus", "-"},
        {"equal", "="},
        {"semicolon", ";"},
        {"apostrophe", "'"},
        {"grave", "`"},
        {"bracketleft", "["},
        {"bracketright", "]"},
        {"backslash", "\\"},
        {"space", "Space"},
        {"Prior", "PgUp"},
        {"Next", "PgDn"},
    };
    for (const auto& alias : kAliases) {
      if (std::string_view(raw) == alias.from) {
        return alias.to;
      }
    }
    return raw;
  }

  std::string buildChordLabel(const umbriel::Keybind& bind) {
    std::string result;
    auto appendMod = [&](const char* name) {
      if (!result.empty()) {
        result += '+';
      }
      result += name;
    };
    if (bind.useMod) {
      appendMod("Mod");
    }
    if ((bind.modifiers & WLR_MODIFIER_LOGO) != 0) {
      appendMod("Super");
    }
    if ((bind.modifiers & WLR_MODIFIER_CTRL) != 0) {
      appendMod("Ctrl");
    }
    if ((bind.modifiers & WLR_MODIFIER_ALT) != 0) {
      appendMod("Alt");
    }
    if ((bind.modifiers & WLR_MODIFIER_SHIFT) != 0) {
      appendMod("Shift");
    }
    if (bind.modifierOnly) {
      return result;
    }

    if (!result.empty()) {
      result += '+';
    }
    if (bind.wheel != umbriel::WheelDirection::None) {
      const char* name = wheelName(bind.wheel);
      result += name != nullptr ? name : "Wheel?";
    } else if (bind.mouseButton != 0) {
      const char* name = mouseButtonName(bind.mouseButton);
      result += name != nullptr ? name : "Mouse?";
    } else {
      char buf[64];
      xkb_keysym_get_name(bind.keysym, buf, sizeof(buf));
      result += prettifyKeysym(buf);
    }
    return result;
  }

  // Decompose a spawn command into a basename and arguments for compact display.
  // IPC pattern "<binary> msg <args>" collapses the "msg" boilerplate.
  struct SpawnParts {
    std::string binary; // basename of the executable
    std::string args;   // arguments (empty for single-word commands)
  };

  SpawnParts splitSpawnCommand(const std::string& cmd) {
    if (cmd.empty()) {
      return {"", ""};
    }
    std::string_view view(cmd);
    const size_t firstSpace = view.find(' ');
    std::string_view binPath = view.substr(0, firstSpace);
    const size_t slash = binPath.rfind('/');
    std::string base(slash != std::string_view::npos ? binPath.substr(slash + 1) : binPath);
    if (firstSpace == std::string_view::npos) {
      return {std::move(base), ""};
    }
    std::string_view rest = view.substr(firstSpace + 1);
    // Collapse "<binary> msg <args>" -> args only.
    if (rest.starts_with("msg ") && rest.size() > 4) {
      return {std::move(base), std::string(rest.substr(4))};
    }
    return {std::move(base), std::string(rest)};
  }

  // Empty unless the bind targets a workspace. Used to collapse the runs of
  // per-digit workspace binds into a single row.
  std::string workspaceSelectorName(const umbriel::Keybind& bind) {
    const auto* workspace = umbriel::payloadIf<umbriel::WorkspaceArg>(bind);
    return workspace != nullptr ? workspace->name : std::string{};
  }

  // Driven by the spec's argument kind and the bind's payload variant, so the
  // set of parameterized actions lives in exactly one place: the spec table.
  std::string actionLabel(const umbriel::Keybind& bind) {
    for (const auto& spec : umbriel::actionSpecs()) {
      if (spec.action != bind.action) {
        continue;
      }
      std::string name(spec.name);
      switch (spec.argKind) {
      case umbriel::ActionArgKind::None:
        return name;
      case umbriel::ActionArgKind::Command:
        if (const auto* spawn = umbriel::payloadIf<umbriel::SpawnArg>(bind)) {
          return name + ": " + spawn->command;
        }
        if (const auto* submap = umbriel::payloadIf<umbriel::SubmapArg>(bind)) {
          return name + ": " + (submap->name.empty() ? "unnamed" : submap->name);
        }
        return name;
      case umbriel::ActionArgKind::WidthFraction:
        if (const auto* width = umbriel::payloadIf<umbriel::WidthArg>(bind)) {
          return std::format("{}: {:.2g}", name, width->fraction);
        }
        return name;
      case umbriel::ActionArgKind::WidthDelta:
        if (const auto* width = umbriel::payloadIf<umbriel::WidthArg>(bind)) {
          return std::format("{}: {:+.2g}", name, width->fraction);
        }
        return name;
      case umbriel::ActionArgKind::LayoutMode:
        if (const auto* arg = umbriel::payloadIf<umbriel::LayoutModeArg>(bind)) {
          if (arg->mode.has_value()) {
            const auto modeName = [](umbriel::LayoutMode mode) {
              switch (mode) {
              case umbriel::LayoutMode::Scrolling:
                return "scrolling";
              case umbriel::LayoutMode::Dwindle:
                return "dwindle";
              case umbriel::LayoutMode::Master:
                return "master";
              }
              return "scrolling";
            };
            return name + ": " + modeName(*arg->mode);
          }
          return name + ": toggle";
        }
        return name;
      case umbriel::ActionArgKind::Workspace:
        if (const auto* workspace = umbriel::payloadIf<umbriel::WorkspaceArg>(bind)) {
          std::string label = name + ": " + workspace->name;
          if (!workspace->output.empty()) {
            label += "/" + workspace->output;
          }
          return label;
        }
        return name;
      case umbriel::ActionArgKind::OptionalOutput:
        if (const auto* output = umbriel::payloadIf<umbriel::OutputArg>(bind);
            output != nullptr && !output->output.empty()) {
          return name + ": " + output->output;
        }
        return name;
      case umbriel::ActionArgKind::WindowId:
      case umbriel::ActionArgKind::OptionalWindowId:
        if (const auto* window = umbriel::payloadIf<umbriel::WindowIdArg>(bind);
            window != nullptr && !window->id.empty()) {
          return name + ": " + window->id;
        }
        return name;
      case umbriel::ActionArgKind::SkipConfirmation:
        if (const auto* quit = umbriel::payloadIf<umbriel::QuitArg>(bind); quit != nullptr && quit->skipConfirmation) {
          return name + ": skip-confirmation";
        }
        return name;
      }
      return name;
    }
    return "unknown";
  }

  // Row data
  const char* groupTitleImpl(Group group) {
    switch (group) {
    case Group::Apps:
      return "Apps";
    case Group::Focus:
      return "Focus";
    case Group::MoveSize:
      return "Move & size";
    case Group::Windows:
      return "Windows";
    case Group::Workspaces:
      return "Workspaces";
    case Group::Overview:
      return "Overview";
    case Group::System:
      return "System";
    default:
      return nullptr; // submaps handled separately
    }
  }

  Group groupForActionImpl(umbriel::KeybindAction action) {
    using A = umbriel::KeybindAction;
    switch (action) {
    case A::Spawn:
      return Group::Apps;
    case A::WindowFocusLeft:
    case A::WindowFocusRight:
    case A::WindowFocusOrOutputLeft:
    case A::WindowFocusOrOutputRight:
    case A::ColumnFocusFirst:
    case A::ColumnFocusLast:
    case A::WindowFocusUp:
    case A::WindowFocusDown:
    case A::WindowFocusOrWorkspaceUp:
    case A::WindowFocusOrWorkspaceDown:
    case A::WindowFocusOrOutputUp:
    case A::WindowFocusOrOutputDown:
    case A::WindowFocusNext:
    case A::WindowFocusPrevious:
    case A::WindowFocusId:
    case A::WindowFocusWarpId:
    case A::WindowFocusSwitchFloating:
      return Group::Focus;
    case A::ColumnMoveLeft:
    case A::ColumnMoveRight:
    case A::WindowMoveOrOutputLeft:
    case A::WindowMoveOrOutputRight:
    case A::ColumnMoveToFirst:
    case A::ColumnMoveToLast:
    case A::ColumnCenter:
    case A::WindowMoveUp:
    case A::WindowMoveDown:
    case A::WindowMoveOrWorkspaceUp:
    case A::WindowMoveOrWorkspaceDown:
    case A::WindowMoveOrOutputUp:
    case A::WindowMoveOrOutputDown:
    case A::WindowConsumeLeft:
    case A::WindowExpelRight:
    case A::WindowCycleWidth:
    case A::WindowCycleWidthBack:
    case A::WindowSetWidth:
    case A::WindowModifyWidth:
    case A::WindowCenter:
    case A::LayoutScrollLeft:
    case A::LayoutScrollRight:
    case A::WindowMoveToOutputLeft:
    case A::WindowMoveToOutputRight:
    case A::WindowMoveToOutputUp:
    case A::WindowMoveToOutputDown:
    case A::ColumnMoveToOutputLeft:
    case A::ColumnMoveToOutputRight:
    case A::ColumnMoveToOutputUp:
    case A::ColumnMoveToOutputDown:
    case A::WindowSwapNext:
    case A::WindowSwapPrevious:
    case A::MasterCountIncrease:
    case A::MasterCountDecrease:
      return Group::MoveSize;
    case A::WindowClose:
    case A::ToggleFloating:
    case A::ToggleMaximize:
    case A::ToggleMaximizeToEdges:
    case A::ToggleFullscreen:
      return Group::Windows;
    case A::WorkspaceSwitch:
    case A::ColumnMoveToWorkspace:
    case A::ColumnMoveToWorkspaceNext:
    case A::ColumnMoveToWorkspacePrevious:
    case A::WindowMoveToWorkspace:
    case A::WindowMoveToWorkspaceNext:
    case A::WindowMoveToWorkspacePrevious:
    case A::WorkspaceNext:
    case A::WorkspacePrevious:
    case A::WorkspaceMoveDown:
    case A::WorkspaceMoveUp:
    case A::WorkspaceSetLayout:
    case A::WorkspaceMoveToOutputLeft:
    case A::WorkspaceMoveToOutputRight:
    case A::WorkspaceMoveToOutputUp:
    case A::WorkspaceMoveToOutputDown:
      return Group::Workspaces;
    case A::OutputFocusLeft:
    case A::OutputFocusRight:
    case A::OutputFocusUp:
    case A::OutputFocusDown:
      return Group::Focus;
    case A::OverviewToggle:
    case A::OverviewOpen:
    case A::OverviewClose:
      return Group::Overview;
    case A::ConfigReload:
    case A::DpmsOff:
    case A::DpmsOn:
    case A::SessionQuit:
    case A::Submap:
    case A::CheatsheetToggle:
    case A::CheatsheetOpen:
    case A::CheatsheetClose:
      return Group::System;
    default:
      return Group::System;
    }
  }

  // Merge key: groups keybinds with identical action for stacked display.
  struct MergeKey {
    std::string submap;
    std::string submapAfter;
    umbriel::KeybindAction action;
    std::string actionLabel;
    uint32_t modifiers;
    bool useMod;

    bool operator<(const MergeKey& other) const {
      if (submap != other.submap)
        return submap < other.submap;
      if (submapAfter != other.submapAfter)
        return submapAfter < other.submapAfter;
      if (action != other.action)
        return action < other.action;
      if (actionLabel != other.actionLabel)
        return actionLabel < other.actionLabel;
      if (modifiers != other.modifiers)
        return modifiers < other.modifiers;
      return useMod < other.useMod;
    }
  };

} // namespace

namespace umbriel {

  int columnsNeededFor(std::span<const int> blockSizes, int limit) {
    int columns = 1;
    int used = 0;
    for (const int size : blockSizes) {
      if (used > 0 && used + size > limit) {
        ++columns;
        used = 0;
      }
      used += size;
    }
    return columns;
  }

  int balancedColumnHeight(std::span<const int> blockSizes, int numCols) {
    int low = 0;
    int high = 0;
    for (const int size : blockSizes) {
      low = std::max(low, size);
      high += size;
    }
    const int columns = std::max(1, numCols);
    while (low < high) {
      const int mid = low + (high - low) / 2;
      if (columnsNeededFor(blockSizes, mid) <= columns) {
        high = mid;
      } else {
        low = mid + 1;
      }
    }
    return low;
  }

  const char* groupTitle(Group group) { return groupTitleImpl(group); }

  Group groupForAction(KeybindAction action) { return groupForActionImpl(action); }

  std::vector<CheatsheetRow> buildCheatsheetRows(std::span<const Keybind> keybinds) {

    // Collect rows, merging keybinds with identical action into one group. Each group becomes multiple display rows:
    // first shows the action, subsequent rows show the chord with a dim ditto mark.
    struct RawRow {
      std::vector<std::string> chords;
      std::string actionStr;
      KeybindAction actionType;
      std::string submap;
      std::string submapAfter;
      // From the first bind in the group (for workspace collapse).
      uint32_t keysym;
      std::string workspaceName;
      uint32_t modifiers;
      bool useMod;
    };

    std::map<MergeKey, size_t> mergeIndex;
    std::vector<RawRow> rawRows;

    for (const auto& bind : keybinds) {
      if (bind.action == KeybindAction::None) {
        continue;
      }

      std::string chord = buildChordLabel(bind);
      std::string aLabel = actionLabel(bind);
      const std::string submapAfter = bind.submapAfter.has_value() ? bind.submapAfter->name : std::string{};

      MergeKey key{
          .submap = bind.submap,
          .submapAfter = submapAfter,
          .action = bind.action,
          .actionLabel = aLabel,
          .modifiers = bind.modifiers,
          .useMod = bind.useMod,
      };

      auto it = mergeIndex.find(key);
      if (it != mergeIndex.end()) {
        rawRows[it->second].chords.push_back(std::move(chord));
      } else {
        mergeIndex[key] = rawRows.size();
        rawRows.push_back({
            .chords = {std::move(chord)},
            .actionStr = std::move(aLabel),
            .actionType = bind.action,
            .submap = bind.submap,
            .submapAfter = submapAfter,
            .keysym = bind.keysym,
            .workspaceName = workspaceSelectorName(bind),
            .modifiers = bind.modifiers,
            .useMod = bind.useMod,
        });
      }
    }

    // Expand merged groups into display rows: first chord gets the action,
    // additional chords get a dim ditto mark (\u2033).
    std::vector<CheatsheetRow> rows;
    rows.reserve(keybinds.size());
    for (auto& raw : rawRows) {
      // For spawn actions, decompose into binary + args for sub-grouped display.
      std::string displayAction = raw.actionStr;
      SpawnParts spawn;
      if (raw.actionType == KeybindAction::Spawn) {
        constexpr std::string_view kPrefix = "spawn: ";
        if (displayAction.starts_with(kPrefix)) {
          spawn = splitSpawnCommand(std::string(displayAction.substr(kPrefix.size())));
          // Display action is the args (or binary if no args).
          displayAction = spawn.args.empty() ? spawn.binary : spawn.args;
        }
      }
      if (displayAction.size() > 32) {
        displayAction = displayAction.substr(0, 32) + "\xe2\x80\xa6"; // …
      }
      if (!raw.submapAfter.empty()) {
        displayAction += " \xe2\x86\x92 " + raw.submapAfter;
      }
      for (size_t i = 0; i < raw.chords.size(); ++i) {
        rows.push_back({
            .chord = std::move(raw.chords[i]),
            .action = i == 0 ? displayAction : "\xe2\x80\xb3", // ″ ditto
            .actionType = raw.actionType,
            .submap = raw.submap,
            .submapAfter = raw.submapAfter,
            .spawnBinary = spawn.binary,
            .spawnArgs = spawn.args,
            .keysym = raw.keysym,
            .workspaceName = raw.workspaceName,
            .modifiers = raw.modifiers,
            .useMod = raw.useMod,
        });
      }
    }

    // Collapse workspace digit runs for each workspace selector action. Group
    // binds by submap and modifiers, then check for workspace digits 1..9.
    auto collapseWorkspaceRuns = [&](KeybindAction wsAction) {
      struct RunKey {
        std::string submap;
        std::string submapAfter;
        uint32_t modifiers;
        bool useMod;
        bool operator<(const RunKey& o) const {
          if (submap != o.submap)
            return submap < o.submap;
          if (submapAfter != o.submapAfter)
            return submapAfter < o.submapAfter;
          if (modifiers != o.modifiers)
            return modifiers < o.modifiers;
          return useMod < o.useMod;
        }
      };
      std::map<RunKey, std::vector<size_t>> groups;
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].actionType != wsAction)
          continue;
        if (!rows[i].submap.empty())
          continue; // submaps handled separately
        RunKey rk{rows[i].submap, rows[i].submapAfter, rows[i].modifiers, rows[i].useMod};
        groups[rk].push_back(i);
      }

      for (auto& [rk, indices] : groups) {
        // Check if we have the full 1..9 digit run.
        std::vector<size_t> digitIndices;
        std::vector<size_t> kpIndices;
        for (size_t idx : indices) {
          const auto& row = rows[idx];
          if (row.keysym >= XKB_KEY_1
              && row.keysym <= XKB_KEY_9
              && row.workspaceName == std::to_string(row.keysym - XKB_KEY_1 + 1)) {
            digitIndices.push_back(idx);
          } else if (row.keysym >= XKB_KEY_KP_1 && row.keysym <= XKB_KEY_KP_9) {
            kpIndices.push_back(idx);
          }
        }
        if (digitIndices.size() < 9) {
          continue; // no full run
        }

        // Build the modifier prefix for the collapsed row.
        std::string modPrefix;
        if (rk.useMod) {
          modPrefix = "Mod";
        }
        if ((rk.modifiers & WLR_MODIFIER_LOGO) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Super";
        }
        if ((rk.modifiers & WLR_MODIFIER_CTRL) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Ctrl";
        }
        if ((rk.modifiers & WLR_MODIFIER_ALT) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Alt";
        }
        if ((rk.modifiers & WLR_MODIFIER_SHIFT) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Shift";
        }
        if (!modPrefix.empty())
          modPrefix += '+';

        // Find the action spec name.
        std::string_view specName;
        for (const auto& spec : actionSpecs()) {
          if (spec.action == wsAction) {
            specName = spec.name;
            break;
          }
        }

        // Replace the first digit index row with the collapsed version.
        rows[digitIndices[0]].chord = modPrefix
            + "1\xe2\x80\xa6"
              "9"; // 1…9
        rows[digitIndices[0]].action = std::string(specName) + ": 1-9";
        if (!rk.submapAfter.empty()) {
          rows[digitIndices[0]].action += " \xe2\x86\x92 " + rk.submapAfter;
        }

        // Mark remaining digit rows and all KP rows for deletion.
        std::vector<size_t> toRemove;
        for (size_t i = 1; i < digitIndices.size(); ++i) {
          toRemove.push_back(digitIndices[i]);
        }
        for (size_t idx : kpIndices) {
          toRemove.push_back(idx);
        }
        std::ranges::sort(toRemove, std::greater<>());
        for (size_t idx : toRemove) {
          rows.erase(rows.begin() + static_cast<ptrdiff_t>(idx));
        }
      }
    };

    collapseWorkspaceRuns(KeybindAction::WorkspaceSwitch);
    collapseWorkspaceRuns(KeybindAction::ColumnMoveToWorkspace);
    collapseWorkspaceRuns(KeybindAction::WindowMoveToWorkspace);

    return rows;
  }

} // namespace umbriel
