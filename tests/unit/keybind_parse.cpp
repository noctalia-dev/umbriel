#include "config/keybind_parse.h"

#include "check.h"

// clang-format off
// See the note in keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
// clang-format on

#include <algorithm>
#include <variant>

using umbriel::ActionArgKind;
using umbriel::Keybind;
using umbriel::KeybindAction;
using umbriel::parseAction;
using umbriel::parseChord;
using umbriel::WheelDirection;

namespace {

  Keybind chord(std::string_view text) {
    Keybind bind;
    CHECK(parseChord(text, bind));
    return bind;
  }

} // namespace

// parseChord: modifiers
UMBRIEL_TEST(parsesEveryModifierToken) {
  CHECK(chord("Mod+a").useMod);
  CHECK_EQ(chord("Shift+a").modifiers, uint32_t{WLR_MODIFIER_SHIFT});
  CHECK_EQ(chord("Ctrl+a").modifiers, uint32_t{WLR_MODIFIER_CTRL});
  CHECK_EQ(chord("Control+a").modifiers, uint32_t{WLR_MODIFIER_CTRL});
  CHECK_EQ(chord("Alt+a").modifiers, uint32_t{WLR_MODIFIER_ALT});
  CHECK_EQ(chord("Super+a").modifiers, uint32_t{WLR_MODIFIER_LOGO});
  CHECK_EQ(chord("Logo+a").modifiers, uint32_t{WLR_MODIFIER_LOGO});
  CHECK_EQ(chord("Win+a").modifiers, uint32_t{WLR_MODIFIER_LOGO});
}

UMBRIEL_TEST(modifierTokensAreCaseInsensitive) {
  CHECK_EQ(chord("SHIFT+a").modifiers, chord("shift+a").modifiers);
  CHECK_EQ(chord("CtRl+a").modifiers, uint32_t{WLR_MODIFIER_CTRL});
  CHECK(chord("MOD+a").useMod);
}

UMBRIEL_TEST(modIsDistinctFromExplicitSuper) {
  const Keybind withMod = chord("Mod+a");
  const Keybind withSuper = chord("Super+a");
  CHECK(withMod.useMod);
  CHECK_EQ(withMod.modifiers, uint32_t{0});
  CHECK(!withSuper.useMod);
  CHECK_EQ(withSuper.modifiers, uint32_t{WLR_MODIFIER_LOGO});
}
UMBRIEL_TEST(parsesModifierOnlyBinds) {
  const Keybind withMod = chord("Mod");
  CHECK(withMod.modifierOnly);
  CHECK(withMod.useMod);
  CHECK_EQ(withMod.keysym, uint32_t{0});
  CHECK(!withMod.repeat);

  CHECK_EQ(chord("Shift").modifiers, uint32_t{WLR_MODIFIER_SHIFT});
  CHECK_EQ(chord("Ctrl").modifiers, uint32_t{WLR_MODIFIER_CTRL});
  CHECK_EQ(chord("Control").modifiers, uint32_t{WLR_MODIFIER_CTRL});
  CHECK_EQ(chord("Alt").modifiers, uint32_t{WLR_MODIFIER_ALT});
  CHECK_EQ(chord("Super").modifiers, uint32_t{WLR_MODIFIER_LOGO});
  CHECK_EQ(chord("Logo").modifiers, uint32_t{WLR_MODIFIER_LOGO});
  CHECK_EQ(chord("Win").modifiers, uint32_t{WLR_MODIFIER_LOGO});
}

UMBRIEL_TEST(rejectsModifierOnlyCombinations) {
  Keybind bind;
  CHECK(!parseChord("Ctrl+Alt", bind));
  CHECK(!parseChord("Mod+Shift", bind));
}

UMBRIEL_TEST(combinesMultipleModifiers) {
  const Keybind bind = chord("Mod+Ctrl+Shift+q");
  CHECK(bind.useMod);
  CHECK_EQ(bind.modifiers, uint32_t{WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT});
  CHECK_EQ(bind.keysym, uint32_t{XKB_KEY_q});
}

// parseChord: keys
UMBRIEL_TEST(keysymsAreLowercased) {
  CHECK_EQ(chord("Mod+A").keysym, uint32_t{XKB_KEY_a});
  CHECK_EQ(chord("Mod+a").keysym, uint32_t{XKB_KEY_a});
}

