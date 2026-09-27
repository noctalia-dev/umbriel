#include "config/effects.h"

#include "check.h"
#include "config/config.h"
#include "scene/effect_ledger.h"
#include "scene/effect_registry.h"
#include "view/effects.h"

#include <cstdint>

using umbriel::EffectKind;
using umbriel::EffectPreset;
using umbriel::Effects;

namespace {
  Effects twoPresets() {
    Effects effects;
    effects.presets.push_back(EffectPreset{.name = "pulse", .kind = EffectKind::Border, .shader = {.code = "x"}});
    effects.presets.push_back(EffectPreset{.name = "lines", .kind = EffectKind::Window, .shader = {.code = "y"}});
    return effects;
  }
} // namespace

UMBRIEL_TEST(effectKindsParseTheirCanonicalNamesOnly) {
  CHECK(umbriel::parseEffectKind("animation") == EffectKind::Animation);
  CHECK(umbriel::parseEffectKind("border") == EffectKind::Border);
  CHECK(umbriel::parseEffectKind("window") == EffectKind::Window);
  CHECK(umbriel::parseEffectKind("screen") == EffectKind::Screen);
  CHECK(umbriel::parseEffectKind("cursor") == EffectKind::Cursor);
  CHECK(!umbriel::parseEffectKind("Border"));
  CHECK(!umbriel::parseEffectKind("overlay"));
  CHECK(!umbriel::parseEffectKind(""));
  CHECK_EQ(umbriel::effectKindName(EffectKind::Cursor), std::string_view("cursor"));
}

UMBRIEL_TEST(effectReferencesRequireAnExistingPresetOfTheRightKind) {
  const Effects effects = twoPresets();
  CHECK(umbriel::findEffectPreset(effects, "pulse") != nullptr);
  CHECK(umbriel::findEffectPreset(effects, "PULSE") == nullptr);
  CHECK(!umbriel::effectReferenceError(effects, "", EffectKind::Border, false));
  CHECK(!umbriel::effectReferenceError(effects, "pulse", EffectKind::Border, false));
  CHECK(umbriel::effectReferenceError(effects, "pulse", EffectKind::Window, false));
  CHECK(umbriel::effectReferenceError(effects, "pulse", EffectKind::Animation, false));
  Effects fades;
  fades.presets.push_back(EffectPreset{.name = "fade", .kind = EffectKind::Animation, .shader = {.code = "z"}});
  CHECK(!umbriel::effectReferenceError(fades, "fade", EffectKind::Animation, false));
  CHECK(umbriel::effectReferenceError(fades, "fade", EffectKind::Border, false));
  CHECK(umbriel::effectReferenceError(effects, "missing", EffectKind::Window, false));
}

UMBRIEL_TEST(offIsOnlyValidWhereAnOverrideCanDisableTheDefault) {
  const Effects effects = twoPresets();
  CHECK(!umbriel::effectReferenceError(effects, "off", EffectKind::Border, true));
  CHECK(umbriel::effectReferenceError(effects, "off", EffectKind::Border, false));
}

UMBRIEL_TEST(clockSecondsKeepsMillisecondResolutionPastAFloatsExactRange) {
  // A raw millisecond count this large already exceeds a float's exact integer range (2^24, ~4.7
  // hours): a plain `static_cast<float>(msec) / 1000.0F` on it collapses a 1 ms step to nothing.
  // Measuring from an epoch keeps the value handed to clockSeconds() small, so the same step survives.
  constexpr uint64_t epoch = 7ULL * 24 * 3600 * 1000;
  const float naiveAt = static_cast<float>(epoch + 1) / 1000.0F;
  const float naiveBefore = static_cast<float>(epoch) / 1000.0F;
  CHECK_EQ(naiveAt, naiveBefore);

  const float at = umbriel::effectClockSeconds(epoch + 1, epoch);
  const float before = umbriel::effectClockSeconds(epoch, epoch);
  CHECK(at != before);
}

UMBRIEL_TEST(inertPresetsKeepTheirNameWithoutSource) {
  EffectPreset preset{.name = "broken", .kind = EffectKind::Screen};
  CHECK(preset.inert());
  preset.shader.code = "vec4 screen(vec2 uv) { return umbriel_sample(uv); }";
  CHECK(!preset.inert());
}

UMBRIEL_TEST(ledgerCountsOnlyVisibleAdvancingTimeReaders) {
  umbriel::EffectLedger ledger;
  int outputA = 0;
  int outputB = 0;
  int viewOne = 0;
  int viewTwo = 0;
  ledger.update(&viewOne, {.output = &outputA, .visible = true, .readsTime = true, .advancing = true});
  ledger.update(&viewTwo, {.output = &outputA, .visible = true, .readsTime = false, .advancing = true});
  CHECK_EQ(ledger.eligible(&outputA), 1U);
  CHECK_EQ(ledger.eligible(&outputB), 0U);
  CHECK_EQ(ledger.active(), 2U);

  // A frozen clock, animated = false, or speed = 0 all arrive as advancing = false.
  ledger.update(&viewOne, {.output = &outputA, .visible = true, .readsTime = true, .advancing = false});
  CHECK_EQ(ledger.eligible(&outputA), 0U);
  ledger.update(&viewOne, {.output = &outputA, .visible = true, .readsTime = true, .advancing = true});
  CHECK_EQ(ledger.eligible(&outputA), 1U);

  // Hidden or off-output instances never request frames.
  ledger.update(&viewOne, {.output = &outputA, .visible = false, .readsTime = true, .advancing = true});
  CHECK_EQ(ledger.eligible(&outputA), 0U);
  ledger.update(&viewOne, {.output = &outputB, .visible = true, .readsTime = true, .advancing = true});
  CHECK_EQ(ledger.eligible(&outputA), 0U);
  CHECK_EQ(ledger.eligible(&outputB), 1U);

  ledger.remove(&viewOne);
  CHECK_EQ(ledger.eligible(&outputB), 0U);
  CHECK_EQ(ledger.active(), 1U);
}

