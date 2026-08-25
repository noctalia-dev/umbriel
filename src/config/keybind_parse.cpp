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

    uint32_t mouseButtonFromName(std::string_view lowered) {
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

    // Shared shape of a parameterized action: "name:arg" with a non-empty arg.
    bool takeActionArg(std::string_view value, const ActionSpec& spec, std::string_view& arg) {
      if (value.size() <= spec.name.size() + 1 || value[spec.name.size()] != ':' || !value.starts_with(spec.name)) {
        return false;
      }
      arg = value.substr(spec.name.size() + 1);
      return true;
    }

    constexpr ActionSpec kActionSpecs[] = {
        {"cheatsheet-close", "", KeybindAction::CheatsheetClose},
        {"cheatsheet-open", "", KeybindAction::CheatsheetOpen},
        {"cheatsheet-toggle", "", KeybindAction::CheatsheetToggle},
        {"column-move-left", "", KeybindAction::ColumnMoveLeft},
        {"column-move-right", "", KeybindAction::ColumnMoveRight},
        {"column-move-to-output-down", "", KeybindAction::ColumnMoveToOutputDown},
        {"column-move-to-output-left", "", KeybindAction::ColumnMoveToOutputLeft},
        {"column-move-to-output-right", "", KeybindAction::ColumnMoveToOutputRight},
        {"column-move-to-output-up", "", KeybindAction::ColumnMoveToOutputUp},
        {"config-reload", "", KeybindAction::ConfigReload},
        {"dpms-off", "[<output>]", KeybindAction::DpmsOff, ActionArgKind::OptionalOutput},
        {"dpms-on", "[<output>]", KeybindAction::DpmsOn, ActionArgKind::OptionalOutput},
        {"keyboard-layout-next", "", KeybindAction::KeyboardLayoutNext},
        {"layout-scroll-down", "", KeybindAction::LayoutScrollDown},
        {"layout-scroll-left", "", KeybindAction::LayoutScrollLeft},
        {"layout-scroll-right", "", KeybindAction::LayoutScrollRight},
        {"layout-scroll-up", "", KeybindAction::LayoutScrollUp},
        {"output-focus-down", "", KeybindAction::OutputFocusDown},
        {"output-focus-left", "", KeybindAction::OutputFocusLeft},
        {"output-focus-right", "", KeybindAction::OutputFocusRight},
        {"output-focus-up", "", KeybindAction::OutputFocusUp},
        {"overview-close", "", KeybindAction::OverviewClose},
        {"overview-open", "", KeybindAction::OverviewOpen},
        {"overview-toggle", "", KeybindAction::OverviewToggle},
        {"scratchpad-focus-next", "[<output>]", KeybindAction::ScratchpadFocusNext, ActionArgKind::OptionalOutput},
        {"scratchpad-toggle", "[<output>]", KeybindAction::ScratchpadToggle, ActionArgKind::OptionalOutput},
        {"session-quit", "[skip-confirmation]", KeybindAction::SessionQuit, ActionArgKind::SkipConfirmation},
        {"spawn", "<cmd>", KeybindAction::Spawn, ActionArgKind::Command},
        {"submap", "<name>", KeybindAction::Submap, ActionArgKind::Command},
        {"window-center", "", KeybindAction::WindowCenter},
        {"window-close", "[<window-id>]", KeybindAction::WindowClose, ActionArgKind::OptionalWindowId},
        {"window-consume-left", "", KeybindAction::WindowConsumeLeft},
        {"window-cycle-width", "", KeybindAction::WindowCycleWidth},
        {"window-cycle-width-back", "", KeybindAction::WindowCycleWidthBack},
        {"window-expel-right", "", KeybindAction::WindowExpelRight},
        {"window-focus", "<window-id>", KeybindAction::WindowFocusId, ActionArgKind::WindowId},
        {"window-focus-down", "", KeybindAction::WindowFocusDown},
        {"window-focus-left", "", KeybindAction::WindowFocusLeft},
        {"window-focus-next", "", KeybindAction::WindowFocusNext},
        {"window-focus-right", "", KeybindAction::WindowFocusRight},
        {"window-focus-up", "", KeybindAction::WindowFocusUp},
        {"window-modify-width", "<delta>", KeybindAction::WindowModifyWidth, ActionArgKind::WidthDelta},
        {"window-move-down", "", KeybindAction::WindowMoveDown},
        {"window-move-to-output-down", "", KeybindAction::WindowMoveToOutputDown},
        {"window-move-to-output-left", "", KeybindAction::WindowMoveToOutputLeft},
        {"window-move-to-output-right", "", KeybindAction::WindowMoveToOutputRight},
        {"window-move-to-output-up", "", KeybindAction::WindowMoveToOutputUp},
        {"window-move-to-scratchpad", "[<output>]", KeybindAction::WindowMoveToScratchpad,
         ActionArgKind::OptionalOutput},
        {"window-move-to-workspace", "<workspace>[/<output>]", KeybindAction::WindowMoveToWorkspace,
         ActionArgKind::Workspace},
        {"window-move-to-workspace-next", "", KeybindAction::WindowMoveToWorkspaceNext},
        {"window-move-to-workspace-previous", "", KeybindAction::WindowMoveToWorkspacePrevious},
        {"window-move-up", "", KeybindAction::WindowMoveUp},
        {"window-restore-from-scratchpad", "[<output>]", KeybindAction::WindowRestoreFromScratchpad,
         ActionArgKind::OptionalOutput},
        {"window-set-width", "<fraction>", KeybindAction::WindowSetWidth, ActionArgKind::WidthFraction},
        {"window-toggle-floating", "", KeybindAction::ToggleFloating},
        {"window-toggle-fullscreen", "", KeybindAction::ToggleFullscreen},
        {"window-toggle-maximize", "", KeybindAction::ToggleMaximize},
        {"window-toggle-maximize-to-edges", "", KeybindAction::ToggleMaximizeToEdges},
        {"window-toggle-pinned", "", KeybindAction::TogglePinned},
        {"workspace-move-down", "", KeybindAction::WorkspaceMoveDown},
        {"workspace-move-to-output-down", "", KeybindAction::WorkspaceMoveToOutputDown},
        {"workspace-move-to-output-left", "", KeybindAction::WorkspaceMoveToOutputLeft},
        {"workspace-move-to-output-right", "", KeybindAction::WorkspaceMoveToOutputRight},
        {"workspace-move-to-output-up", "", KeybindAction::WorkspaceMoveToOutputUp},
        {"workspace-move-up", "", KeybindAction::WorkspaceMoveUp},
        {"workspace-next", "", KeybindAction::WorkspaceNext},
        {"workspace-previous", "", KeybindAction::WorkspacePrevious},
        {"workspace-set-layout", "<scrolling|dwindle|toggle>", KeybindAction::WorkspaceSetLayout,
         ActionArgKind::LayoutMode},
        {"workspace-switch", "<workspace>[/<output>]", KeybindAction::WorkspaceSwitch, ActionArgKind::Workspace},
    };

  } // namespace

  std::span<const ActionSpec> actionSpecs() { return kActionSpecs; }

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

  bool parseAction(std::string_view value, Keybind& output) {
    for (const auto& spec : kActionSpecs) {
      std::string_view arg;
      switch (spec.argKind) {
      case ActionArgKind::None:
        if (value == spec.name) {
          output.action = spec.action;
          output.payload = std::monostate{};
          return true;
        }
        break;
      case ActionArgKind::Command:
        if (takeActionArg(value, spec, arg)) {
          output.action = spec.action;
          // "submap" shares the name:<text> syntax with "spawn", but its
          // argument is a submap name rather than a shell command.
          if (spec.action == KeybindAction::Submap) {
            if (arg == "disable") {
              return false;
            }
            output.payload = SubmapArg{.name = std::string(arg)};
          } else {
            output.payload = SpawnArg{.command = std::string(arg)};
          }
          return true;
        }
        break;
      case ActionArgKind::WidthFraction: {
        if (!takeActionArg(value, spec, arg)) {
          break;
        }
        double fraction = 0.0;
        const auto [fractionPtr, fractionError] = std::from_chars(arg.data(), arg.data() + arg.size(), fraction);
        if (fractionError != std::errc{}
            || fractionPtr != arg.data() + arg.size()
            || !std::isfinite(fraction)
            || fraction < 0.1
            || fraction > 1.0) {
          break;
        }
        output.action = spec.action;
        output.payload = WidthArg{.fraction = fraction};
        return true;
      }
      case ActionArgKind::WidthDelta: {
        if (!takeActionArg(value, spec, arg)) {
          break;
        }
        // std::from_chars rejects a leading '+', but the delta is signed.
        if (!arg.empty() && arg.front() == '+') {
          arg.remove_prefix(1);
        }
        double delta = 0.0;
        const auto [deltaPtr, deltaError] = std::from_chars(arg.data(), arg.data() + arg.size(), delta);
        if (deltaError != std::errc{}
            || deltaPtr != arg.data() + arg.size()
            || !std::isfinite(delta)
            || delta == 0.0
            || std::fabs(delta) > 0.9) {
          break;
        }
        output.action = spec.action;
        output.payload = WidthArg{.fraction = delta};
        return true;
      }
      case ActionArgKind::Workspace: {
        if (!takeActionArg(value, spec, arg)) {
          break;
        }
        WorkspaceArg workspace;
        std::string_view selector = arg;
        const size_t separator = selector.find('/');
        if (separator != std::string_view::npos) {
          if (separator == 0
              || separator + 1 == selector.size()
              || selector.find('/', separator + 1) != std::string_view::npos) {
            break;
          }
          workspace.output = selector.substr(separator + 1);
          selector = selector.substr(0, separator);
        }
        workspace.name = selector;
        output.action = spec.action;
        output.payload = std::move(workspace);
        return true;
      }
      case ActionArgKind::OptionalOutput:
        if (value == spec.name) {
          output.action = spec.action;
          output.payload = OutputArg{};
          return true;
        }
        if (takeActionArg(value, spec, arg)) {
          output.action = spec.action;
          output.payload = OutputArg{.output = std::string(arg)};
          return true;
        }
        break;
      case ActionArgKind::WindowId:
        if (takeActionArg(value, spec, arg)) {
          output.action = spec.action;
          output.payload = WindowIdArg{.id = std::string(arg)};
          return true;
        }
        break;
      case ActionArgKind::OptionalWindowId:
        if (value == spec.name) {
          output.action = spec.action;
          output.payload = WindowIdArg{};
          return true;
        }
        if (takeActionArg(value, spec, arg)) {
          output.action = spec.action;
          output.payload = WindowIdArg{.id = std::string(arg)};
          return true;
        }
        break;
      case ActionArgKind::SkipConfirmation:
        if (value == spec.name) {
          output.action = spec.action;
          output.payload = QuitArg{};
          return true;
        }
        if (takeActionArg(value, spec, arg) && arg == "skip-confirmation") {
          output.action = spec.action;
          output.payload = QuitArg{.skipConfirmation = true};
          return true;
        }
        break;
      case ActionArgKind::LayoutMode:
        if (takeActionArg(value, spec, arg)) {
          if (arg == "scrolling") {
            output.action = spec.action;
            output.payload = LayoutModeArg{.mode = LayoutMode::Scrolling};
            return true;
          }
          if (arg == "dwindle") {
            output.action = spec.action;
            output.payload = LayoutModeArg{.mode = LayoutMode::Dwindle};
            return true;
          }
          if (arg == "toggle") {
            output.action = spec.action;
            output.payload = LayoutModeArg{};
            return true;
          }
        }
        break;
      }
    }
    return false;
  }

  std::vector<Keybind> defaultKeybinds() {
    std::vector<Keybind> keybinds;
    keybinds.reserve(60);
    // Built by assignment rather than aggregate initialisation: the trigger and payload fields already carry default
    // member initialisers, and naming every one of them just to satisfy -Wmissing-field-initializers is noise.
    auto add = [&keybinds](KeybindAction action, uint32_t keysym, uint32_t modifiers = 0) {
      Keybind bind;
      bind.modifiers = modifiers;
      bind.useMod = true;
      bind.keysym = xkb_keysym_to_lower(keysym);
      bind.action = action;
      keybinds.push_back(std::move(bind));
    };

    add(KeybindAction::SessionQuit, XKB_KEY_Escape);
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
    add(KeybindAction::WindowExpelRight, XKB_KEY_period);
    add(KeybindAction::WindowCycleWidth, XKB_KEY_r);
    add(KeybindAction::WindowCycleWidthBack, XKB_KEY_r, WLR_MODIFIER_SHIFT);
    add(KeybindAction::ToggleFullscreen, XKB_KEY_f);
    add(KeybindAction::ToggleMaximize, XKB_KEY_f, WLR_MODIFIER_CTRL);
    add(KeybindAction::ToggleMaximizeToEdges, XKB_KEY_m);
    add(KeybindAction::ToggleFloating, XKB_KEY_t);
    add(KeybindAction::TogglePinned, XKB_KEY_p);
    // Overview must not repeat: holding the key would thrash open/close.
    {
      Keybind overview;
      overview.useMod = true;
      overview.keysym = XKB_KEY_o;
      overview.repeat = false;
      overview.action = KeybindAction::OverviewToggle;
      keybinds.push_back(std::move(overview));
    }

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
        workspace.name = std::to_string(index + 1);
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
