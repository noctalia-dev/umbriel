#include "config/keybind_parse.h"

// <cmath> must precede the Wayland headers to avoid a libstdc++ 16 include-order failure.
// clang-format off
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
// WLR_MODIFIER_* only. Pulling src/wlr.h would drag SceneFX and the renderer
// into a translation unit that parses strings.
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}
// clang-format on

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <ranges>
#include <system_error>
#include <utility>

namespace umbriel {

  namespace {

    std::string toLower(std::string_view text) {
      std::string lowered(text);
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return lowered;
    }

    WheelDirection wheelFromName(std::string_view lowered) {
      if (lowered == "wheelup") {
        return WheelDirection::Up;
      }
      if (lowered == "wheeldown") {
        return WheelDirection::Down;
      }
      if (lowered == "wheelleft") {
        return WheelDirection::Left;
      }
      if (lowered == "wheelright") {
        return WheelDirection::Right;
      }
      return WheelDirection::None;
    }

    bool applyModifier(std::string_view token, Keybind& output) {
      const std::string modifier = toLower(token);
      if (modifier == "mod") {
        output.useMod = true;
      } else if (modifier == "shift") {
        output.modifiers |= WLR_MODIFIER_SHIFT;
      } else if (modifier == "ctrl" || modifier == "control") {
        output.modifiers |= WLR_MODIFIER_CTRL;
      } else if (modifier == "alt") {
        output.modifiers |= WLR_MODIFIER_ALT;
      } else if (modifier == "super" || modifier == "logo" || modifier == "win") {
        output.modifiers |= WLR_MODIFIER_LOGO;
      } else {
        return false;
      }
      return true;
    }

    // Fold every token but the last into the bind's modifier state.
    bool applyModifiers(const std::vector<std::string_view>& tokens, Keybind& output) {
      for (size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (!applyModifier(tokens[index], output)) {
          return false;
        }
      }
      return true;
    }

    // Strip a leading "submap[name]" (with optional trailing comma) and record it.
    // Returns false when the prefix is present but malformed.
    bool takeSubmapPrefix(std::string_view& chord, Keybind& output) {
      constexpr std::string_view kPrefix = "submap[";
      if (!chord.starts_with(kPrefix)) {
        return true;
      }
      const size_t closeBracket = chord.find(']');
      if (closeBracket == std::string_view::npos) {
        return false;
      }
      output.submap = chord.substr(kPrefix.size(), closeBracket - kPrefix.size());
      if (output.submap.empty()) {
        return false;
      }
      size_t restStart = closeBracket + 1;
      if (restStart < chord.size() && chord[restStart] == ',') {
        ++restStart;
      }
      if (restStart >= chord.size()) {
        return false;
      }
      chord = chord.substr(restStart);
      return true;
    }

    bool splitChordTokens(std::string_view chord, std::vector<std::string_view>& tokens) {
      size_t start = 0;
      while (start <= chord.size()) {
        const size_t separator = chord.find('+', start);
        const size_t end = separator == std::string_view::npos ? chord.size() : separator;
        const std::string_view token = chord.substr(start, end - start);
        if (token.empty()) {
          return false;
        }
        tokens.push_back(token);
        if (separator == std::string_view::npos) {
          break;
        }
        start = separator + 1;
      }
      return !tokens.empty();
    }

    constexpr std::string_view kActionSpace = " \t\n\r";

    bool isActionSpace(char character) { return kActionSpace.contains(character); }

    constexpr unsigned kFlagOutput = 1U << 0;
    constexpr unsigned kFlagWindow = 1U << 1;
    constexpr unsigned kFlagName = 1U << 2;

    // An action's argument words, sorted into positional words and the flags its kind allows.
    struct ActionWords {
      std::vector<std::string_view> positional;
      std::optional<std::string_view> output;
      std::optional<std::string_view> window;
      std::optional<std::string_view> name;
    };

    // A word starting with "--" is always a flag; its value is the next word. Empty words, unknown or repeated flags,
    // and flags without a value are rejected.
    std::optional<ActionWords> splitActionWords(std::span<const std::string> words, unsigned allowed) {
      ActionWords result;
      for (size_t index = 0; index < words.size(); ++index) {
        const std::string_view word = words[index];
        if (word.empty()) {
          return std::nullopt;
        }
        if (!word.starts_with("--")) {
          result.positional.push_back(word);
          continue;
        }
        std::optional<std::string_view>* slot = nullptr;
        if (word == "--output" && (allowed & kFlagOutput) != 0) {
          slot = &result.output;
        } else if (word == "--window" && (allowed & kFlagWindow) != 0) {
          slot = &result.window;
        } else if (word == "--name" && (allowed & kFlagName) != 0) {
          slot = &result.name;
        }
        if (slot == nullptr || slot->has_value() || index + 1 >= words.size() || words[index + 1].empty()) {
          return std::nullopt;
        }
        *slot = words[++index];
      }
      return result;
    }

    // Zero or one positional word and no flags.
    std::optional<std::string_view> optionalWord(std::span<const std::string> words) {
      const auto split = splitActionWords(words, 0);
      if (!split || split->positional.size() > 1) {
        return std::nullopt;
      }
      return split->positional.empty() ? std::string_view{} : split->positional.front();
    }

    // Exactly one positional word and no flags.
    std::optional<std::string_view> singleWord(std::span<const std::string> words) {
      const auto word = optionalWord(words);
      if (!word || word->empty()) {
        return std::nullopt;
      }
      return word;
    }

    // Exactly one of a positional selector or `--name`, plus an optional `--output`. All-digit positional selectors
    // are 1-based positions; `--name` forces a name, including one made only of digits.
    std::optional<WorkspaceArg> parseWorkspaceSelector(const ActionWords& split) {
      if (split.positional.size() + (split.name ? 1U : 0U) != 1) {
        return std::nullopt;
      }
      WorkspaceArg workspace;
      workspace.output = std::string(split.output.value_or(std::string_view{}));
      if (split.name) {
        workspace.reference = WorkspaceName{std::string(*split.name)};
        return workspace;
      }
      const std::string_view selector = split.positional.front();
      if (std::ranges::all_of(selector, [](char value) { return value >= '0' && value <= '9'; })) {
        size_t index = 0;
        const auto [end, error] = std::from_chars(selector.data(), selector.data() + selector.size(), index);
        if (error != std::errc{} || end != selector.data() + selector.size() || index < 1 || index > kMaxWorkspaces) {
          return std::nullopt;
        }
        workspace.reference = WorkspaceIndex{index};
        return workspace;
      }
      workspace.reference = WorkspaceName{std::string(selector)};
      return workspace;
    }

    // A signed pixel count; std::from_chars rejects a leading '+', so it is stripped first.
    bool parseSignedPixels(std::string_view text, int& output) {
      if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
      }
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
      return error == std::errc{} && end == text.data() + text.size();
    }