UMBRIEL_TEST(parsesNamedAndBareKeys) {
  CHECK_EQ(chord("Escape").keysym, uint32_t{XKB_KEY_Escape});
  CHECK_EQ(chord("Mod+Return").keysym, uint32_t{XKB_KEY_Return});
  CHECK_EQ(chord("Mod+comma").keysym, uint32_t{XKB_KEY_comma});
  CHECK_EQ(chord("Mod+F11").keysym, uint32_t{XKB_KEY_F11});
  CHECK_EQ(chord("Mod+KP_1").keysym, uint32_t{XKB_KEY_KP_1});
}

UMBRIEL_TEST(parsesXf86KeysWithModifiers) {
  CHECK_EQ(chord("XF86AudioMute").keysym, uint32_t{XKB_KEY_XF86AudioMute});

  const Keybind modMute = chord("Mod+XF86AudioMute");
  CHECK(modMute.useMod);
  CHECK_EQ(modMute.keysym, uint32_t{XKB_KEY_XF86AudioMute});

  const Keybind modifiedPlay = chord("Ctrl+Shift+XF86AudioPlay");
  CHECK_EQ(modifiedPlay.modifiers, uint32_t{WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT});
  CHECK_EQ(modifiedPlay.keysym, uint32_t{XKB_KEY_XF86AudioPlay});
}

UMBRIEL_TEST(rejectsUnknownKeysyms) {
  Keybind bind;
  CHECK(!parseChord("Mod+NotAKey", bind));
  CHECK(!parseChord("Mod+", bind));
  CHECK(!parseChord("+a", bind));
  CHECK(!parseChord("Mod++a", bind));
  CHECK(!parseChord("", bind));
  CHECK(!parseChord("Bogus+a", bind)); // unknown modifier
}

UMBRIEL_TEST(failedParseLeavesBindDefaulted) {
  Keybind bind;
  CHECK(parseChord("Mod+Shift+a", bind));
  CHECK(!parseChord("Mod+NotAKey", bind));
  CHECK_EQ(bind.modifiers, uint32_t{0});
  CHECK_EQ(bind.keysym, uint32_t{0});
  CHECK(!bind.useMod);
  CHECK(!bind.modifierOnly);
}

// parseChord: wheel and mouse
UMBRIEL_TEST(parsesWheelDirections) {
  CHECK(chord("Mod+WheelUp").wheel == WheelDirection::Up);
  CHECK(chord("Mod+WheelDown").wheel == WheelDirection::Down);
  CHECK(chord("Mod+WheelLeft").wheel == WheelDirection::Left);
  CHECK(chord("Mod+wheelright").wheel == WheelDirection::Right);
  CHECK_EQ(chord("Mod+WheelUp").keysym, uint32_t{0});
}

UMBRIEL_TEST(parsesMouseButtons) {
  CHECK_EQ(chord("Mod+MouseLeft").mouseButton, uint32_t{BTN_LEFT});
  CHECK_EQ(chord("Mod+MouseRight").mouseButton, uint32_t{BTN_RIGHT});
  CHECK_EQ(chord("Mod+MouseMiddle").mouseButton, uint32_t{BTN_MIDDLE});
  CHECK_EQ(chord("Mod+MouseBack").mouseButton, uint32_t{BTN_SIDE});
  CHECK_EQ(chord("Mod+MouseForward").mouseButton, uint32_t{BTN_EXTRA});
}

UMBRIEL_TEST(rejectsBareWheelAndMouseBinds) {
  // An unmodified wheel or button bind would swallow all client input.
  Keybind bind;
  CHECK(!parseChord("WheelUp", bind));
  CHECK(!parseChord("MouseLeft", bind));
}

// parseChord: submaps
UMBRIEL_TEST(parsesSubmapPrefix) {
  const Keybind bind = chord("submap[resize],Mod+h");
  CHECK_EQ(bind.submap, std::string{"resize"});
  CHECK(bind.useMod);
  CHECK_EQ(bind.keysym, uint32_t{XKB_KEY_h});
}

UMBRIEL_TEST(submapCommaIsOptional) { CHECK_EQ(chord("submap[resize]Escape").submap, std::string{"resize"}); }

