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
#include <cstddef>
#include <string>
#include <variant>

using umbriel::ActionArgKind;
using umbriel::ActionSpec;
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

UMBRIEL_TEST(parsesLayoutScrollDragAction) {
  Keybind bind;
  CHECK(umbriel::parseAction("layout-scroll-drag", bind));
  CHECK(bind.action == KeybindAction::LayoutScrollDrag);
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

UMBRIEL_TEST(parsesPrimaryExtentFractions) {
  Keybind bind;
  CHECK(parseAction("window-set-primary-extent:0.5", bind));
  CHECK(bind.action == KeybindAction::WindowSetPrimaryExtent);
  const auto* width = umbriel::payloadIf<umbriel::FractionArg>(bind);
  CHECK(width != nullptr);
  CHECK(width != nullptr && std::fabs(width->fraction - 0.5) < 1e-9);

  CHECK(parseAction("window-set-primary-extent:1.0", bind));
  CHECK(parseAction("window-set-primary-extent:0.1", bind));
}

UMBRIEL_TEST(rejectsOutOfRangePrimaryExtentFractions) {
  Keybind bind;
  CHECK(!parseAction("window-set-primary-extent:0", bind)); // below the 0.1 floor
  CHECK(!parseAction("window-set-primary-extent:0.09", bind));
  CHECK(!parseAction("window-set-primary-extent:1.5", bind)); // above 1.0
  CHECK(!parseAction("window-set-primary-extent:-0.5", bind));
  CHECK(!parseAction("window-set-primary-extent:abc", bind));
  CHECK(!parseAction("window-set-primary-extent:0.5x", bind)); // trailing garbage
  CHECK(!parseAction("window-set-primary-extent:", bind));
  CHECK(!parseAction("window-set-primary-extent:nan", bind));
}

UMBRIEL_TEST(parsesPrimaryExtentDeltas) {
  const auto fraction = [](const Keybind& bind) {
    const auto* width = umbriel::payloadIf<umbriel::FractionArg>(bind);
    return width != nullptr ? width->fraction : 0.0;
  };

  Keybind bind;
  CHECK(parseAction("window-modify-primary-extent:-0.2", bind));
  CHECK(bind.action == KeybindAction::WindowModifyPrimaryExtent);
  CHECK(std::fabs(fraction(bind) + 0.2) < 1e-9);

  CHECK(parseAction("window-modify-primary-extent:+0.1", bind)); // explicit plus is allowed
  CHECK(std::fabs(fraction(bind) - 0.1) < 1e-9);

  CHECK(parseAction("window-modify-primary-extent:0.25", bind));
  CHECK(std::fabs(fraction(bind) - 0.25) < 1e-9);
}

UMBRIEL_TEST(rejectsInvalidPrimaryExtentDeltas) {
  Keybind bind;
  CHECK(!parseAction("window-modify-primary-extent:0", bind));    // zero delta is a no-op
  CHECK(!parseAction("window-modify-primary-extent:1.5", bind));  // above the 0.9 cap
  CHECK(!parseAction("window-modify-primary-extent:-1.0", bind)); // below -0.9
  CHECK(!parseAction("window-modify-primary-extent:abc", bind));
  CHECK(!parseAction("window-modify-primary-extent:", bind));      // empty arg
  CHECK(!parseAction("window-modify-primary-extent", bind));       // requires an argument
  CHECK(!parseAction("window-modify-primary-extent:++0.1", bind)); // only one leading '+'
  CHECK(!parseAction("window-modify-primary-extent:0.1x", bind));  // trailing garbage
  CHECK(!parseAction("window-modify-primary-extent:nan", bind));
}

UMBRIEL_TEST(parsesSecondaryExtentActions) {
  Keybind bind;
  CHECK(parseAction("window-set-secondary-extent:0.5", bind));
  CHECK(bind.action == KeybindAction::WindowSetSecondaryExtent);
  const auto* height = umbriel::payloadIf<umbriel::FractionArg>(bind);
  CHECK(height != nullptr);
  CHECK(height != nullptr && std::fabs(height->fraction - 0.5) < 1e-9);

  CHECK(parseAction("window-modify-secondary-extent:-0.2", bind));
  CHECK(bind.action == KeybindAction::WindowModifySecondaryExtent);
  height = umbriel::payloadIf<umbriel::FractionArg>(bind);
  CHECK(height != nullptr && std::fabs(height->fraction + 0.2) < 1e-9);

  CHECK(parseAction("window-modify-secondary-extent:+0.1", bind));
  height = umbriel::payloadIf<umbriel::FractionArg>(bind);
  CHECK(height != nullptr && std::fabs(height->fraction - 0.1) < 1e-9);

  CHECK(!parseAction("window-set-secondary-extent:0.05", bind));
  CHECK(!parseAction("window-modify-secondary-extent:0", bind));
}

UMBRIEL_TEST(rejectsRemovedWidthAndHeightActions) {
  constexpr std::array<std::string_view, 8> removed{
      "window-cycle-width",   "window-cycle-width-back", "window-cycle-height",     "window-cycle-height-back",
      "window-set-width:0.5", "window-set-height:0.5",   "window-modify-width:0.1", "window-modify-height:0.1",
  };
  for (const std::string_view action : removed) {
    Keybind bind;
    CHECK(!parseAction(action, bind));
  }
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
  CHECK(parseAction("layout-master-count-increase", bind));
  CHECK(bind.action == KeybindAction::LayoutMasterCountIncrease);
  CHECK(parseAction("layout-master-count-decrease", bind));
  CHECK(bind.action == KeybindAction::LayoutMasterCountDecrease);
  CHECK(!parseAction("master-count-increase", bind)); // clean cutover: the old name is simply unknown

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
  const auto selector = [](const Keybind& bind) { return umbriel::payloadIf<umbriel::WorkspaceArg>(bind); };

  Keybind bind;
  CHECK(parseAction("workspace-switch:3", bind));
  CHECK(bind.action == KeybindAction::WorkspaceSwitch);
  const auto* position = selector(bind);
  CHECK(position != nullptr);
  const auto* positionValue =
      position != nullptr ? std::get_if<umbriel::WorkspaceIndex>(&position->reference) : nullptr;
  CHECK(positionValue != nullptr);
  CHECK(positionValue != nullptr && positionValue->value == 3);
  CHECK(position != nullptr && position->output.empty());

  CHECK(parseAction("workspace-switch:web/DP-1", bind));
  const auto* named = selector(bind);
  CHECK(named != nullptr);
  const auto* nameValue = named != nullptr ? std::get_if<umbriel::WorkspaceName>(&named->reference) : nullptr;
  CHECK(nameValue != nullptr);
  CHECK(nameValue != nullptr && nameValue->value == "web");
  CHECK(named != nullptr && named->output == "DP-1");

  CHECK(parseAction("window-move-to-workspace:2/HDMI-A-1", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToWorkspace);
  position = selector(bind);
  positionValue = position != nullptr ? std::get_if<umbriel::WorkspaceIndex>(&position->reference) : nullptr;
  CHECK(positionValue != nullptr);
  CHECK(positionValue != nullptr && positionValue->value == 2);
  CHECK(position != nullptr && position->output == "HDMI-A-1");

  CHECK(parseAction("column-move-to-workspace:\"2\"/DP-1", bind));
  CHECK(bind.action == KeybindAction::ColumnMoveToWorkspace);
  named = selector(bind);
  nameValue = named != nullptr ? std::get_if<umbriel::WorkspaceName>(&named->reference) : nullptr;
  CHECK(nameValue != nullptr);
  CHECK(nameValue != nullptr && nameValue->value == "2");
  CHECK(named != nullptr && named->output == "DP-1");

  CHECK(parseAction("workspace-switch:name:2", bind));
  named = selector(bind);
  nameValue = named != nullptr ? std::get_if<umbriel::WorkspaceName>(&named->reference) : nullptr;
  CHECK(nameValue != nullptr);
  CHECK(nameValue != nullptr && nameValue->value == "name:2");
}

UMBRIEL_TEST(rejectsMalformedWorkspaceSelectors) {
  Keybind bind;
  CHECK(!parseAction("workspace-switch:", bind));      // no selector
  CHECK(!parseAction("workspace-switch:/DP-1", bind)); // empty workspace
  CHECK(!parseAction("workspace-switch:web/", bind));  // empty output
  CHECK(!parseAction("workspace-switch:a/b/c", bind)); // two separators
  CHECK(!parseAction("workspace-switch:0", bind));
  CHECK(!parseAction("workspace-switch:65", bind));
  CHECK(!parseAction("workspace-switch:\"\"", bind));
  CHECK(!parseAction("workspace-switch:\"2", bind));
  CHECK(!parseAction("workspace-switch:2\"", bind));
  CHECK(!parseAction("column-move-to-workspace:", bind));
  CHECK(!parseAction("column-move-to-workspace:/DP-1", bind));
}

UMBRIEL_TEST(parsesOptionalOutputActions) {
  const auto outputOf = [](const Keybind& bind) {
    const auto* arg = umbriel::payloadIf<umbriel::OutputArg>(bind);
    return arg != nullptr ? arg->output : std::string{};
  };

  Keybind bind;
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

UMBRIEL_TEST(parsesOptionalScratchpadActions) {
  const auto scratchpadOf = [](const Keybind& bind) {
    const auto* arg = umbriel::payloadIf<umbriel::ScratchpadArg>(bind);
    return arg != nullptr ? arg->name : std::string{};
  };

  Keybind bind;
  CHECK(parseAction("scratchpad-toggle", bind));
  CHECK(bind.action == KeybindAction::ScratchpadToggle);
  // The alternative is present even with no name, so the payload still says
  // which action shape it belongs to.
  CHECK(umbriel::payloadIf<umbriel::ScratchpadArg>(bind) != nullptr);
  CHECK(umbriel::payloadIf<umbriel::OutputArg>(bind) == nullptr);
  CHECK(scratchpadOf(bind).empty());

  CHECK(parseAction("scratchpad-toggle:terminal", bind));
  CHECK_EQ(scratchpadOf(bind), std::string{"terminal"});

  CHECK(parseAction("window-move-to-scratchpad:notes", bind));
  CHECK(bind.action == KeybindAction::WindowMoveToScratchpad);
  CHECK_EQ(scratchpadOf(bind), std::string{"notes"});
  CHECK(parseAction("window-restore-from-scratchpad:music", bind));
  CHECK_EQ(scratchpadOf(bind), std::string{"music"});
  CHECK(parseAction("window-toggle-scratchpad:chat", bind));
  CHECK(bind.action == KeybindAction::WindowToggleScratchpad);
  CHECK_EQ(scratchpadOf(bind), std::string{"chat"});
  CHECK(parseAction("scratchpad-focus-next:terminal", bind));
  CHECK_EQ(scratchpadOf(bind), std::string{"terminal"});

  CHECK(parseAction("window-move-to-scratchpad", bind));
  CHECK(scratchpadOf(bind).empty());
  CHECK(!parseAction("scratchpad-toggle:", bind));
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
    case ActionArgKind::OptionalScratchpad:
    case ActionArgKind::OptionalWindowId:
    case ActionArgKind::SkipConfirmation:
      break;
    case ActionArgKind::Command:
      input += ":value";
      break;
    case ActionArgKind::Fraction:
      input += ":0.5";
      break;
    case ActionArgKind::FractionDelta:
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
    case ActionArgKind::Fraction:
    case ActionArgKind::FractionDelta:
      CHECK(umbriel::payloadIf<umbriel::FractionArg>(bind) != nullptr);
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
    case ActionArgKind::OptionalScratchpad:
      CHECK(umbriel::payloadIf<umbriel::ScratchpadArg>(bind) != nullptr);
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
    case ActionArgKind::OptionalScratchpad:
    case ActionArgKind::OptionalWindowId:
    case ActionArgKind::SkipConfirmation:
      break;
    case ActionArgKind::Command:
      input += ":true";
      break;
    case ActionArgKind::Fraction:
      input += ":0.5";
      break;
    case ActionArgKind::FractionDelta:
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

UMBRIEL_TEST(everySpecHasAOneLineSummary) {
  // `umbriel msg --help` and docs/user/actions.md print these verbatim in a table cell, so a missing, multi-line, or
  // sentence-shaped summary shows up as broken output rather than a prose choice.
  for (const auto& spec : umbriel::actionSpecs()) {
    CHECK(!spec.summary.empty());
    CHECK(spec.summary.size() <= 60);
    CHECK(!spec.summary.ends_with('.'));
    CHECK(!spec.summary.contains('\n'));
    CHECK(!spec.summary.contains('|')); // would break the markdown table cell
    CHECK(!spec.summary.empty() && std::isupper(static_cast<unsigned char>(spec.summary.front())) != 0);
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
  const auto close =
      std::ranges::find_if(binds, [](const Keybind& bind) { return bind.action == KeybindAction::WindowClose; });
  CHECK(close != binds.end());
  CHECK(close->useMod);
  CHECK_EQ(close->modifiers, uint32_t{0});
  CHECK_EQ(close->keysym, xkb_keysym_to_lower(XKB_KEY_q));

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

// Every name `umbriel msg --help` and the keybind reader advertise must round-trip through parseAction with an
// argument of the kind its spec declares, and the help text beside it must describe that same argument. The action
// list is spread across the KeybindAction enum, the kActionSpecs table, and parseAction's switch, so a spec whose
// name, argument kind, or advertised parameter stops agreeing with the others is otherwise only discovered by a user
// typing it.
UMBRIEL_TEST(everyAdvertisedActionParsesWithItsDeclaredArgument) {
  const auto sampleFor = [](ActionArgKind kind) -> std::string {
    switch (kind) {
    case ActionArgKind::None:
      return {};
    case ActionArgKind::Command:
      return ":true";
    case ActionArgKind::Fraction:
      return ":0.5";
    case ActionArgKind::Workspace:
      return ":1";
    case ActionArgKind::OptionalOutput:
      return ":DP-1";
    case ActionArgKind::OptionalScratchpad:
      return ":terminal";
    case ActionArgKind::WindowId:
    case ActionArgKind::OptionalWindowId:
      return ":window-1";
    case ActionArgKind::FractionDelta:
      return ":0.1";
    case ActionArgKind::LayoutMode:
      return ":scrolling";
    case ActionArgKind::SkipConfirmation:
      return ":skip-confirmation";
    }
    return {};
  };

  const auto paramFor = [](ActionArgKind kind) -> std::string_view {
    switch (kind) {
    case ActionArgKind::None:
      return "";
    case ActionArgKind::Command:
      return "<cmd>";
    case ActionArgKind::Fraction:
      return "<fraction>";
    case ActionArgKind::Workspace:
      return "<workspace>[/<output>]";
    case ActionArgKind::OptionalOutput:
      return "[<output>]";
    case ActionArgKind::OptionalScratchpad:
      return "[<scratchpad>]";
    case ActionArgKind::WindowId:
      return "<window-id>";
    case ActionArgKind::OptionalWindowId:
      return "[<window-id>]";
    case ActionArgKind::FractionDelta:
      return "<delta>";
    case ActionArgKind::LayoutMode:
      return "<scrolling|dwindle|master|toggle>";
    case ActionArgKind::SkipConfirmation:
      return "[skip-confirmation]";
    }
    return "";
  };

  size_t swept = 0;
  for (const ActionSpec& spec : umbriel::actionSpecs()) {
    Keybind bind;
    const std::string text = std::string(spec.name) + sampleFor(spec.argKind);
    CHECK(parseAction(text, bind));
    CHECK(bind.action == spec.action);
    // An optional argument must also parse without one.
    if (spec.argKind == ActionArgKind::OptionalOutput
        || spec.argKind == ActionArgKind::OptionalScratchpad
        || spec.argKind == ActionArgKind::OptionalWindowId
        || spec.argKind == ActionArgKind::SkipConfirmation) {
      Keybind bare;
      CHECK(parseAction(spec.name, bare));
      CHECK(bare.action == spec.action);
    }
    // `msg --help` and docs/user/actions.md render `param` verbatim, so it is what the user types against. "submap"
    // shares the name:<text> syntax with "spawn" while naming a submap rather than a shell command.
    const std::string_view expectedParam =
        spec.action == KeybindAction::Submap ? std::string_view{"<name>"} : paramFor(spec.argKind);
    CHECK_EQ(std::string(spec.param), std::string(expectedParam));
    ++swept;
  }
  CHECK(swept > 100);
}

int main() { return RUN_TESTS(); }
