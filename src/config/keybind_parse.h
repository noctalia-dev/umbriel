#pragma once

// Keybind vocabulary and the pure text-to-struct parsers over it. Split out of
// config.h so the parsing can be exercised without loading a config file.

#include "layout/layout.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace umbriel {

  enum class WheelDirection {
    None,
    Up,
    Down,
    Left,
    Right,
  };

  enum class KeybindAction {
    None,
    Spawn,
    WindowClose,
    SessionQuit,
    WindowFocusLeft,
    WindowFocusRight,
    WindowFocusUp,
    WindowFocusDown,
    ColumnMoveLeft,
    ColumnMoveRight,
    WindowMoveUp,
    WindowMoveDown,
    WindowConsumeLeft,
    WindowExpelRight,
    WindowCycleWidth,
    WindowCycleWidthBack,
    WindowSetWidth,
    ToggleMaximize,
    ToggleMaximizeToEdges,
    ToggleFullscreen,
    ToggleFloating,
    TogglePinned,
    WindowFocusNext,
    WorkspaceSwitch,
    WindowMoveToWorkspace,
    WindowMoveToWorkspaceNext,
    WindowMoveToWorkspacePrevious,
    ConfigReload,
    KeyboardLayoutNext,
    LayoutScrollLeft,
    LayoutScrollRight,
    LayoutScrollUp,
    LayoutScrollDown,
    OverviewToggle,
    OverviewOpen,
    OverviewClose,
    CheatsheetToggle,
    CheatsheetOpen,
    CheatsheetClose,
    WindowMoveToScratchpad,
    ScratchpadToggle,
    WindowRestoreFromScratchpad,
    ScratchpadFocusNext,
    Submap,
    WindowFocusId,
    WorkspaceNext,
    WorkspacePrevious,
    OutputFocusLeft,
    OutputFocusRight,
    OutputFocusUp,
    OutputFocusDown,
    WindowMoveToOutputLeft,
    WindowMoveToOutputRight,
    WindowMoveToOutputUp,
    WindowMoveToOutputDown,
    ColumnMoveToOutputLeft,
    ColumnMoveToOutputRight,
    ColumnMoveToOutputUp,
    ColumnMoveToOutputDown,
    WorkspaceMoveToOutputLeft,
    WorkspaceMoveToOutputRight,
    WorkspaceMoveToOutputUp,
    WorkspaceMoveToOutputDown,
    WindowModifyWidth,
    WindowCenter,
    WorkspaceSetLayout,
    DpmsOff,
    DpmsOn,
    WorkspaceMoveDown,
    WorkspaceMoveUp,
    Count,
  };

  // Action payloads. Exactly one is valid for a given action, so they live in a variant rather than as sibling fields:
  // a spawn command and a workspace selector can no longer be set at the same time, and the submap name no longer
  // shares storage with the spawn command.
  struct SpawnArg {
    std::string command;
    bool operator==(const SpawnArg&) const = default;
  };
  struct SubmapArg {
    std::string name;
    bool operator==(const SubmapArg&) const = default;
  };
  struct WidthArg {
    double fraction = 0.0;
    bool operator==(const WidthArg&) const = default;
  };
  struct WorkspaceArg {
    std::string name;
    std::string output; // empty = resolve against the focused output
    bool operator==(const WorkspaceArg&) const = default;
  };
  struct OutputArg {
    std::string output; // empty = the focused output
    bool operator==(const OutputArg&) const = default;
  };
  struct WindowIdArg {
    std::string id; // empty = the focused window
    bool operator==(const WindowIdArg&) const = default;
  };
  struct LayoutModeArg {
    std::optional<LayoutMode> mode; // nullopt = toggle between scrolling and dwindle
    bool operator==(const LayoutModeArg&) const = default;
  };
  struct QuitArg {
    bool skipConfirmation = false;
    bool operator==(const QuitArg&) const = default;
  };

  using KeybindPayload = std::variant<
      std::monostate, SpawnArg, SubmapArg, WidthArg, WorkspaceArg, OutputArg, WindowIdArg, LayoutModeArg, QuitArg>;

  struct Keybind {
    // What triggers the bind.
    std::string submap;
    uint32_t modifiers = 0;
    bool useMod = false;
    bool modifierOnly = false;
    uint32_t keysym = 0;
    WheelDirection wheel = WheelDirection::None;
    uint32_t mouseButton = 0; // evdev BTN_* code, 0 = not a mouse bind
    bool repeat = true;

    // What it does.
    KeybindAction action = KeybindAction::None;
    KeybindPayload payload;

    bool operator==(const Keybind&) const = default;
  };

  // Null unless the bind carries that payload alternative.
  template <typename Arg> [[nodiscard]] const Arg* payloadIf(const Keybind& bind) {
    return std::get_if<Arg>(&bind.payload);
  }

  // "reset" pops the current submap instead of pushing a new one. Recognised in
  // both the action and matcher so a default-context bind can be an emergency exit.
  [[nodiscard]] inline bool isSubmapReset(const SubmapArg& arg) { return arg.name == "reset"; }

  [[nodiscard]] inline bool isSubmapResetBind(const Keybind& bind) {
    const auto* arg = payloadIf<SubmapArg>(bind);
    return bind.action == KeybindAction::Submap && arg != nullptr && isSubmapReset(*arg);
  }

  // The cheatsheet's own binds must not dismiss it: the press or chord that
  // opened the overlay would otherwise close it again in the same event.
  [[nodiscard]] inline bool isCheatsheetAction(KeybindAction action) {
    return action == KeybindAction::CheatsheetToggle
        || action == KeybindAction::CheatsheetOpen
        || action == KeybindAction::CheatsheetClose;
  }

  enum class ActionArgKind : uint8_t {
    None,
    Command,
    WidthFraction,
    Workspace,
    OptionalOutput,
    WindowId,
    OptionalWindowId,
    WidthDelta,
    LayoutMode,
    SkipConfirmation
  };

  struct ActionSpec {
    std::string_view name;  // e.g. "spawn", "workspace-switch", "window-close"
    std::string_view param; // "" for simple, "<cmd>" / "<workspace>[/<output>]" for parameterized
    KeybindAction action;
    ActionArgKind argKind = ActionArgKind::None;
  };

  // Parse a chord such as "Mod+Shift+h", "Ctrl+Alt+Delete", "Mod+WheelUp", "Mod+MouseBack", or "submap[resize],Escape".
  // Only the trigger fields are written; the action is set separately by parseAction. Returns false and leaves `output`
  // default-constructed on any malformed input.
  bool parseChord(std::string_view chord, Keybind& output);

  // Parse an action such as "window-close", "spawn:foot", "window-set-width:0.5", or "workspace-switch:2/DP-1", writing
  // the action and its payload into `output` without touching the trigger fields.
  bool parseAction(std::string_view value, Keybind& output);

  std::span<const ActionSpec> actionSpecs();

  // The binds used when no config file supplies any.
  std::vector<Keybind> defaultKeybinds();

} // namespace umbriel