UMBRIEL_TEST(rejectsMalformedSubmapPrefix) {
  Keybind bind;
  CHECK(!parseChord("submap[resize", bind));   // unterminated
  CHECK(!parseChord("submap[],Escape", bind)); // empty name
  CHECK(!parseChord("submap[resize],", bind)); // nothing after the prefix
  CHECK(!parseChord("submap[resize]", bind));  // nothing after the prefix
}

// parseAction
UMBRIEL_TEST(parsesSimpleActions) {
  Keybind bind;
  CHECK(parseAction("window-close", bind));
  CHECK(bind.action == KeybindAction::WindowClose);

  CHECK(parseAction("window-focus-switch-floating", bind));
  CHECK(bind.action == KeybindAction::WindowFocusSwitchFloating);
  CHECK(!parseAction("window-focus-toggle-floating-tiling", bind));

  CHECK(parseAction("session-quit", bind));
  CHECK(bind.action == KeybindAction::SessionQuit);
}

UMBRIEL_TEST(parsesSessionQuitConfirmation) {
  Keybind bind;
  CHECK(parseAction("session-quit", bind));
  CHECK(bind.action == KeybindAction::SessionQuit);
  const auto* plain = umbriel::payloadIf<umbriel::QuitArg>(bind);
  CHECK(plain != nullptr);
  CHECK(plain == nullptr || !plain->skipConfirmation);

  CHECK(parseAction("session-quit:skip-confirmation", bind));
  const auto* skip = umbriel::payloadIf<umbriel::QuitArg>(bind);
  CHECK(skip != nullptr);
  CHECK(skip == nullptr || skip->skipConfirmation);

  CHECK(!parseAction("session-quit:bogus", bind));
  CHECK(!parseAction("session-quit:", bind));
}

UMBRIEL_TEST(parsesCommandActions) {
  Keybind bind;
  CHECK(parseAction("spawn:foot -e htop", bind));
  CHECK(bind.action == KeybindAction::Spawn);
  const auto* spawn = umbriel::payloadIf<umbriel::SpawnArg>(bind);
  CHECK(spawn != nullptr);
  CHECK_EQ(spawn != nullptr ? spawn->command : std::string{}, std::string{"foot -e htop"});
}

UMBRIEL_TEST(submapNoLongerSharesStorageWithSpawn) {
  // These used to be the same string field, so a submap name was indistinguishable
  // from a shell command.
  Keybind bind;
  CHECK(parseAction("submap:resize", bind));
  CHECK(bind.action == KeybindAction::Submap);
  const auto* submap = umbriel::payloadIf<umbriel::SubmapArg>(bind);
  CHECK(submap != nullptr);
  CHECK_EQ(submap != nullptr ? submap->name : std::string{}, std::string{"resize"});
  CHECK(umbriel::payloadIf<umbriel::SpawnArg>(bind) == nullptr);

  CHECK(!parseAction("submap", bind));
  CHECK(!parseAction("submap:", bind));
  CHECK(!parseAction("submap:invalid]name", bind));

  CHECK(parseAction("spawn:resize", bind));
  CHECK(umbriel::payloadIf<umbriel::SubmapArg>(bind) == nullptr);
}

UMBRIEL_TEST(onlyResetPopsASubmap) {
  Keybind bind;
  CHECK(parseAction("submap:reset", bind));
  CHECK(umbriel::isSubmapResetBind(bind));
  CHECK(!parseAction("submap:disable", bind));
  CHECK(parseAction("submap:resize", bind));
  CHECK(!umbriel::isSubmapResetBind(bind));

  // Only a submap bind can be a submap reset, whatever its payload says.
  CHECK(parseAction("spawn:reset", bind));
  CHECK(!umbriel::isSubmapResetBind(bind));
}

UMBRIEL_TEST(parsesWidthFractions) {
  Keybind bind;
  CHECK(parseAction("window-set-width:0.5", bind));
  CHECK(bind.action == KeybindAction::WindowSetWidth);
  const auto* width = umbriel::payloadIf<umbriel::WidthArg>(bind);
  CHECK(width != nullptr);
  CHECK(width != nullptr && std::fabs(width->fraction - 0.5) < 1e-9);

  CHECK(parseAction("window-set-width:1.0", bind));
  CHECK(parseAction("window-set-width:0.1", bind));
}

