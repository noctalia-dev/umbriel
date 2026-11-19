#include "input/global_shortcut_trigger.h"

#include "check.h"

using namespace umbriel;

UMBRIEL_TEST(parsesAndNormalizesPortalChords) {
  const auto trigger = parseGlobalShortcutTrigger("CTRL+ALT+SHIFT+LOGO+M");
  CHECK(trigger.has_value());
  if (!trigger) {
    return;
  }
  CHECK_EQ(trigger->keysym, xkb_keysym_t{XKB_KEY_m});
  CHECK_EQ(describeGlobalShortcutTrigger(*trigger), "Ctrl+Alt+Shift+Super+M");
  CHECK_EQ(parseGlobalShortcutTrigger("CTRL+NUM+a"), parseGlobalShortcutTrigger("CTRL+a"));
  CHECK_EQ(parseGlobalShortcutTrigger("CTRL+CTRL+a"), parseGlobalShortcutTrigger("CTRL+a"));
}

UMBRIEL_TEST(stopsAtUndocumentedCharacters) {
  CHECK_EQ(parseGlobalShortcutTrigger("CTRL+a;ignored"), parseGlobalShortcutTrigger("CTRL+a"));
  CHECK_EQ(parseGlobalShortcutTrigger("ALT+Return comment"), parseGlobalShortcutTrigger("ALT+Return"));
  CHECK(!parseGlobalShortcutTrigger(" CTRL+a"));
  CHECK(!parseGlobalShortcutTrigger("CTRL+;ignored"));
}

UMBRIEL_TEST(rejectsMalformedAndUnsupportedChords) {
  for (const char* text :
       {"", "+a", "CTRL+", "CTRL++a", "ctrl+a", "SUPER+a", "Mod+a", "CTRL+unknown_key", "SHIFT", "Shift_L"}) {
    CHECK(!parseGlobalShortcutTrigger(text));
  }
  CHECK(!parseGlobalShortcutTrigger("ALT+return"));
  CHECK(parseGlobalShortcutTrigger("ALT+Return").has_value());
}

UMBRIEL_TEST(preferredTriggersPreserveOrdinaryTyping) {
  for (const char* text : {"a", "SHIFT+a", "Return", "space", "BackSpace", "Tab", "Left", "NUM+1"}) {
    const auto trigger = parseGlobalShortcutTrigger(text);
    CHECK(trigger.has_value());
    CHECK(trigger && !preferredGlobalShortcutAllowed(*trigger));
  }
  for (const char* text :
       {"CTRL+a", "ALT+Return", "LOGO+m", "F1", "F35", "XF86AudioMute", "Print", "Pause", "Scroll_Lock", "Menu"}) {
    const auto trigger = parseGlobalShortcutTrigger(text);
    CHECK(trigger.has_value());
    CHECK(trigger && preferredGlobalShortcutAllowed(*trigger));
  }
  CHECK(!preferredGlobalShortcutAllowed({}));
  CHECK(describeGlobalShortcutTrigger({}).empty());
}

int main() { return RUN_TESTS(); }