UMBRIEL_TEST(ledgerSuspensionAndOutputRemovalClearEligibility) {
  umbriel::EffectLedger ledger;
  int output = 0;
  int screen = 0;
  int cursor = 0;
  ledger.update(&screen, {.output = &output, .visible = true, .readsTime = true, .advancing = true});
  ledger.update(&cursor, {.output = &output, .visible = true, .readsTime = true, .advancing = true});
  CHECK_EQ(ledger.eligible(&output), 2U);
  ledger.setSuspended(true);
  CHECK(ledger.suspended());
  CHECK_EQ(ledger.eligible(&output), 0U);
  CHECK_EQ(ledger.active(), 2U);
  ledger.setSuspended(false);
  CHECK_EQ(ledger.eligible(&output), 2U);
  ledger.removeOutput(&output);
  CHECK_EQ(ledger.eligible(&output), 0U);
  CHECK_EQ(ledger.active(), 0U);
}

UMBRIEL_TEST(ledgerUpdatesReplaceAnOwnersPreviousState) {
  umbriel::EffectLedger ledger;
  int output = 0;
  int owner = 0;
  for (int i = 0; i < 3; ++i) {
    ledger.update(&owner, {.output = &output, .visible = true, .readsTime = true, .advancing = true});
  }
  CHECK_EQ(ledger.eligible(&output), 1U);
  CHECK_EQ(ledger.active(), 1U);
}

UMBRIEL_TEST(viewEffectNamesFollowTheMostSpecificSelector) {
  umbriel::Effects effects;
  effects.border = "pulse";
  effects.window = "lines";
  umbriel::ResolvedWindowRule rule;
  auto names = umbriel::resolveViewEffectNames(effects, rule);
  CHECK_EQ(names.border, std::string("pulse"));
  CHECK_EQ(names.window, std::string("lines"));
  rule.borderEffect = "off";
  rule.windowEffect = "scan";
  names = umbriel::resolveViewEffectNames(effects, rule);
  CHECK(names.border.empty());
  CHECK_EQ(names.window, std::string("scan"));
  rule.borderEffect = "";
  names = umbriel::resolveViewEffectNames(effects, rule);
  CHECK(names.border.empty());
}

UMBRIEL_TEST(borderEffectsApplyOnlyToFocusedDecoratedCalmWindows) {
  CHECK(umbriel::borderEffectApplies({.focused = true, .decorated = true, .urgent = false, .fullscreen = false}));
  CHECK(!umbriel::borderEffectApplies({.focused = false, .decorated = true, .urgent = false, .fullscreen = false}));
  CHECK(!umbriel::borderEffectApplies({.focused = true, .decorated = false, .urgent = false, .fullscreen = false}));
  CHECK(!umbriel::borderEffectApplies({.focused = true, .decorated = true, .urgent = true, .fullscreen = false}));
  CHECK(!umbriel::borderEffectApplies({.focused = true, .decorated = true, .urgent = false, .fullscreen = true}));
}

UMBRIEL_TEST(borderPaddingNeedsACompiledBorderPreset) {
  const EffectPreset border{.name = "ring", .kind = EffectKind::Border, .padding = 20};
  CHECK_EQ(umbriel::borderPresetPadding(&border, true), 20);
  CHECK_EQ(umbriel::borderPresetPadding(&border, false), 0);
  CHECK_EQ(umbriel::borderPresetPadding(nullptr, true), 0);
  const EffectPreset window{.name = "tint", .kind = EffectKind::Window, .padding = 20};
  CHECK_EQ(umbriel::borderPresetPadding(&window, true), 0);
}

UMBRIEL_TEST(screenEffectNameFollowsTheOutputOverride) {
  umbriel::Effects effects;
  effects.screen = "vig";
  CHECK_EQ(umbriel::resolveScreenEffectName(effects, nullptr), std::string("vig"));
  umbriel::OutputRule rule;
  CHECK_EQ(umbriel::resolveScreenEffectName(effects, &rule), std::string("vig"));
  rule.screenEffect = "off";
  CHECK(umbriel::resolveScreenEffectName(effects, &rule).empty());
  rule.screenEffect = "crt";
  CHECK_EQ(umbriel::resolveScreenEffectName(effects, &rule), std::string("crt"));
}

int main() { return RUN_TESTS(); }
