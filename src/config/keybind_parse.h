#pragma once

// Keybind vocabulary and the pure text-to-struct parsers over it. Split out of
// config.h so the parsing can be exercised without loading a config file.

#include "config/effects.h"
#include "config/window_position.h"
#include "layout/layout.h"
#include "view/window_region.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace umbriel {

  inline constexpr size_t kMaxWorkspaces = 64;

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
    WindowKill,
    WindowMinimize,
    WindowRestoreMinimized,
    WorkspaceToggleShowDesktop,
    SessionQuit,
    WindowFocusLeft,
    WindowFocusRight,
    WindowFocusOrOutputLeft,
    WindowFocusOrOutputRight,
    WindowFocusUp,
    WindowFocusDown,
    WindowFocusOrWorkspaceUp,
    WindowFocusOrWorkspaceDown,
    WindowFocusSwitchFloating,
    WindowFocusOrOutputUp,
    WindowFocusOrOutputDown,
    ColumnMoveLeft,
    ColumnMoveRight,
    ColumnMoveToWorkspace,
    ColumnMoveToWorkspaceNext,
    ColumnMoveToWorkspacePrevious,
    WindowMoveOrOutputLeft,
    WindowMoveOrOutputRight,
    WindowMoveUp,
    WindowMoveDown,
    WindowMoveLeft,
    WindowMoveRight,
    WindowMoveOrWorkspaceUp,
    WindowMoveOrWorkspaceDown,
    WindowMoveOrOutputUp,
    WindowMoveOrOutputDown,
    WindowConsumeLeft,
    WindowConsumeOrExpelLeft,
    WindowConsumeRight,
    WindowConsumeOrExpelRight,
    WindowConsumeFromLeft,
    WindowConsumeFromRight,
    WindowCyclePrimaryExtent,
    WindowCyclePrimaryExtentBack,
    WindowSetPrimaryExtent,
    ToggleMaximize,
    ToggleMaximizeToEdges,
    ToggleFullscreen,
    ToggleFloating,
    TogglePinned,
    WindowFocusNext,
    WorkspaceSwitch,
    WindowMoveToWorkspace,
    WindowMoveToWorkspaceSilent,
    WindowMoveToWorkspaceSilentNext,
    WindowMoveToWorkspaceSilentPrevious,
    WindowMoveToWorkspaceNext,
    WindowMoveToWorkspacePrevious,
    ConfigReload,
    CursorHide,
    CursorShow,
    ScreenCastClear,
    ScreenCastSetOutput,
    ScreenCastSetWindow,
    ScreenCastFollowWindow,
    ScreenCastFollowOutput,
    ScreenCastFollowStop,
    KeyboardLayoutNext,
    ShortcutsInhibitToggle,
    LayoutScrollDrag,
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
    WindowToggleScratchpad,
    ScratchpadFocusNext,
    Submap,
    WindowFocusId,
    WindowFocusWarpId,
    WorkspaceNext,
    WorkspacePrevious,
    OutputFocusLeft,
    OutputFocusRight,
    OutputFocusUp,
    OutputFocusDown,
    OutputFocusNext,
    OutputFocusPrevious,
    WindowMoveToOutputLeft,
    WindowMoveToOutputRight,
    WindowMoveToOutputUp,
    WindowMoveToOutputDown,
    WindowMoveToOutputNext,
    WindowMoveToOutputPrevious,
    ColumnMoveToOutputLeft,
    ColumnMoveToOutputRight,
    ColumnMoveToOutputUp,
    ColumnMoveToOutputDown,
    WorkspaceMoveToOutput,
    WorkspaceMoveToOutputLeft,
    WorkspaceMoveToOutputRight,
    WorkspaceMoveToOutputUp,
    WorkspaceMoveToOutputDown,
    WorkspaceMoveToOutputNext,
    WorkspaceMoveToOutputPrevious,
    WorkspaceSwapActiveOutputLeft,
    WorkspaceSwapActiveOutputRight,
    WorkspaceSwapActiveOutputUp,
    WorkspaceSwapActiveOutputDown,
    WorkspaceSwapActiveOutputNext,
    WorkspaceSwapActiveOutputPrevious,
    WindowModifyPrimaryExtent,
    WindowCenter,
    WindowMoveTo,
    WindowMoveBy,
    WorkspaceSetLayout,
    DpmsOff,
    DpmsOn,
    OutputDisable,
    OutputEnable,
    OutputToggle,
    WorkspaceMoveDown,
    WorkspaceMoveUp,
    ColumnCenter,
    ColumnFocusFirst,
    ColumnFocusLast,
    ColumnMoveToFirst,
    ColumnMoveToLast,
    WindowFocusPrevious,
    WindowSwapNext,
    WindowSwapPrevious,
    WindowSwapLeft,
    WindowSwapRight,
    WindowSwapUp,
    WindowSwapDown,
    LayoutMasterCountIncrease,
    LayoutMasterCountDecrease,
    WindowSetSecondaryExtent,
    WindowModifySecondaryExtent,
    WindowModifyWidthLeft,
    WindowModifyWidthRight,
    WindowModifyHeightUp,
    WindowModifyHeightDown,
    WindowCycleSecondaryExtent,
    WindowCycleSecondaryExtentBack,
    WindowFocusLast,
    WorkspaceFocusLast,
    EffectWindowSet,
    EffectWindowCycle,
    EffectWindowToggle,
    EffectWindowReset,
    EffectBorderSet,
    EffectBorderCycle,
    EffectBorderToggle,
    EffectBorderReset,
    EffectScreenSet,
    EffectScreenCycle,
    EffectScreenToggle,
    EffectScreenReset,
    EffectCursorSet,
    EffectCursorCycle,
    EffectCursorToggle,
    EffectCursorReset,
    ColumnToggleTabbed,
    ColumnSetDisplay,
    ColumnFocusTabNext,
    ColumnFocusTabPrevious,
    ColumnFocusTab,
    ColumnMoveTabNext,
    ColumnMoveTabPrevious,
    ColumnToggleTabBar,
    ColumnShowTabBar,
    ColumnHideTabBar,
    ScratchpadWindowShowNext,
    ScratchpadWindowShowPrevious,
    LayoutToggleSplit,
    OutputSdrWhite,
    WindowSetRegion,
    WindowClearRegion,
    Count,
  };

  // Action payloads. Exactly one is valid for a given action, so they live in a variant.
  struct SpawnArg {
    std::string command;
    bool operator==(const SpawnArg&) const = default;
  };
  struct SubmapArg {
    std::string name;
    bool operator==(const SubmapArg&) const = default;
  };

  inline bool validSubmapName(std::string_view name) {
    return !name.empty() && name != "disable" && !name.contains(']');
  }

  // A positional workspace word is a 1-based index when it is all digits, so a name made only of digits has to go
  // through `--name`.
  inline bool allDigits(std::string_view word) {
    return std::ranges::all_of(word, [](char value) { return value >= '0' && value <= '9'; });
  }

  // The workspace index a positional word denotes, or nullopt when the word is not a usable one.
  inline std::optional<size_t> workspaceSelectorIndex(std::string_view word) {
    if (!allDigits(word)) {
      return std::nullopt;
    }
    size_t index = 0;
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), index);
    if (error != std::errc{} || end != word.data() + word.size() || index < 1 || index > kMaxWorkspaces) {
      return std::nullopt;
    }
    return index;
  }

  // Whether the word grammar accepts a word where it expects a workspace selector.
  inline bool validWorkspaceSelector(std::string_view word) {
    return !allDigits(word) || workspaceSelectorIndex(word).has_value();
  }

  struct FractionArg {
    double fraction = 0.0;
    bool operator==(const FractionArg&) const = default;
  };
  struct WorkspaceIndex {
    size_t value = 0;
    bool operator==(const WorkspaceIndex&) const = default;
  };
  struct WorkspaceName {
    std::string value;
    bool operator==(const WorkspaceName&) const = default;
  };
  using WorkspaceReference = std::variant<WorkspaceIndex, WorkspaceName>;
  struct WorkspaceArg {
    WorkspaceReference reference;
    std::string output; // empty = positions use the cursor-preferred output, names resolve globally
    bool operator==(const WorkspaceArg&) const = default;
  };
  struct OutputArg {
    std::string output; // empty = the focused output
    bool operator==(const OutputArg&) const = default;
  };
  struct WorkspaceMoveToOutputArg {
    std::string destination;
    std::optional<WorkspaceArg> workspace; // absent = the active workspace
    bool operator==(const WorkspaceMoveToOutputArg&) const = default;
  };
  struct ScratchpadArg {
    std::string name; // empty = the implicit default scratchpad
    bool operator==(const ScratchpadArg&) const = default;
  };
  struct WindowIdArg {
    std::string id; // empty = the focused window
    bool operator==(const WindowIdArg&) const = default;
  };
  struct LayoutModeArg {
    std::optional<LayoutMode> mode; // nullopt toggles through toggleSequence
    std::vector<LayoutMode>
        toggleSequence; // distinct modes in toggle order; empty = scrolling, dwindle, master, stacking
    bool operator==(const LayoutModeArg&) const = default;
  };
  struct WindowRegionArg {
    WindowRegion region = WindowRegion::Left;
    bool operator==(const WindowRegionArg&) const = default;
  };
  struct ColumnDisplayArg {
    ColumnDisplay display = ColumnDisplay::Normal;
    bool operator==(const ColumnDisplayArg&) const = default;
  };
  struct TabIndexArg {
    int index = 1; // counted from 1, or from the end when negative
    bool operator==(const TabIndexArg&) const = default;
  };
  struct QuitArg {
    bool skipConfirmation = false;
    bool operator==(const QuitArg&) const = default;
  };
  struct SdrWhiteArg {
    // SDR reference white in cd/m², in the same range the config's `sdr_white` accepts.
    double nits = 0.0;
    std::string output; // empty = the preferred output
    bool operator==(const SdrWhiteArg&) const = default;
  };

  struct EffectWindowArg {
    std::string name; // empty = cycle the underlying pool
    std::string id;   // empty = the focused owner
    bool operator==(const EffectWindowArg&) const = default;
  };
  struct EffectScreenArg {
    std::string name;   // empty = cycle the underlying pool
    std::string output; // empty = the preferred output
    bool operator==(const EffectScreenArg&) const = default;
  };
  struct EffectCursorArg {
    std::string name; // empty = cycle the underlying pool
    bool operator==(const EffectCursorArg&) const = default;
  };
  struct WindowMoveToArg {
    std::string id; // empty = the focused window
    WindowPosition position;
    bool operator==(const WindowMoveToArg&) const = default;
  };
  struct WindowMoveByArg {
    std::string id; // empty = the focused window
    int deltaX = 0;
    int deltaY = 0;
    bool operator==(const WindowMoveByArg&) const = default;
  };

  using KeybindPayload = std::variant<
      std::monostate, SpawnArg, SubmapArg, FractionArg, WorkspaceArg, OutputArg, ScratchpadArg, WindowIdArg,
      LayoutModeArg, ColumnDisplayArg, TabIndexArg, QuitArg, EffectWindowArg, EffectScreenArg, EffectCursorArg,
      SdrWhiteArg, WindowMoveToArg, WindowMoveByArg, WindowRegionArg, WorkspaceMoveToOutputArg>;

  struct Keybind {
    // What triggers the bind.
    std::string submap;
    uint32_t modifiers = 0;
    bool useMod = false;
    bool modifierOnly = false;
    bool onRelease = false;
    uint32_t keysym = 0;
    WheelDirection wheel = WheelDirection::None;
    uint32_t mouseButton = 0; // evdev BTN_* code, 0 = not a mouse bind
    bool repeat = true;
    bool allowWhenLocked = false;
    bool allowWhenInhibited = false;
    int cooldownMs = 0;

    // What it does.
    KeybindAction action = KeybindAction::None;
    KeybindPayload payload;
    // Optional layer transition after the primary action is dispatched.
    std::optional<SubmapArg> submapAfter;

    bool operator==(const Keybind&) const = default;
  };

  // Null unless the bind carries that payload alternative.
  template <typename Arg> const Arg* payloadIf(const Keybind& bind) { return std::get_if<Arg>(&bind.payload); }

  // Pops the current submap instead of pushing a new one. Recognised in both the action and matcher so a
  // default-context bind can be an emergency exit.
  inline constexpr std::string_view kSubmapResetWord = "reset";
  inline bool isSubmapReset(const SubmapArg& arg) { return arg.name == kSubmapResetWord; }

  inline bool isSubmapResetBind(const Keybind& bind) {
    const auto* arg = payloadIf<SubmapArg>(bind);
    return bind.action == KeybindAction::Submap && arg != nullptr && isSubmapReset(*arg);
  }

  // The cheatsheet's own binds must not dismiss it: the press or chord that
  // opened the overlay would otherwise close it again in the same event.
  inline bool isCheatsheetAction(KeybindAction action) {
    return action == KeybindAction::CheatsheetToggle
        || action == KeybindAction::CheatsheetOpen
        || action == KeybindAction::CheatsheetClose;
  }

  enum class ActionArgKind : uint8_t {
    None,
    Command,
    Submap,
    Fraction,
    Workspace,
    WorkspaceMoveToOutput,
    Output,
    OptionalOutput,
    OptionalScratchpad,
    WindowId,
    OptionalWindowId,
    FractionDelta,
    LayoutMode,
    WindowRegion,
    ColumnDisplay,
    TabIndex,
    SkipConfirmation,
    WindowEffectSet,
    WindowEffectCycle,
    ScreenEffectSet,
    ScreenEffectCycle,
    CursorEffectSet,
    CursorEffectCycle,
    SdrWhite,
    WindowMoveTo,
    WindowMoveBy,
  };

  struct ActionSpec {
    std::string_view name;  // e.g. "spawn", "workspace-switch", "window-close"
    std::string_view param; // "" for simple, "<cmd...>" / "<workspace> [--output <output>]" for parameterized
    // One line, no trailing period. `umbriel msg --help` and docs/user/actions.md render it verbatim, and a unit test
    // fails when the doc table and this table disagree.
    std::string_view summary;
    KeybindAction action;
    ActionArgKind argKind = ActionArgKind::None;
  };

  // Evdev BTN_* code for a canonical mouse button name such as "MouseBack", or 0 when the name is not one. Matching is
  // case-insensitive.
  uint32_t mouseButtonFromName(std::string_view name);

  // Canonical name for an evdev BTN_* code, or nullptr for a button Umbriel does not name.
  const char* mouseButtonName(uint32_t button);

  // The BTN_* code binds store for a pressed button. Mice report their thumb buttons as either BTN_SIDE/BTN_EXTRA or
  // BTN_BACK/BTN_FORWARD; both pairs map to BTN_SIDE/BTN_EXTRA.
  uint32_t canonicalMouseButton(uint32_t button);

  // Parse a chord such as "Mod+Shift+h", "Ctrl+Alt+Delete", "Mod+WheelUp", "Mod+MouseBack", or "submap[resize],Escape".
  // Only the trigger fields are written; the action is set separately by parseAction. Returns false and leaves `output`
  // default-constructed on any malformed input.
  bool parseChord(std::string_view chord, Keybind& output);

  // Parse a config action string such as "window-close", "spawn foot -e htop", "window-set-primary-extent 0.5",
  // "workspace-switch 2 --output DP-1", or "workspace-switch --name 2", writing the action and its payload into
  // `output` without touching the trigger fields. Words split like a shell (see splitActionString); `spawn` keeps the
  // raw rest of the string as its command.
  bool parseAction(std::string_view value, Keybind& output);

  // The same grammar over words that are already split, as `umbriel msg` receives them. `words[0]` is the action name;
  // a `spawn` command is the remaining words joined with spaces.
  bool parseActionWords(std::span<const std::string> words, Keybind& output);

  // Splits a config action string into words: whitespace separates words outside quotes, '…' is literal, "…" honours
  // \" and \\, and a backslash outside quotes escapes the next character. Adjacent pieces join into one word. Returns
  // nullopt on an unterminated quote or a trailing lone backslash.
  std::optional<std::vector<std::string>> splitActionString(std::string_view text);

  // Argument values every action encoding shares, so the word grammar and the IPC action request accept the same set.
  std::optional<LayoutMode> layoutModeNamed(std::string_view name);
  std::string_view layoutModeName(LayoutMode mode);
  // Every layout mode name in enum order, and the `column-set-display` words in `ColumnDisplay` order: the parsers and
  // shell completion read one table each.
  std::span<const std::string_view> layoutModeNames();
  // Every layout mode, in the order `workspace-set-layout` cycles them, which `layoutModeNames` spells.
  inline constexpr std::array<LayoutMode, 4> kFullLayoutCycle{
      LayoutMode::Scrolling, LayoutMode::Dwindle, LayoutMode::Master, LayoutMode::Stacking
  };
  std::span<const std::string_view> columnDisplayNames();
  // The `column-set-display` value a word names, or nullopt for anything else.
  std::optional<ColumnDisplay> columnDisplayNamed(std::string_view name);
  // `session-quit`'s only word.
  inline constexpr std::string_view kSkipConfirmationWord = "skip-confirmation";
  // `workspace-set-layout`'s cycle keyword; the modes it cycles come from `layoutModeNames`.
  inline constexpr std::string_view kLayoutModeToggleWord = "toggle";
  // `workspace-move-to-output`'s destination word for the output under the pointer.
  inline constexpr std::string_view kCurrentOutputWord = "current";
  // An argument kind's allowed flags: the bits select, the words are what `--output`, `--window` and `--name` are
  // spelled. `actionAllowedFlags` is the only table that maps a kind to a mask, so the parser rejects exactly the
  // flags shell completion offers.
  inline constexpr std::string_view kActionFlagOutputWord = "--output";
  inline constexpr std::string_view kActionFlagWindowWord = "--window";
  inline constexpr std::string_view kActionFlagNameWord = "--name";
  inline constexpr unsigned kActionFlagOutput = 1U << 0;
  inline constexpr unsigned kActionFlagWindow = 1U << 1;
  inline constexpr unsigned kActionFlagName = 1U << 2;
  unsigned actionAllowedFlags(ActionArgKind kind);
  // The mask a flag word selects, or 0 when the word is no argument flag. The inverse of the words above.
  unsigned actionFlagMask(std::string_view word);
  // A primary or secondary extent: 0.1 up to the widest scrolling column.
  bool validActionFraction(double fraction);
  // A signed, non-zero extent change of at most 0.9.
  bool validActionFractionDelta(double delta);
  // The span the config's `output.<name>.sdr_white` accepts.
  bool validSdrWhiteNits(double nits);

  // Named action roots only; suppression and current-pool cycle have no named reference.
  struct EffectActionReference {
    std::string_view name;
    EffectKind kind;
    bool poolRequired;
  };
  std::optional<EffectActionReference> effectActionReference(const Keybind& bind);
  // The slot an effect action selects into, or nullopt when the action is not an effect action. `Animation` is not a
  // slot, so no action ever names it.
  std::optional<EffectKind> effectSlotKind(KeybindAction action);
  // The most positional words an argument kind's parser reads, or nullopt when it reads an unbounded tail (`spawn`).
  // Reading one more word than this leaves the grammar with nothing, so a position beyond it offers no flags either.
  std::optional<size_t> actionPositionalArity(ActionArgKind kind);
  // Whether the action cycles pools, which needs a pool rather than accepting either.
  bool effectSlotRequiresPool(KeybindAction action);

  std::span<const ActionSpec> actionSpecs();
  // The spec with exactly this name, or null.
  const ActionSpec* findActionSpec(std::string_view name);

  // The binds used when no config file supplies any.
  std::vector<Keybind> defaultKeybinds();

} // namespace umbriel