UMBRIEL_TEST(rejectsOutOfRangeWidthFractions) {
  Keybind bind;
  CHECK(!parseAction("window-set-width:0", bind)); // below the 0.1 floor
  CHECK(!parseAction("window-set-width:0.09", bind));
  CHECK(!parseAction("window-set-width:1.5", bind)); // above 1.0
  CHECK(!parseAction("window-set-width:-0.5", bind));
  CHECK(!parseAction("window-set-width:abc", bind));
  CHECK(!parseAction("window-set-width:0.5x", bind)); // trailing garbage
  CHECK(!parseAction("window-set-width:", bind));
  CHECK(!parseAction("window-set-width:nan", bind));
}

UMBRIEL_TEST(parsesWidthDeltas) {
  const auto fraction = [](const Keybind& bind) {
    const auto* width = umbriel::payloadIf<umbriel::WidthArg>(bind);
    return width != nullptr ? width->fraction : 0.0;
  };

  Keybind bind;
  CHECK(parseAction("window-modify-width:-0.2", bind));
  CHECK(bind.action == KeybindAction::WindowModifyWidth);
  CHECK(std::fabs(fraction(bind) + 0.2) < 1e-9);

  CHECK(parseAction("window-modify-width:+0.1", bind)); // explicit plus is allowed
  CHECK(std::fabs(fraction(bind) - 0.1) < 1e-9);

  CHECK(parseAction("window-modify-width:0.25", bind));
  CHECK(std::fabs(fraction(bind) - 0.25) < 1e-9);
}

UMBRIEL_TEST(rejectsInvalidWidthDeltas) {
  Keybind bind;
  CHECK(!parseAction("window-modify-width:0", bind));    // zero delta is a no-op
  CHECK(!parseAction("window-modify-width:1.5", bind));  // above the 0.9 cap
  CHECK(!parseAction("window-modify-width:-1.0", bind)); // below -0.9
  CHECK(!parseAction("window-modify-width:abc", bind));
  CHECK(!parseAction("window-modify-width:", bind));      // empty arg
  CHECK(!parseAction("window-modify-width", bind));       // requires an argument
  CHECK(!parseAction("window-modify-width:++0.1", bind)); // only one leading '+'
  CHECK(!parseAction("window-modify-width:0.1x", bind));  // trailing garbage
  CHECK(!parseAction("window-modify-width:nan", bind));
}

UMBRIEL_TEST(parsesHeightActions) {
  Keybind bind;
  CHECK(parseAction("window-set-height:0.5", bind));
  CHECK(bind.action == KeybindAction::WindowSetHeight);
  const auto* height = umbriel::payloadIf<umbriel::WidthArg>(bind);
  CHECK(height != nullptr);
  CHECK(height != nullptr && std::fabs(height->fraction - 0.5) < 1e-9);

  CHECK(parseAction("window-modify-height:-0.2", bind));
  CHECK(bind.action == KeybindAction::WindowModifyHeight);
  height = umbriel::payloadIf<umbriel::WidthArg>(bind);
  CHECK(height != nullptr && std::fabs(height->fraction + 0.2) < 1e-9);

  CHECK(parseAction("window-modify-height:+0.1", bind));
  height = umbriel::payloadIf<umbriel::WidthArg>(bind);
  CHECK(height != nullptr && std::fabs(height->fraction - 0.1) < 1e-9);

  CHECK(!parseAction("window-set-height:0.05", bind));
  CHECK(!parseAction("window-modify-height:0", bind));
}

UMBRIEL_TEST(parsesLayoutModeActions) {
  Keybind bind;
  CHECK(parseAction("workspace-set-layout:scrolling", bind));
  CHECK(bind.action == KeybindAction::WorkspaceSetLayout);
  const auto* scrolling = umbriel::payloadIf<umbriel::LayoutModeArg>(bind);
  CHECK(scrolling != nullptr);
  CHECK(scrolling != nullptr && scrolling->mode == umbriel::LayoutMode::Scrolling);

  CHECK(parseAction("workspace-set-layout:dwindle", bind));
  const auto* dwindle = umbriel::payloadIf<umbriel::LayoutModeArg>(bind);
  CHECK(dwindle != nullptr && dwindle->mode == umbriel::LayoutMode::Dwindle);

  CHECK(parseAction("workspace-set-layout:master", bind));
  const auto* master = umbriel::payloadIf<umbriel::LayoutModeArg>(bind);
  CHECK(master != nullptr && master->mode == umbriel::LayoutMode::Master);

  CHECK(parseAction("workspace-set-layout:toggle", bind));
  const auto* toggle = umbriel::payloadIf<umbriel::LayoutModeArg>(bind);
  CHECK(toggle != nullptr && !toggle->mode.has_value());
}