    std::optional<double> parseFraction(std::string_view text) {
      double fraction = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), fraction);
      if (error != std::errc{} || end != text.data() + text.size() || !validActionFraction(fraction)) {
        return std::nullopt;
      }
      return fraction;
    }

    std::optional<double> parseFractionDelta(std::string_view text) {
      // std::from_chars rejects a leading '+', but the delta is signed.
      if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
      }
      double delta = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), delta);
      if (error != std::errc{} || end != text.data() + text.size() || !validActionFractionDelta(delta)) {
        return std::nullopt;
      }
      return delta;
    }

    std::optional<LayoutModeArg> parseLayoutModeWords(std::span<const std::string> words) {
      const auto split = splitActionWords(words, 0);
      if (!split || split->positional.empty()) {
        return std::nullopt;
      }
      const auto& positional = split->positional;
      if (positional.size() == 1) {
        if (const auto mode = layoutModeNamed(positional.front())) {
          return LayoutModeArg{.mode = mode, .toggleSequence = {}};
        }
        if (positional.front() == "toggle") {
          return LayoutModeArg{};
        }
        return std::nullopt;
      }
      if (positional.front() != "toggle") {
        return std::nullopt;
      }
      std::vector<LayoutMode> sequence;
      for (const std::string_view name : positional | std::views::drop(1)) {
        const auto mode = layoutModeNamed(name);
        if (!mode || std::ranges::contains(sequence, *mode)) {
          return std::nullopt;
        }
        sequence.push_back(*mode);
      }
      if (sequence.size() < 2) {
        return std::nullopt;
      }
      return LayoutModeArg{.mode = std::nullopt, .toggleSequence = std::move(sequence)};
    }

    std::optional<KeybindPayload> parseEffectWords(ActionArgKind kind, std::span<const std::string> words) {
      const bool cycle = kind == ActionArgKind::WindowEffectCycle
          || kind == ActionArgKind::ScreenEffectCycle
          || kind == ActionArgKind::CursorEffectCycle;
      const bool cursor = kind == ActionArgKind::CursorEffectSet || kind == ActionArgKind::CursorEffectCycle;
      const bool screen = kind == ActionArgKind::ScreenEffectSet || kind == ActionArgKind::ScreenEffectCycle;
      const unsigned flags = cursor ? 0U : (screen ? kFlagOutput : kFlagWindow);
      const auto split = splitActionWords(words, flags);
      if (!split || split->positional.size() > 1) {
        return std::nullopt;
      }
      const std::string name(split->positional.empty() ? std::string_view{} : split->positional.front());
      if ((!cycle && name.empty()) || (cycle && name == kEffectOff)) {
        return std::nullopt;
      }
      if (cursor) {
        return EffectCursorArg{.name = name};
      }
      if (screen) {
        return EffectScreenArg{.name = name, .output = std::string(split->output.value_or(std::string_view{}))};
      }
      return EffectWindowArg{.name = name, .id = std::string(split->window.value_or(std::string_view{}))};
    }

    std::optional<KeybindPayload> parseSdrWhiteWords(std::span<const std::string> words) {
      const auto split = splitActionWords(words, kFlagOutput);
      if (!split || split->positional.size() != 1) {
        return std::nullopt;
      }
      const std::string_view text = split->positional.front();
      double nits = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), nits);
      if (error != std::errc{} || end != text.data() + text.size() || !validSdrWhiteNits(nits)) {
        return std::nullopt;
      }
      return SdrWhiteArg{.nits = nits, .output = std::string(split->output.value_or(std::string_view{}))};
    }

    std::optional<KeybindPayload> parseWindowMoveToWords(std::span<const std::string> words) {
      const auto split = splitActionWords(words, kFlagWindow);
      if (!split || split->positional.size() != 3) {
        return std::nullopt;
      }
      const std::optional<WindowPositionAnchor> anchor = windowPositionAnchorFromName(split->positional[0]);
      WindowPosition position;
      if (!anchor
          || !parseSignedPixels(split->positional[1], position.x)
          || !parseSignedPixels(split->positional[2], position.y)) {
        return std::nullopt;
      }
      position.anchor = *anchor;
      return WindowMoveToArg{.id = std::string(split->window.value_or(std::string_view{})), .position = position};
    }

    std::optional<KeybindPayload> parseWindowMoveByWords(std::span<const std::string> words) {
      const auto split = splitActionWords(words, kFlagWindow);
      int deltaX = 0;
      int deltaY = 0;
      if (!split
          || split->positional.size() != 2
          || !parseSignedPixels(split->positional[0], deltaX)
          || !parseSignedPixels(split->positional[1], deltaY)) {
        return std::nullopt;
      }
      return WindowMoveByArg{
          .id = std::string(split->window.value_or(std::string_view{})), .deltaX = deltaX, .deltaY = deltaY
      };
    }

    constexpr ActionSpec kActionSpecs[] = {
        {"cheatsheet-close", "", "Hide the keybind cheatsheet", KeybindAction::CheatsheetClose},
        {"cheatsheet-open", "", "Show the keybind cheatsheet", KeybindAction::CheatsheetOpen},
        {"cheatsheet-toggle", "", "Show or hide the keybind cheatsheet", KeybindAction::CheatsheetToggle},
        {"column-center", "", "Center the focused column in the viewport", KeybindAction::ColumnCenter},
        {"column-focus-first", "", "Focus the first column in the workspace", KeybindAction::ColumnFocusFirst},
        {"column-focus-last", "", "Focus the last column in the workspace", KeybindAction::ColumnFocusLast},
        {"column-focus-tab", "<index>", "Focus a tab of the focused tab group by its position",
         KeybindAction::ColumnFocusTab, ActionArgKind::TabIndex},
        {"column-focus-tab-next", "", "Focus the next tab in the focused tab group", KeybindAction::ColumnFocusTabNext},
        {"column-focus-tab-previous", "", "Focus the previous tab in the focused tab group",
         KeybindAction::ColumnFocusTabPrevious},
        {"column-hide-tab-bar", "", "Hide the focused tab group's bar; its tabs take the space",
         KeybindAction::ColumnHideTabBar},
        {"column-move-left", "", "Move the focused column one position left", KeybindAction::ColumnMoveLeft},
        {"column-move-right", "", "Move the focused column one position right", KeybindAction::ColumnMoveRight},
        {"column-move-tab-next", "", "Move the focused tab one place later among its tabs",
         KeybindAction::ColumnMoveTabNext},
        {"column-move-tab-previous", "", "Move the focused tab one place earlier among its tabs",
         KeybindAction::ColumnMoveTabPrevious},
        {"column-move-to-first", "", "Move the focused column to the first position", KeybindAction::ColumnMoveToFirst},
        {"column-move-to-last", "", "Move the focused column to the last position", KeybindAction::ColumnMoveToLast},
        {"column-move-to-output-down", "", "Move the focused column to the output below",
         KeybindAction::ColumnMoveToOutputDown},
        {"column-move-to-output-left", "", "Move the focused column to the output left",
         KeybindAction::ColumnMoveToOutputLeft},
        {"column-move-to-output-right", "", "Move the focused column to the output right",
         KeybindAction::ColumnMoveToOutputRight},
        {"column-move-to-output-up", "", "Move the focused column to the output above",
         KeybindAction::ColumnMoveToOutputUp},
        {"column-move-to-workspace", "<workspace> [--output <output>]",
         "Move the focused column to the selected workspace", KeybindAction::ColumnMoveToWorkspace,
         ActionArgKind::Workspace},
        {"column-move-to-workspace-next", "", "Move the focused column to the next workspace",
         KeybindAction::ColumnMoveToWorkspaceNext},
        {"column-move-to-workspace-previous", "", "Move the focused column to the previous workspace",
         KeybindAction::ColumnMoveToWorkspacePrevious},
        {"column-set-display", "<normal|tabbed>", "Show the rows around the focused window as tabs or stacked",
         KeybindAction::ColumnSetDisplay, ActionArgKind::ColumnDisplay},
        {"column-show-tab-bar", "", "Show the focused tab group's bar", KeybindAction::ColumnShowTabBar},
        {"column-toggle-tab-bar", "", "Show or hide the focused tab group's bar", KeybindAction::ColumnToggleTabBar},
        {"column-toggle-tabbed", "", "Tab the rows around the focused window, or stack its tabs",
         KeybindAction::ColumnToggleTabbed},
        {"config-reload", "", "Reload the configuration file", KeybindAction::ConfigReload},
        {"cursor-hide", "", "Hide the cursor until pointer activity", KeybindAction::CursorHide},
        {"cursor-show", "", "Show the cursor and restart its inactivity timeout", KeybindAction::CursorShow},
        {"dpms-off", "[<output>]", "Power off one output, or every output when bare", KeybindAction::DpmsOff,
         ActionArgKind::OptionalOutput},
        {"dpms-on", "[<output>]", "Power on one output, or every output when bare", KeybindAction::DpmsOn,
         ActionArgKind::OptionalOutput},
        {"effect-border-cycle", "[<pool>] [--window <window-id>]", "Cycle the current or named border pool",
         KeybindAction::EffectBorderCycle, ActionArgKind::WindowEffectCycle},
        {"effect-border-reset", "[<window-id>]", "Clear runtime state and resolve configuration",
         KeybindAction::EffectBorderReset, ActionArgKind::OptionalWindowId},
        {"effect-border-set", "<name> [--window <window-id>]", "Select a border preset/pool, or suppress with off",
         KeybindAction::EffectBorderSet, ActionArgKind::WindowEffectSet},
        {"effect-border-toggle", "[<window-id>]", "Toggle border-slot suppression", KeybindAction::EffectBorderToggle,
         ActionArgKind::OptionalWindowId},
        {"effect-cursor-cycle", "[<pool>]", "Cycle the current or named cursor pool", KeybindAction::EffectCursorCycle,
         ActionArgKind::CursorEffectCycle},
        {"effect-cursor-reset", "", "Clear runtime state and resolve configuration", KeybindAction::EffectCursorReset,
         ActionArgKind::None},
        {"effect-cursor-set", "<name>", "Select a cursor preset/pool, or suppress with off",
         KeybindAction::EffectCursorSet, ActionArgKind::CursorEffectSet},
        {"effect-cursor-toggle", "", "Toggle cursor-slot suppression", KeybindAction::EffectCursorToggle,
         ActionArgKind::None},
        {"effect-screen-cycle", "[<pool>] [--output <output>]", "Cycle the current or named screen pool",
         KeybindAction::EffectScreenCycle, ActionArgKind::ScreenEffectCycle},
        {"effect-screen-reset", "[<output>]", "Clear runtime state and resolve configuration",
         KeybindAction::EffectScreenReset, ActionArgKind::OptionalOutput},
        {"effect-screen-set", "<name> [--output <output>]", "Select a screen preset/pool, or suppress with off",
         KeybindAction::EffectScreenSet, ActionArgKind::ScreenEffectSet},
        {"effect-screen-toggle", "[<output>]", "Toggle screen-slot suppression", KeybindAction::EffectScreenToggle,
         ActionArgKind::OptionalOutput},
        {"effect-window-cycle", "[<pool>] [--window <window-id>]", "Cycle the current or named window pool",
         KeybindAction::EffectWindowCycle, ActionArgKind::WindowEffectCycle},
        {"effect-window-reset", "[<window-id>]", "Clear runtime state and resolve configuration",
         KeybindAction::EffectWindowReset, ActionArgKind::OptionalWindowId},
        {"effect-window-set", "<name> [--window <window-id>]", "Select a window preset/pool, or suppress with off",
         KeybindAction::EffectWindowSet, ActionArgKind::WindowEffectSet},
        {"effect-window-toggle", "[<window-id>]", "Toggle window-slot suppression", KeybindAction::EffectWindowToggle,
         ActionArgKind::OptionalWindowId},
        {"keyboard-layout-next", "", "Switch one keyboard to its next configured layout",
         KeybindAction::KeyboardLayoutNext},
        {"layout-master-count-decrease", "", "Demote the last master window to the stack",
         KeybindAction::LayoutMasterCountDecrease},
        {"layout-master-count-increase", "", "Promote the first stack window to master",
         KeybindAction::LayoutMasterCountIncrease},
        {"layout-scroll-down", "", "Scroll the strip toward its end", KeybindAction::LayoutScrollDown},
        {"layout-scroll-drag", "", "Pan the strip while the bound button is held", KeybindAction::LayoutScrollDrag},
        {"layout-scroll-left", "", "Scroll the strip toward its start", KeybindAction::LayoutScrollLeft},
        {"layout-scroll-right", "", "Scroll the strip toward its end", KeybindAction::LayoutScrollRight},
        {"layout-scroll-up", "", "Scroll the strip toward its start", KeybindAction::LayoutScrollUp},
        {"layout-toggle-split", "", "Toggle the focused tile's dwindle split direction",
         KeybindAction::LayoutToggleSplit},
        {"output-disable", "<output>", "Remove an output from the desktop", KeybindAction::OutputDisable,
         ActionArgKind::Output},
        {"output-enable", "<output>", "Add an output to the desktop", KeybindAction::OutputEnable,
         ActionArgKind::Output},
        {"output-focus-down", "", "Focus the output below", KeybindAction::OutputFocusDown},
        {"output-focus-left", "", "Focus the output to the left", KeybindAction::OutputFocusLeft},
        {"output-focus-next", "", "Focus the next output, wrapping around", KeybindAction::OutputFocusNext},
        {"output-focus-previous", "", "Focus the previous output, wrapping around", KeybindAction::OutputFocusPrevious},
        {"output-focus-right", "", "Focus the output to the right", KeybindAction::OutputFocusRight},
        {"output-focus-up", "", "Focus the output above", KeybindAction::OutputFocusUp},
        {"output-sdr-white", "<nits> [--output <output>]", "Set an output's live SDR reference white in cd/m2",
         KeybindAction::OutputSdrWhite, ActionArgKind::SdrWhite},
        {"output-toggle", "<output>", "Add or remove an output from the desktop", KeybindAction::OutputToggle,
         ActionArgKind::Output},
        {"overview-close", "", "Close the workspace overview", KeybindAction::OverviewClose},
        {"overview-open", "", "Open the workspace overview", KeybindAction::OverviewOpen},
        {"overview-toggle", "", "Open or close the workspace overview", KeybindAction::OverviewToggle},
        {"scratchpad-focus-next", "[<scratchpad>]", "Focus the next visible scratchpad window",
         KeybindAction::ScratchpadFocusNext, ActionArgKind::OptionalScratchpad},
        {"scratchpad-toggle", "[<scratchpad>]", "Show or hide the selected scratchpad windows",
         KeybindAction::ScratchpadToggle, ActionArgKind::OptionalScratchpad},
        {"scratchpad-window-show-next", "[<scratchpad>]", "Show only the next scratchpad window",
         KeybindAction::ScratchpadWindowShowNext, ActionArgKind::OptionalScratchpad},
        {"scratchpad-window-show-previous", "[<scratchpad>]", "Show only the previous scratchpad window",
         KeybindAction::ScratchpadWindowShowPrevious, ActionArgKind::OptionalScratchpad},
        {"screencast-clear", "", "Pause the screencast and stop following", KeybindAction::ScreenCastClear},
        {"screencast-follow-output", "", "Follow the focused output", KeybindAction::ScreenCastFollowOutput},
        {"screencast-follow-stop", "", "Stop following and keep the current target",
         KeybindAction::ScreenCastFollowStop},
        {"screencast-follow-window", "", "Follow the focused window", KeybindAction::ScreenCastFollowWindow},
        {"screencast-set-output", "[<output>]", "Share the focused output, or the selected output",
         KeybindAction::ScreenCastSetOutput, ActionArgKind::OptionalOutput},
        {"screencast-set-window", "[<window-id>]", "Share the focused window, or the selected window",
         KeybindAction::ScreenCastSetWindow, ActionArgKind::OptionalWindowId},
        {"session-quit", "[skip-confirmation]", "Quit the session, confirming first unless told to skip",
         KeybindAction::SessionQuit, ActionArgKind::SkipConfirmation},
        {"shortcuts-inhibit-toggle", "", "Toggle shortcuts inhibition for the focused surface",
         KeybindAction::ShortcutsInhibitToggle},
        {"spawn", "<cmd...>", "Run a command with a launch activation token", KeybindAction::Spawn,
         ActionArgKind::Command},
        {"submap", "<name>", "Enter a submap layer, or leave one with 'reset'", KeybindAction::Submap,
         ActionArgKind::Submap},
        {"window-center", "", "Center the focused floating window on its output", KeybindAction::WindowCenter},
        {"window-clear-region", "", "Restore the window's normal position and size", KeybindAction::WindowClearRegion},
        {"window-close", "[<window-id>]", "Close the focused window, or the given window", KeybindAction::WindowClose,
         ActionArgKind::OptionalWindowId},
        {"window-consume-from-left", "", "Pull the left column's window into the focused column",
         KeybindAction::WindowConsumeFromLeft},
        {"window-consume-from-right", "", "Pull the right column's window into the focused column",
         KeybindAction::WindowConsumeFromRight},
        {"window-consume-left", "", "Stack the focused window into the column left", KeybindAction::WindowConsumeLeft},
        {"window-consume-or-expel-left", "", "Split the window out, or stack it into the column left",
         KeybindAction::WindowConsumeOrExpelLeft},
        {"window-consume-or-expel-right", "", "Split the window out, or stack it into the column right",
         KeybindAction::WindowConsumeOrExpelRight},
        {"window-consume-right", "", "Stack the focused window into the column right",
         KeybindAction::WindowConsumeRight},
        {"window-cycle-primary-extent", "", "Cycle the focused area's primary extent through presets",
         KeybindAction::WindowCyclePrimaryExtent},
        {"window-cycle-primary-extent-back", "", "Cycle the primary extent presets in reverse",
         KeybindAction::WindowCyclePrimaryExtentBack},
        {"window-cycle-secondary-extent", "", "Cycle the focused area's secondary extent through presets",
         KeybindAction::WindowCycleSecondaryExtent},
        {"window-cycle-secondary-extent-back", "", "Cycle the secondary extent presets in reverse",
         KeybindAction::WindowCycleSecondaryExtentBack},
        {"window-focus", "<window-id>", "Focus a window, revealing it from a hidden scratchpad",
         KeybindAction::WindowFocusId, ActionArgKind::WindowId},
        {"window-focus-down", "", "Focus the next window down in the column", KeybindAction::WindowFocusDown},
        {"window-focus-last", "", "Focus the previously focused window", KeybindAction::WindowFocusLast},
        {"window-focus-left", "", "Focus the window to the left", KeybindAction::WindowFocusLeft},
        {"window-focus-next", "", "Focus the next window in layout order", KeybindAction::WindowFocusNext},
        {"window-focus-or-output-down", "", "Focus down, or the output below at the edge",
         KeybindAction::WindowFocusOrOutputDown},
        {"window-focus-or-output-left", "", "Focus left, or the output left at the edge",
         KeybindAction::WindowFocusOrOutputLeft},
        {"window-focus-or-output-right", "", "Focus right, or the output right at the edge",
         KeybindAction::WindowFocusOrOutputRight},
        {"window-focus-or-output-up", "", "Focus up, or the output above at the edge",
         KeybindAction::WindowFocusOrOutputUp},
        {"window-focus-or-workspace-down", "", "Focus down, or the next workspace at the edge",
         KeybindAction::WindowFocusOrWorkspaceDown},
        {"window-focus-or-workspace-up", "", "Focus up, or the previous workspace at the edge",
         KeybindAction::WindowFocusOrWorkspaceUp},
        {"window-focus-previous", "", "Focus the previous window in layout order", KeybindAction::WindowFocusPrevious},
        {"window-focus-right", "", "Focus the window to the right", KeybindAction::WindowFocusRight},
        {"window-focus-switch-floating", "", "Focus the last window of the opposite floating state",
         KeybindAction::WindowFocusSwitchFloating},
        {"window-focus-up", "", "Focus the next window up in the column", KeybindAction::WindowFocusUp},
        {"window-focus-warp", "<window-id>", "Focus or reveal a window and warp the cursor to it",
         KeybindAction::WindowFocusWarpId, ActionArgKind::WindowId},
        {"window-kill", "[<window-id>]", "Kill the process owning the focused or given window",
         KeybindAction::WindowKill, ActionArgKind::OptionalWindowId},
        {"window-minimize", "[<window-id>]", "Minimize the focused floating window, or the given window",
         KeybindAction::WindowMinimize, ActionArgKind::OptionalWindowId},
        {"window-modify-height-down", "<delta>", "Resize the focused window from its bottom edge",
         KeybindAction::WindowModifyHeightDown, ActionArgKind::FractionDelta},
        {"window-modify-height-up", "<delta>", "Resize the focused window from its top edge",
         KeybindAction::WindowModifyHeightUp, ActionArgKind::FractionDelta},
        {"window-modify-primary-extent", "<delta>", "Change the focused area's primary extent by a fraction",
         KeybindAction::WindowModifyPrimaryExtent, ActionArgKind::FractionDelta},
        {"window-modify-secondary-extent", "<delta>", "Change the focused area's secondary extent by a fraction",
         KeybindAction::WindowModifySecondaryExtent, ActionArgKind::FractionDelta},
        {"window-modify-width-left", "<delta>", "Resize the focused column from its left edge",
         KeybindAction::WindowModifyWidthLeft, ActionArgKind::FractionDelta},
        {"window-modify-width-right", "<delta>", "Resize the focused column from its right edge",
         KeybindAction::WindowModifyWidthRight, ActionArgKind::FractionDelta},
        {"window-move-by", "<delta-x> <delta-y> [--window <window-id>]", "Move the floating window by logical pixels",
         KeybindAction::WindowMoveBy, ActionArgKind::WindowMoveBy},
        {"window-move-down", "", "Move down in its column, or a float to the bottom edge",
         KeybindAction::WindowMoveDown},
        {"window-move-left", "", "Move the focused window left, or a float to the left edge",
         KeybindAction::WindowMoveLeft},
        {"window-move-or-output-down", "", "Move down, or the column to the output below",
         KeybindAction::WindowMoveOrOutputDown},
        {"window-move-or-output-left", "", "Move the column left, or to the output left",
         KeybindAction::WindowMoveOrOutputLeft},
        {"window-move-or-output-right", "", "Move the column right, or to the output right",
         KeybindAction::WindowMoveOrOutputRight},
        {"window-move-or-output-up", "", "Move up, or the column to the output above",
         KeybindAction::WindowMoveOrOutputUp},
        {"window-move-or-workspace-down", "", "Move down, or to the next workspace at the edge",
         KeybindAction::WindowMoveOrWorkspaceDown},
        {"window-move-or-workspace-up", "", "Move up, or to the previous workspace at the edge",
         KeybindAction::WindowMoveOrWorkspaceUp},
        {"window-move-right", "", "Move the focused window right, or a float to the right edge",
         KeybindAction::WindowMoveRight},
        {"window-move-to", "<anchor> <x> <y> [--window <window-id>]",
         "Move the floating window to an anchored position", KeybindAction::WindowMoveTo, ActionArgKind::WindowMoveTo},
        {"window-move-to-output-down", "", "Move the focused window to the output below",
         KeybindAction::WindowMoveToOutputDown},
        {"window-move-to-output-left", "", "Move the focused window to the output left",
         KeybindAction::WindowMoveToOutputLeft},
        {"window-move-to-output-next", "", "Move the focused window to the next output",
         KeybindAction::WindowMoveToOutputNext},
        {"window-move-to-output-previous", "", "Move the focused window to the previous output",
         KeybindAction::WindowMoveToOutputPrevious},
        {"window-move-to-output-right", "", "Move the focused window to the output right",
         KeybindAction::WindowMoveToOutputRight},
        {"window-move-to-output-up", "", "Move the focused window to the output above",
         KeybindAction::WindowMoveToOutputUp},
        {"window-move-to-scratchpad", "[<scratchpad>]", "Move the focused window into a scratchpad",
         KeybindAction::WindowMoveToScratchpad, ActionArgKind::OptionalScratchpad},
        {"window-move-to-workspace", "<workspace> [--output <output>]",
         "Move the focused window to the selected workspace", KeybindAction::WindowMoveToWorkspace,
         ActionArgKind::Workspace},
        {"window-move-to-workspace-next", "", "Move the focused window to the next workspace",
         KeybindAction::WindowMoveToWorkspaceNext},
        {"window-move-to-workspace-previous", "", "Move the focused window to the previous workspace",
         KeybindAction::WindowMoveToWorkspacePrevious},
        {"window-move-to-workspace-silent", "<workspace> [--output <output>]",
         "Move the focused window to the selected workspace silently", KeybindAction::WindowMoveToWorkspaceSilent,
         ActionArgKind::Workspace},
        {"window-move-to-workspace-silent-next", "", "Move the focused window to the next workspace silently",
         KeybindAction::WindowMoveToWorkspaceSilentNext},
        {"window-move-to-workspace-silent-previous", "", "Move the focused window to the previous workspace silently",
         KeybindAction::WindowMoveToWorkspaceSilentPrevious},
        {"window-move-up", "", "Move up in its column, or a float to the top edge", KeybindAction::WindowMoveUp},
        {"window-restore-from-scratchpad", "[<scratchpad>]", "Return a scratchpad window to its saved workspace",
         KeybindAction::WindowRestoreFromScratchpad, ActionArgKind::OptionalScratchpad},
        {"window-restore-minimized", "", "Restore the latest minimized window on this workspace",
         KeybindAction::WindowRestoreMinimized},
        {"window-set-primary-extent", "<fraction>", "Set the focused area's primary extent fraction",
         KeybindAction::WindowSetPrimaryExtent, ActionArgKind::Fraction},
        {"window-set-region", "<left|right|top|bottom|top_left|top_right|bottom_left|bottom_right>",
         "Place the stacking window in a half or quarter", KeybindAction::WindowSetRegion, ActionArgKind::WindowRegion},
        {"window-set-secondary-extent", "<fraction>", "Set the focused area's secondary extent fraction",
         KeybindAction::WindowSetSecondaryExtent, ActionArgKind::Fraction},
        {"window-swap-down", "", "Swap with the window below", KeybindAction::WindowSwapDown},
        {"window-swap-left", "", "Swap with the window to the left", KeybindAction::WindowSwapLeft},
        {"window-swap-next", "", "Swap with the next window in layout order", KeybindAction::WindowSwapNext},
        {"window-swap-previous", "", "Swap with the previous window in layout order",
         KeybindAction::WindowSwapPrevious},
        {"window-swap-right", "", "Swap with the window to the right", KeybindAction::WindowSwapRight},
        {"window-swap-up", "", "Swap with the window above", KeybindAction::WindowSwapUp},
        {"window-toggle-floating", "[<window-id>]", "Float or tile the focused window, or the given window",
         KeybindAction::ToggleFloating, ActionArgKind::OptionalWindowId},
        {"window-toggle-fullscreen", "", "Toggle fullscreen or exit a window covering the focus",
         KeybindAction::ToggleFullscreen},
        {"window-toggle-maximize", "", "Toggle full width for the focused column", KeybindAction::ToggleMaximize},
        {"window-toggle-maximize-to-edges", "", "Toggle maximize without gaps, struts, or borders",
         KeybindAction::ToggleMaximizeToEdges},
        {"window-toggle-pinned", "", "Pin the focused window above other windows", KeybindAction::TogglePinned},
        {"window-toggle-scratchpad", "[<scratchpad>]", "Move the focused window to or from a scratchpad",
         KeybindAction::WindowToggleScratchpad, ActionArgKind::OptionalScratchpad},
        {"workspace-focus-last", "", "Focus the previously active workspace", KeybindAction::WorkspaceFocusLast},
        {"workspace-move-down", "", "Move the focused workspace down the list", KeybindAction::WorkspaceMoveDown},
        {"workspace-move-to-output", "<destination> [<workspace>] [--output <output>]",
         "Move the active or selected workspace to an output", KeybindAction::WorkspaceMoveToOutput,
         ActionArgKind::WorkspaceMoveToOutput},
        {"workspace-move-to-output-down", "", "Move the workspace to the output below",
         KeybindAction::WorkspaceMoveToOutputDown},
        {"workspace-move-to-output-left", "", "Move the workspace to the output left",
         KeybindAction::WorkspaceMoveToOutputLeft},
        {"workspace-move-to-output-next", "", "Move the workspace to the next output",
         KeybindAction::WorkspaceMoveToOutputNext},
        {"workspace-move-to-output-previous", "", "Move the workspace to the previous output",
         KeybindAction::WorkspaceMoveToOutputPrevious},
        {"workspace-move-to-output-right", "", "Move the workspace to the output right",
         KeybindAction::WorkspaceMoveToOutputRight},
        {"workspace-move-to-output-up", "", "Move the workspace to the output above",
         KeybindAction::WorkspaceMoveToOutputUp},
        {"workspace-move-up", "", "Move the focused workspace up the list", KeybindAction::WorkspaceMoveUp},
        {"workspace-next", "", "Switch to the next workspace on this output", KeybindAction::WorkspaceNext},
        {"workspace-previous", "", "Switch to the previous workspace on this output", KeybindAction::WorkspacePrevious},
        {"workspace-set-layout", "<scrolling|dwindle|master|stacking|toggle [mode1 mode2 ...]>",
         "Set the active workspace's layout mode", KeybindAction::WorkspaceSetLayout, ActionArgKind::LayoutMode},
        {"workspace-swap-active-output-down", "", "Swap active workspace windows with the output below",
         KeybindAction::WorkspaceSwapActiveOutputDown},
        {"workspace-swap-active-output-left", "", "Swap active workspace windows with the output left",
         KeybindAction::WorkspaceSwapActiveOutputLeft},
        {"workspace-swap-active-output-next", "", "Swap active workspace windows with the next output",
         KeybindAction::WorkspaceSwapActiveOutputNext},
        {"workspace-swap-active-output-previous", "", "Swap active workspace windows with the previous output",
         KeybindAction::WorkspaceSwapActiveOutputPrevious},
        {"workspace-swap-active-output-right", "", "Swap active workspace windows with the output right",
         KeybindAction::WorkspaceSwapActiveOutputRight},
        {"workspace-swap-active-output-up", "", "Swap active workspace windows with the output above",
         KeybindAction::WorkspaceSwapActiveOutputUp},
        {"workspace-switch", "<workspace> [--output <output>]", "Switch to the selected workspace",
         KeybindAction::WorkspaceSwitch, ActionArgKind::Workspace},
        {"workspace-toggle-show-desktop", "", "Minimize or restore the workspace's floating windows",
         KeybindAction::WorkspaceToggleShowDesktop},
    };

  } // namespace

  std::span<const ActionSpec> actionSpecs() { return kActionSpecs; }

  const ActionSpec* findActionSpec(std::string_view name) {
    const auto* spec = std::ranges::lower_bound(kActionSpecs, name, {}, &ActionSpec::name);
    return spec != std::ranges::end(kActionSpecs) && spec->name == name ? spec : nullptr;
  }

  std::optional<LayoutMode> layoutModeNamed(std::string_view name) {
    for (const LayoutMode mode :
         {LayoutMode::Scrolling, LayoutMode::Dwindle, LayoutMode::Master, LayoutMode::Stacking}) {
      if (name == layoutModeName(mode)) {
        return mode;
      }
    }
    return std::nullopt;
  }

  std::string_view layoutModeName(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Scrolling:
      return "scrolling";
    case LayoutMode::Dwindle:
      return "dwindle";
    case LayoutMode::Master:
      return "master";
    case LayoutMode::Stacking:
      return "stacking";
    }
    return "scrolling";
  }

  bool validActionFraction(double fraction) {
    return std::isfinite(fraction) && fraction >= 0.1 && fraction <= kMaxWideFraction;
  }

  bool validActionFractionDelta(double delta) {
    return std::isfinite(delta) && delta != 0.0 && std::fabs(delta) <= 0.9;
  }

  bool validSdrWhiteNits(double nits) { return std::isfinite(nits) && nits >= 80.0 && nits <= 1000.0; }

  uint32_t mouseButtonFromName(std::string_view name) {
    const std::string lowered = toLower(name);
    if (lowered == "mouseleft") {
      return BTN_LEFT;
    }
    if (lowered == "mouseright") {
      return BTN_RIGHT;
    }
    if (lowered == "mousemiddle") {
      return BTN_MIDDLE;
    }
    if (lowered == "mouseback") {
      return BTN_SIDE;
    }
    if (lowered == "mouseforward") {
      return BTN_EXTRA;
    }
    return 0;
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

  uint32_t canonicalMouseButton(uint32_t button) {
    switch (button) {
    case BTN_BACK:
      return BTN_SIDE;
    case BTN_FORWARD:
      return BTN_EXTRA;
    default:
      return button;
    }
  }

  bool parseChord(std::string_view chord, Keybind& output) {
    output = Keybind{};

    if (!takeSubmapPrefix(chord, output)) {
      return false;
    }

    std::vector<std::string_view> tokens;
    if (!splitChordTokens(chord, tokens)) {
      return false;
    }
    if (tokens.size() == 1 && applyModifier(tokens.front(), output)) {
      output.modifierOnly = true;
      output.repeat = false;
      return true;
    }

    const std::string lastLower = toLower(tokens.back());
    const WheelDirection wheelDir = wheelFromName(lastLower);
    const uint32_t mouseButton = mouseButtonFromName(lastLower);

    if (wheelDir != WheelDirection::None || mouseButton != 0) {
      // A bare wheel or mouse-button bind would hijack all client input.
      if (tokens.size() < 2) {
        return false;
      }
      if (!applyModifiers(tokens, output)) {
        return false;
      }
      output.wheel = wheelDir;
      output.mouseButton = mouseButton;
      return true;
    }

    const std::string keyName(tokens.back());
    const xkb_keysym_t keysym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (keysym == XKB_KEY_NoSymbol) {
      return false;
    }

    if (!applyModifiers(tokens, output)) {
      return false;
    }

    output.keysym = xkb_keysym_to_lower(keysym);
    return true;
  }

  std::optional<std::vector<std::string>> splitActionString(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    for (size_t index = 0; index < text.size(); ++index) {
      const char character = text[index];
      if (isActionSpace(character)) {
        if (inWord) {
          words.push_back(std::move(word));
          word.clear();
          inWord = false;
        }
        continue;
      }
      inWord = true;
      if (character == '\'') {
        const size_t close = text.find('\'', index + 1);
        if (close == std::string_view::npos) {
          return std::nullopt;
        }
        word.append(text.substr(index + 1, close - index - 1));
        index = close;
      } else if (character == '"') {
        bool closed = false;
        for (++index; index < text.size(); ++index) {
          if (text[index] == '"') {
            closed = true;
            break;
          }
          if (text[index] == '\\' && index + 1 < text.size() && (text[index + 1] == '"' || text[index + 1] == '\\')) {
            ++index;
          }
          word.push_back(text[index]);
        }
        if (!closed) {
          return std::nullopt;
        }
      } else if (character == '\\') {
        if (index + 1 >= text.size()) {
          return std::nullopt;
        }
        word.push_back(text[++index]);
      } else {
        word.push_back(character);
      }
    }
    if (inWord) {
      words.push_back(std::move(word));
    }
    return words;
  }

  namespace {

    std::optional<KeybindPayload> parseActionPayload(const ActionSpec& spec, std::span<const std::string> words) {
      switch (spec.argKind) {
      case ActionArgKind::None:
        if (!words.empty()) {
          return std::nullopt;
        }
        return std::monostate{};
      case ActionArgKind::Command: {
        std::string command;
        for (const auto& word : words) {
          if (!command.empty()) {
            command += ' ';
          }
          command += word;
        }
        if (command.empty()) {
          return std::nullopt;
        }
        return SpawnArg{.command = std::move(command)};
      }
      case ActionArgKind::Submap: {
        const auto name = singleWord(words);
        if (!name || !validSubmapName(*name)) {
          return std::nullopt;
        }
        return SubmapArg{.name = std::string(*name)};
      }
      case ActionArgKind::Fraction: {
        const auto word = singleWord(words);
        const auto fraction = word ? parseFraction(*word) : std::nullopt;
        if (!fraction) {
          return std::nullopt;
        }
        return FractionArg{.fraction = *fraction};
      }
      case ActionArgKind::FractionDelta: {
        const auto word = singleWord(words);
        const auto delta = word ? parseFractionDelta(*word) : std::nullopt;
        if (!delta) {
          return std::nullopt;
        }
        return FractionArg{.fraction = *delta};
      }
      case ActionArgKind::Workspace: {
        const auto split = splitActionWords(words, kFlagOutput | kFlagName);
        auto workspace = split ? parseWorkspaceSelector(*split) : std::nullopt;
        if (!workspace) {
          return std::nullopt;
        }
        return std::move(*workspace);
      }
      case ActionArgKind::WorkspaceMoveToOutput: {
        auto split = splitActionWords(words, kFlagOutput | kFlagName);
        if (!split || split->positional.empty()) {
          return std::nullopt;
        }
        WorkspaceMoveToOutputArg arg{.destination = std::string(split->positional.front()), .workspace = std::nullopt};
        split->positional.erase(split->positional.begin());
        if (split->positional.empty() && !split->name && !split->output) {
          return arg;
        }
        arg.workspace = parseWorkspaceSelector(*split);
        return arg.workspace ? std::make_optional<KeybindPayload>(std::move(arg)) : std::nullopt;
      }
      case ActionArgKind::WindowEffectSet:
      case ActionArgKind::WindowEffectCycle:
      case ActionArgKind::ScreenEffectSet:
      case ActionArgKind::ScreenEffectCycle:
      case ActionArgKind::CursorEffectSet:
      case ActionArgKind::CursorEffectCycle:
        return parseEffectWords(spec.argKind, words);
      case ActionArgKind::Output: {
        const auto output = singleWord(words);
        if (!output) {
          return std::nullopt;
        }
        return OutputArg{.output = std::string(*output)};
      }
      case ActionArgKind::OptionalOutput: {
        const auto output = optionalWord(words);
        if (!output) {
          return std::nullopt;
        }
        return OutputArg{.output = std::string(*output)};
      }
      case ActionArgKind::OptionalScratchpad: {
        const auto name = optionalWord(words);
        if (!name) {
          return std::nullopt;
        }
        return ScratchpadArg{.name = std::string(*name)};
      }
      case ActionArgKind::WindowId: {
        const auto id = singleWord(words);
        if (!id) {
          return std::nullopt;
        }
        return WindowIdArg{.id = std::string(*id)};
      }
      case ActionArgKind::OptionalWindowId: {
        const auto id = optionalWord(words);
        if (!id) {
          return std::nullopt;
        }
        return WindowIdArg{.id = std::string(*id)};
      }
      case ActionArgKind::WindowRegion: {
        const auto word = singleWord(words);
        const auto region = word ? windowRegionNamed(*word) : std::nullopt;
        if (!region) {
          return std::nullopt;
        }
        return WindowRegionArg{.region = *region};
      }
      case ActionArgKind::ColumnDisplay: {
        const auto word = singleWord(words);
        if (!word || (*word != "normal" && *word != "tabbed")) {
          return std::nullopt;
        }
        return ColumnDisplayArg{.display = *word == "tabbed" ? ColumnDisplay::Tabbed : ColumnDisplay::Normal};
      }
      case ActionArgKind::TabIndex: {
        const auto word = singleWord(words);
        if (!word) {
          return std::nullopt;
        }
        int index = 0;
        const auto [end, error] = std::from_chars(word->data(), word->data() + word->size(), index);
        if (error != std::errc{} || end != word->data() + word->size() || index == 0) {
          return std::nullopt;
        }
        return TabIndexArg{.index = index};
      }
      case ActionArgKind::SkipConfirmation: {
        const auto word = optionalWord(words);
        if (!word || (!word->empty() && *word != "skip-confirmation")) {
          return std::nullopt;
        }
        return QuitArg{.skipConfirmation = !word->empty()};
      }
      case ActionArgKind::SdrWhite:
        return parseSdrWhiteWords(words);
      case ActionArgKind::LayoutMode: {
        auto mode = parseLayoutModeWords(words);
        if (!mode) {
          return std::nullopt;
        }
        return std::move(*mode);
      }
      case ActionArgKind::WindowMoveTo:
        return parseWindowMoveToWords(words);
      case ActionArgKind::WindowMoveBy:
        return parseWindowMoveByWords(words);
      }
      return std::nullopt;
    }

  } // namespace

  bool parseActionWords(std::span<const std::string> words, Keybind& output) {
    if (words.empty()) {
      return false;
    }
    const ActionSpec* spec = findActionSpec(words.front());
    if (spec == nullptr) {
      return false;
    }
    auto payload = parseActionPayload(*spec, words.subspan(1));
    if (!payload) {
      return false;
    }
    output.action = spec->action;
    output.payload = std::move(*payload);
    return true;
  }

  bool parseAction(std::string_view value, Keybind& output) {
    const size_t start = value.find_first_not_of(kActionSpace);
    if (start == std::string_view::npos) {
      return false;
    }
    const size_t end = value.find_first_of(kActionSpace, start);
    const std::string_view name = value.substr(start, end == std::string_view::npos ? end : end - start);
    // A command keeps its raw text after the separating whitespace, so its own quoting and escapes, including a
    // trailing escaped space, reach the shell unchanged.
    if (const ActionSpec* spec = findActionSpec(name); spec != nullptr && spec->argKind == ActionArgKind::Command) {
      std::vector<std::string> words{std::string(name)};
      if (end != std::string_view::npos) {
        std::string_view rest = value.substr(end);
        rest.remove_prefix(std::min(rest.find_first_not_of(kActionSpace), rest.size()));
        if (!rest.empty()) {
          words.emplace_back(rest);
        }
      }
      return parseActionWords(words, output);
    }
    const auto words = splitActionString(value);
    return words.has_value() && parseActionWords(*words, output);
  }

  std::optional<EffectActionReference> effectActionReference(const Keybind& bind) {
    std::string_view name;
    EffectKind kind = EffectKind::Window;
    bool poolRequired = false;
    switch (bind.action) {
    case KeybindAction::EffectWindowSet:
    case KeybindAction::EffectWindowCycle:
      if (const auto* arg = payloadIf<EffectWindowArg>(bind)) {
        name = arg->name;
      }
      kind = EffectKind::Window;
      poolRequired = bind.action == KeybindAction::EffectWindowCycle;
      break;
    case KeybindAction::EffectBorderSet:
    case KeybindAction::EffectBorderCycle:
      if (const auto* arg = payloadIf<EffectWindowArg>(bind)) {
        name = arg->name;
      }
      kind = EffectKind::Border;
      poolRequired = bind.action == KeybindAction::EffectBorderCycle;
      break;
    case KeybindAction::EffectScreenSet:
    case KeybindAction::EffectScreenCycle:
      if (const auto* arg = payloadIf<EffectScreenArg>(bind)) {
        name = arg->name;
      }
      kind = EffectKind::Screen;
      poolRequired = bind.action == KeybindAction::EffectScreenCycle;
      break;
    case KeybindAction::EffectCursorSet:
    case KeybindAction::EffectCursorCycle:
      if (const auto* arg = payloadIf<EffectCursorArg>(bind)) {
        name = arg->name;
      }
      kind = EffectKind::Cursor;
      poolRequired = bind.action == KeybindAction::EffectCursorCycle;
      break;
    default:
      return std::nullopt;
    }
    if (name.empty() || name == kEffectOff) {
      return std::nullopt;
    }
    return EffectActionReference{.name = name, .kind = kind, .poolRequired = poolRequired};
  }

  std::vector<Keybind> defaultKeybinds() {
    std::vector<Keybind> keybinds;
    keybinds.reserve(60);
    auto add = [&keybinds](KeybindAction action, uint32_t keysym, uint32_t modifiers = 0) -> Keybind& {
      Keybind bind;
      bind.modifiers = modifiers;
      bind.useMod = true;
      bind.keysym = xkb_keysym_to_lower(keysym);
      bind.action = action;
      return keybinds.emplace_back(std::move(bind));
    };

    add(KeybindAction::SessionQuit, XKB_KEY_Escape);
    // Holding close would also close each window that focus moves to.
    add(KeybindAction::WindowClose, XKB_KEY_q).repeat = false;
    add(KeybindAction::WindowFocusNext, XKB_KEY_F1);

    add(KeybindAction::WindowFocusLeft, XKB_KEY_Left);
    add(KeybindAction::WindowFocusLeft, XKB_KEY_h);
    add(KeybindAction::WindowFocusRight, XKB_KEY_Right);
    add(KeybindAction::WindowFocusRight, XKB_KEY_l);
    add(KeybindAction::WindowFocusUp, XKB_KEY_Up);
    add(KeybindAction::WindowFocusUp, XKB_KEY_k);
    add(KeybindAction::WindowFocusDown, XKB_KEY_Down);
    add(KeybindAction::WindowFocusDown, XKB_KEY_j);

    add(KeybindAction::ColumnMoveLeft, XKB_KEY_Left, WLR_MODIFIER_SHIFT);
    add(KeybindAction::ColumnMoveLeft, XKB_KEY_h, WLR_MODIFIER_SHIFT);
    add(KeybindAction::ColumnMoveRight, XKB_KEY_Right, WLR_MODIFIER_SHIFT);
    add(KeybindAction::ColumnMoveRight, XKB_KEY_l, WLR_MODIFIER_SHIFT);
    add(KeybindAction::WindowMoveUp, XKB_KEY_Up, WLR_MODIFIER_SHIFT);
    add(KeybindAction::WindowMoveUp, XKB_KEY_k, WLR_MODIFIER_SHIFT);
    add(KeybindAction::WindowMoveDown, XKB_KEY_Down, WLR_MODIFIER_SHIFT);
    add(KeybindAction::WindowMoveDown, XKB_KEY_j, WLR_MODIFIER_SHIFT);

    add(KeybindAction::WindowConsumeLeft, XKB_KEY_comma);
    add(KeybindAction::WindowConsumeRight, XKB_KEY_period);
    add(KeybindAction::WindowConsumeFromLeft, XKB_KEY_comma, WLR_MODIFIER_CTRL);
    add(KeybindAction::WindowConsumeFromRight, XKB_KEY_period, WLR_MODIFIER_CTRL);
    add(KeybindAction::WindowCyclePrimaryExtent, XKB_KEY_r);
    add(KeybindAction::WindowCyclePrimaryExtentBack, XKB_KEY_r, WLR_MODIFIER_SHIFT);
    add(KeybindAction::ToggleFullscreen, XKB_KEY_f);
    add(KeybindAction::ToggleMaximize, XKB_KEY_f, WLR_MODIFIER_CTRL);
    add(KeybindAction::ToggleMaximizeToEdges, XKB_KEY_m);
    add(KeybindAction::ToggleFloating, XKB_KEY_t);
    add(KeybindAction::TogglePinned, XKB_KEY_p);
    add(KeybindAction::ColumnToggleTabbed, XKB_KEY_w);
    // Holding the overview key would thrash open/close.
    add(KeybindAction::OverviewToggle, XKB_KEY_o).repeat = false;

    for (int index = 0; index < 9; ++index) {
      const uint32_t digit = XKB_KEY_1 + static_cast<uint32_t>(index);
      const uint32_t keypad = XKB_KEY_KP_1 + static_cast<uint32_t>(index);
      auto addWorkspace = [&](KeybindAction action, uint32_t keysym, uint32_t modifiers) {
        Keybind bind;
        bind.modifiers = modifiers;
        bind.useMod = true;
        bind.keysym = keysym;
        bind.action = action;
        WorkspaceArg workspace;
        workspace.reference = WorkspaceIndex{static_cast<size_t>(index + 1)};
        bind.payload = std::move(workspace);
        keybinds.push_back(std::move(bind));
      };
      addWorkspace(KeybindAction::WorkspaceSwitch, digit, 0);
      addWorkspace(KeybindAction::WorkspaceSwitch, keypad, 0);
      addWorkspace(KeybindAction::WindowMoveToWorkspace, digit, WLR_MODIFIER_SHIFT);
      addWorkspace(KeybindAction::WindowMoveToWorkspace, keypad, WLR_MODIFIER_SHIFT);
    }

    // Default wheel binds: Mod+WheelUp = window-focus-left, Mod+WheelDown = window-focus-right.
    {
      Keybind wheel;
      wheel.useMod = true;
      wheel.wheel = WheelDirection::Up;
      wheel.action = KeybindAction::WindowFocusLeft;
      keybinds.push_back(std::move(wheel));
    }
    {
      Keybind wheel;
      wheel.useMod = true;
      wheel.wheel = WheelDirection::Down;
      wheel.action = KeybindAction::WindowFocusRight;
      keybinds.push_back(std::move(wheel));
    }

    return keybinds;
  }

} // namespace umbriel