UMBRIEL_TEST(rejectsInvalidLayoutModeActions) {
  Keybind bind;
  CHECK(!parseAction("workspace-set-layout:spiral", bind));    // not a known mode
  CHECK(!parseAction("workspace-set-layout:", bind));          // empty arg
  CHECK(!parseAction("workspace-set-layout", bind));           // requires an argument
  CHECK(!parseAction("workspace-set-layout:Scrolling", bind)); // exact lowercase only
}

UMBRIEL_TEST(parsesArgumentFreeNewActions) {
  Keybind bind;
  CHECK(parseAction("workspace-next", bind));
  CHECK(bind.action == KeybindAction::WorkspaceNext);
  CHECK(std::holds_alternative<std::monostate>(bind.payload));

  CHECK(parseAction("workspace-previous", bind));
  CHECK(bind.action == KeybindAction::WorkspacePrevious);

  CHECK(parseAction("window-move-to-workspace-next", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToWorkspaceNext);

  CHECK(parseAction("window-move-to-workspace-previous", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToWorkspacePrevious);

  CHECK(parseAction("column-move-to-workspace-next", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToWorkspaceNext);

  CHECK(parseAction("column-move-to-workspace-previous", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToWorkspacePrevious);

  CHECK(parseAction("output-focus-left", bind));
  CHECK(bind.action == KeybindAction::OutputFocusLeft);
  CHECK(parseAction("output-focus-right", bind));
  CHECK(bind.action == KeybindAction::OutputFocusRight);

  CHECK(parseAction("window-center", bind));
  CHECK(bind.action == KeybindAction::WindowCenter);

  CHECK(parseAction("window-toggle-maximize-to-edges", bind));
  CHECK(bind.action == KeybindAction::ToggleMaximizeToEdges);
  CHECK(parseAction("column-focus-first", bind));
  CHECK(bind.action == KeybindAction::ColumnFocusFirst);
  CHECK(parseAction("column-focus-last", bind));
  CHECK(bind.action == KeybindAction::ColumnFocusLast);

  CHECK(parseAction("column-move-to-first", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToFirst);
  CHECK(parseAction("column-move-to-last", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToLast);

  CHECK(parseAction("window-focus-previous", bind));
  CHECK(bind.action == KeybindAction::WindowFocusPrevious);
  CHECK(parseAction("window-swap-next", bind));
  CHECK(bind.action == KeybindAction::WindowSwapNext);
  CHECK(parseAction("window-swap-previous", bind));
  CHECK(bind.action == KeybindAction::WindowSwapPrevious);
  CHECK(parseAction("master-count-increase", bind));
  CHECK(bind.action == KeybindAction::MasterCountIncrease);
  CHECK(parseAction("master-count-decrease", bind));
  CHECK(bind.action == KeybindAction::MasterCountDecrease);

  CHECK(parseAction("window-focus-last", bind));
  CHECK(bind.action == KeybindAction::WindowFocusLast);
  CHECK(parseAction("window-consume-left", bind));
  CHECK(bind.action == KeybindAction::WindowConsumeLeft);
  CHECK(parseAction("window-consume-or-expel-left", bind));
  CHECK(bind.action == KeybindAction::WindowConsumeOrExpelLeft);
  CHECK(parseAction("window-consume-right", bind));
  CHECK(bind.action == KeybindAction::WindowConsumeRight);
  CHECK(parseAction("window-consume-or-expel-right", bind));
  CHECK(bind.action == KeybindAction::WindowConsumeOrExpelRight);
  CHECK(!parseAction("window-consume-or-expel", bind));
  CHECK(!parseAction("window-expel-right", bind));

  // Argument-free actions reject arguments.
  CHECK(!parseAction("workspace-next:1", bind));
  CHECK(!parseAction("window-move-to-workspace-next:1", bind));
  CHECK(!parseAction("window-move-to-workspace-previous:1", bind));
  CHECK(!parseAction("column-move-to-workspace-next:1", bind));
  CHECK(!parseAction("column-move-to-workspace-previous:1", bind));
  CHECK(!parseAction("output-focus-left:DP-1", bind));
  CHECK(!parseAction("window-center:x", bind));
  CHECK(!parseAction("window-toggle-maximize-to-edges:x", bind));
  CHECK(!parseAction("window-focus-last:x", bind));
  CHECK(!parseAction("window-consume-left:x", bind));
  CHECK(!parseAction("window-consume-or-expel-left:x", bind));
  CHECK(!parseAction("window-consume-right:x", bind));
  CHECK(!parseAction("window-consume-or-expel-right:x", bind));
}

UMBRIEL_TEST(parsesWorkspaceSelectors) {
  const auto selector = [](const Keybind& bind) {
    static const umbriel::WorkspaceArg empty;
    const auto* arg = umbriel::payloadIf<umbriel::WorkspaceArg>(bind);
    return arg != nullptr ? *arg : empty;
  };

  Keybind bind;
  CHECK(parseAction("workspace-switch:3", bind));
  CHECK(bind.action == KeybindAction::WorkspaceSwitch);
  CHECK_EQ(selector(bind).name, std::string{"3"});
  CHECK(selector(bind).output.empty());

  CHECK(parseAction("workspace-switch:web/DP-1", bind));
  CHECK_EQ(selector(bind).name, std::string{"web"});
  CHECK_EQ(selector(bind).output, std::string{"DP-1"});

  CHECK(parseAction("window-move-to-workspace:2/HDMI-A-1", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToWorkspace);
  CHECK_EQ(selector(bind).name, std::string{"2"});
  CHECK_EQ(selector(bind).output, std::string{"HDMI-A-1"});

  CHECK(parseAction("column-move-to-workspace:chat/DP-1", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToWorkspace);
  CHECK_EQ(selector(bind).name, std::string{"chat"});
  CHECK_EQ(selector(bind).output, std::string{"DP-1"});
}

UMBRIEL_TEST(rejectsMalformedWorkspaceSelectors) {
  Keybind bind;
  CHECK(!parseAction("workspace-switch:", bind));      // no selector
  CHECK(!parseAction("workspace-switch:/DP-1", bind)); // empty workspace
  CHECK(!parseAction("workspace-switch:web/", bind));  // empty output
  CHECK(!parseAction("workspace-switch:a/b/c", bind)); // two separators
  CHECK(!parseAction("column-move-to-workspace:", bind));
  CHECK(!parseAction("column-move-to-workspace:/DP-1", bind));
}

UMBRIEL_TEST(parsesOptionalOutputActions) {
  const auto outputOf = [](const Keybind& bind) {
    const auto* arg = umbriel::payloadIf<umbriel::OutputArg>(bind);
    return arg != nullptr ? arg->output : std::string{};
  };

  Keybind bind;
  CHECK(parseAction("scratchpad-toggle", bind));
  CHECK(bind.action == KeybindAction::ScratchpadToggle);
  // The alternative is present even with no output, so the payload still says
  // which action shape it belongs to.
  CHECK(umbriel::payloadIf<umbriel::OutputArg>(bind) != nullptr);
  CHECK(outputOf(bind).empty());

  CHECK(parseAction("scratchpad-toggle:DP-2", bind));
  CHECK_EQ(outputOf(bind), std::string{"DP-2"});

  CHECK(parseAction("window-move-to-scratchpad", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToScratchpad);
  CHECK(parseAction("window-restore-from-scratchpad:eDP-1", bind));
  CHECK_EQ(outputOf(bind), std::string{"eDP-1"});
  CHECK(parseAction("window-toggle-scratchpad", bind));
  CHECK(bind.action == KeybindAction::WindowToggleScratchpad);
  CHECK(parseAction("scratchpad-focus-next", bind));

  CHECK(parseAction("dpms-off", bind));
  CHECK(bind.action == KeybindAction::DpmsOff);
  CHECK(outputOf(bind).empty());
  CHECK(parseAction("dpms-off:DP-1", bind));
  CHECK_EQ(outputOf(bind), std::string{"DP-1"});
  CHECK(parseAction("dpms-on", bind));
  CHECK(bind.action == KeybindAction::DpmsOn);
  CHECK(parseAction("dpms-on:eDP-1", bind));
  CHECK_EQ(outputOf(bind), std::string{"eDP-1"});
}

UMBRIEL_TEST(parsesWindowIdActions) {
  Keybind bind;
  CHECK(parseAction("window-close", bind));
  CHECK_EQ(umbriel::payloadIf<umbriel::WindowIdArg>(bind)->id, std::string{});
  CHECK(parseAction("window-close:abc123", bind));
  CHECK(bind.action == KeybindAction::WindowClose);
  CHECK_EQ(umbriel::payloadIf<umbriel::WindowIdArg>(bind)->id, std::string{"abc123"});
  CHECK(parseAction("window-focus:abc123", bind));
  CHECK(bind.action == KeybindAction::WindowFocusId);
  CHECK_EQ(umbriel::payloadIf<umbriel::WindowIdArg>(bind)->id, std::string{"abc123"});
  CHECK(parseAction("window-focus-warp:abc123", bind));
  CHECK(bind.action == KeybindAction::WindowFocusWarpId);
  CHECK_EQ(umbriel::payloadIf<umbriel::WindowIdArg>(bind)->id, std::string{"abc123"});
}

UMBRIEL_TEST(payloadAlternativeMatchesTheDeclaredArgKind) {
  // The whole point of the variant: the spec's argKind and the payload the
  // parser produces cannot drift apart.
  for (const auto& spec : umbriel::actionSpecs()) {
    Keybind bind;
    std::string input(spec.name);
    switch (spec.argKind) {
    case ActionArgKind::None:
    case ActionArgKind::OptionalOutput:
    case ActionArgKind::OptionalWindowId:
    case ActionArgKind::SkipConfirmation:
      break;
    case ActionArgKind::Command:
      input += ":value";
      break;
    case ActionArgKind::WidthFraction:
      input += ":0.5";
      break;
    case ActionArgKind::WidthDelta:
      input += ":0.1";
      break;
    case ActionArgKind::LayoutMode:
      input += ":toggle";
      break;
    case ActionArgKind::Workspace:
      input += ":1";
      break;
    case ActionArgKind::WindowId:
      input += ":abc";
      break;
    }
    CHECK(parseAction(input, bind));

    switch (spec.argKind) {
    case ActionArgKind::None:
      CHECK(std::holds_alternative<std::monostate>(bind.payload));
      break;
    case ActionArgKind::Command:
      CHECK(
          umbriel::payloadIf<umbriel::SpawnArg>(bind) != nullptr
          || umbriel::payloadIf<umbriel::SubmapArg>(bind) != nullptr
      );
      break;
    case ActionArgKind::WidthFraction:
    case ActionArgKind::WidthDelta:
      CHECK(umbriel::payloadIf<umbriel::WidthArg>(bind) != nullptr);
      break;
    case ActionArgKind::LayoutMode:
      CHECK(umbriel::payloadIf<umbriel::LayoutModeArg>(bind) != nullptr);
      break;
    case ActionArgKind::Workspace:
      CHECK(umbriel::payloadIf<umbriel::WorkspaceArg>(bind) != nullptr);
      break;
    case ActionArgKind::OptionalOutput:
      CHECK(umbriel::payloadIf<umbriel::OutputArg>(bind) != nullptr);
      break;
    case ActionArgKind::WindowId:
    case ActionArgKind::OptionalWindowId:
      CHECK(umbriel::payloadIf<umbriel::WindowIdArg>(bind) != nullptr);
      break;
    case ActionArgKind::SkipConfirmation:
      CHECK(umbriel::payloadIf<umbriel::QuitArg>(bind) != nullptr);
      break;
    }
  }
}

UMBRIEL_TEST(rejectsUnknownActions) {
  Keybind bind;
  CHECK(!parseAction("", bind));
  CHECK(!parseAction("not-an-action", bind));
  CHECK(!parseAction("window-clos", bind));        // truncated
  CHECK(!parseAction("window-close-extra", bind)); // superstring
  CHECK(!parseAction("overview-open:arg", bind));  // takes no argument
  CHECK(!parseAction("window-focus", bind));       // requires an argument
  CHECK(!parseAction("window-focus:", bind));      // requires a non-empty argument
  CHECK(!parseAction("spawn", bind));              // requires an argument
  CHECK(!parseAction("spawn:", bind));             // requires a non-empty argument
}

// action spec table
UMBRIEL_TEST(everyActionSpecRoundTripsThroughParseAction) {
  // The registry is duplicated across the enum, this table, and the dispatch
  // switch. At minimum, every advertised name must parse back to its action.
  for (const auto& spec : umbriel::actionSpecs()) {
    Keybind bind;
    std::string input(spec.name);
    switch (spec.argKind) {
    case ActionArgKind::None:
    case ActionArgKind::OptionalOutput:
    case ActionArgKind::OptionalWindowId:
    case ActionArgKind::SkipConfirmation:
      break;
    case ActionArgKind::Command:
      input += ":true";
      break;
    case ActionArgKind::WidthFraction:
      input += ":0.5";
      break;
    case ActionArgKind::WidthDelta:
      input += ":0.1";
      break;
    case ActionArgKind::LayoutMode:
      input += ":toggle";
      break;
    case ActionArgKind::Workspace:
      input += ":1";
      break;
    case ActionArgKind::WindowId:
      input += ":abc";
      break;
    }
    if (!parseAction(input, bind)) {
      CHECK(parseAction(input, bind));
      continue;
    }
    CHECK(bind.action == spec.action);
  }
}

UMBRIEL_TEST(actionSpecNamesAreUniqueAndSorted) {
  const auto specs = umbriel::actionSpecs();
  CHECK(!specs.empty());
  CHECK(std::ranges::is_sorted(specs, {}, &umbriel::ActionSpec::name));
  for (size_t i = 1; i < specs.size(); ++i) {
    CHECK(specs[i - 1].name != specs[i].name);
  }
}

UMBRIEL_TEST(everyActionHasASpec) {
  // actions.cpp guards the handler table with a consteval everyActionHasHandler.
  // Nothing guarded the name table, so an action could ship with a handler, a
  // cheatsheet row, and docs while staying unbindable and unreachable over IPC.
  std::array<bool, static_cast<size_t>(KeybindAction::Count)> named{};
  for (const auto& spec : umbriel::actionSpecs()) {
    named[static_cast<size_t>(spec.action)] = true;
  }
  // Enumerator 0 is None, which is never bindable. On failure the reported
  // "got" value is the index of the first action that has no name.
  size_t firstUnnamed = named.size();
  for (size_t action = 1; action < named.size(); ++action) {
    if (!named[action]) {
      firstUnnamed = action;
      break;
    }
  }
  CHECK_EQ(firstUnnamed, named.size());
}

UMBRIEL_TEST(parameterizedSpecsDeclareAParam) {
  for (const auto& spec : umbriel::actionSpecs()) {
    if (spec.argKind == ActionArgKind::None) {
      CHECK(spec.param.empty());
    } else {
      CHECK(!spec.param.empty());
    }
  }
}

// defaults
UMBRIEL_TEST(defaultKeybindsAreUsable) {
  const auto binds = umbriel::defaultKeybinds();
  CHECK(!binds.empty());

  // Every default is Mod-based, so a bare keystroke always reaches the client.
  CHECK(std::ranges::all_of(binds, [](const Keybind& bind) { return bind.useMod; }));

  // No default may carry an unset action.
  CHECK(std::ranges::none_of(binds, [](const Keybind& bind) { return bind.action == KeybindAction::None; }));

  // Overview toggle must not key-repeat: holding it would thrash open/close.
  const auto overview =
      std::ranges::find_if(binds, [](const Keybind& bind) { return bind.action == KeybindAction::OverviewToggle; });
  CHECK(overview != binds.end());
  CHECK(!overview->repeat);

  // Workspaces 1-9 are bound on both the number row and the keypad.
  const auto switches =
      std::ranges::count_if(binds, [](const Keybind& bind) { return bind.action == KeybindAction::WorkspaceSwitch; });
  CHECK_EQ(switches, 18);
}

int main() { return RUN_TESTS(); }
