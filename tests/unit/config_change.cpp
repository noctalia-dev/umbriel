#include "check.h"
#include "config/change.h"

using umbriel::AccelProfile;
using umbriel::ClickMethod;
using umbriel::Config;
using umbriel::ConfigChange;
using umbriel::ConfigEffects;
using umbriel::ContentType;
using umbriel::Keybind;
using umbriel::LayerRule;
using umbriel::ModifierKey;
using umbriel::OutputRule;
using umbriel::SecurityContextRule;
using umbriel::WindowRule;

UMBRIEL_TEST(anIdenticalConfigChangesNothing) {
  const Config before;
  const Config after;
  const ConfigChange change = ConfigChange::between(before, after);
  // The whole point of the item: a reload that parsed the same file again must
  // report nothing to do, so nothing is re-applied and nothing flickers.
  CHECK(!change.any());
}

UMBRIEL_TEST(aFirstLoadReportsEverything) {
  const ConfigChange change = ConfigChange::everything();
  CHECK(change.any());
  CHECK(change.appearance);
  CHECK(change.animation);
  CHECK(change.colors);
  CHECK(change.events);
  CHECK(change.input);
  CHECK(change.outputs);
}

UMBRIEL_TEST(eachSectionIsReportedOnItsOwn) {
  const Config before;

  {
    Config after;
    after.colors.accentPrimary[0] += 0.1F;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.colors);
    CHECK(!change.appearance);
    CHECK_EQ(change.summary(), std::string("colors"));
  }
  {
    Config after;
    after.appearance.borderWidth += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.appearance);
    // A border width change must not reprogram keyboards or reconfigure outputs.
    CHECK(!change.input);
    CHECK(!change.outputs);
    CHECK(!change.keybinds);
  }
  {
    Config after;
    after.input.keyboard.repeatRate += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.input);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.input.mouse.accelProfile = AccelProfile{};
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.input);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.input.touchpad.accelProfile.emplace();
    after.input.touchpad.accelProfile->kind = AccelProfile::Kind::Flat;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.input);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.input.touchpad.sensitivity = 0.5;
    CHECK(ConfigChange::between(before, after).input);
  }
  {
    Config after;
    after.input.touchpad.scrollFactor = 1.5;
    CHECK(ConfigChange::between(before, after).input);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.input.touchpad.disableWhileTyping = false;
    CHECK(ConfigChange::between(before, after).input);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.input.touchpad.disableOnExternalMouse = true;
    CHECK(ConfigChange::between(before, after).input);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.input.touchpad.clickMethod = ClickMethod::ClickFinger;
    CHECK(ConfigChange::between(before, after).input);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.input.middleClickPaste = !after.input.middleClickPaste;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.input);
    CHECK(!change.general);
    CHECK(ConfigEffects::between(before, after).input);
  }
  {
    Config after;
    after.layout.gap += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.layout);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.general.xwayland = !after.general.xwayland;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.general);
    CHECK(!change.input);
  }
  {
    Config after;
    after.events.lidClose = "systemctl suspend";
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.events);
    CHECK(!change.general);
    CHECK(!change.input);
    CHECK_EQ(change.summary(), std::string("events"));
  }
  {
    Config after;
    after.overview.zoom += 0.1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.overview);
    CHECK(!change.layout);
  }
  {
    Config after;
    after.workspaces.backAndForth = !after.workspaces.backAndForth;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.workspaces);
    CHECK(!change.workspaceRules);
    CHECK(!ConfigEffects::between(before, after).workspaceInventory);
  }
  {
    Config after;
    after.workspaces.emptyAbove = !after.workspaces.emptyAbove;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.workspaces);
    const ConfigEffects effects = ConfigEffects::between(before, after);
    CHECK(effects.workspaceInventory);
    CHECK(effects.workspaceLayout);
  }
  {
    Config after;
    after.animation.windowsMove.durationMs += 1;
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.animation);
    CHECK(!change.appearance);
    CHECK_EQ(change.summary(), std::string("animation"));
    const ConfigEffects effects = ConfigEffects::between(before, after);
    CHECK(effects.animation);
    CHECK(!effects.viewChrome);
  }
  {
    Config after;
    after.animation.dimUnfocused.dim = 0.25;
    const ConfigEffects effects = ConfigEffects::between(before, after);
    CHECK(effects.animation);
    CHECK(effects.viewChrome);
  }
}

UMBRIEL_TEST(nestedAppearanceChangesAreCaught) {
  const Config before;
  Config after;
  // Nested structs need their own comparison, so a change buried a level down is
  // exactly what a defaulted operator== would miss if one were forgotten.
  after.appearance.blur.radius += 1;
  CHECK(ConfigChange::between(before, after).appearance);

  Config shadowed;
  shadowed.appearance.shadow.offsetX += 1;
  CHECK(ConfigChange::between(before, shadowed).appearance);

  Config scrolled;
  scrolled.layout.scrolling.defaultWidthFraction = 0.6;
  CHECK(ConfigChange::between(before, scrolled).layout);

  Config focused;
  focused.input.focus.followsMouse = !focused.input.focus.followsMouse;
  CHECK(ConfigChange::between(before, focused).input);
}

UMBRIEL_TEST(borderColorChangesAreColorChangesThatRefreshChrome) {
  const Config before;
  Config after;
  after.colors.border.focused[0] += 0.1F;
  const ConfigChange change = ConfigChange::between(before, after);
  CHECK(change.colors);
  CHECK(!change.appearance);

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.viewChrome);
  CHECK(effects.internalUi);
  CHECK(effects.overviewPresentation);
  CHECK(!effects.workspaceLayout);
  CHECK(!effects.sceneBlur);
}

UMBRIEL_TEST(listSectionsAreCompared) {
  const Config before;

  {
    Config after;
    after.keybinds.push_back(Keybind{});
    const ConfigChange change = ConfigChange::between(before, after);
    CHECK(change.keybinds);
    CHECK(!change.appearance);
  }
  {
    Config after;
    after.outputs.push_back(OutputRule{});
    CHECK(ConfigChange::between(before, after).outputs);
  }
  {
    Config after;
    after.windowRules.push_back(WindowRule{});
    CHECK(ConfigChange::between(before, after).windowRules);
  }
  {
    Config after;
    after.layerRules.push_back(LayerRule{});
    CHECK(ConfigChange::between(before, after).layerRules);
  }
  {
    Config after;
    after.securityContextRules.push_back(SecurityContextRule{});
    CHECK(ConfigChange::between(before, after).securityContextRules);
  }
}

UMBRIEL_TEST(ruleEqualityIgnoresTheCompiledRegex) {
  // Two rules built from the same pattern are the same rule, even though their
  // std::regex members are distinct objects that cannot be compared at all.
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  first.appIdRegex = std::regex(first.appIdPattern);
  first.xdgTagPattern = "^main-window$";
  first.xdgTagRegex = std::regex(first.xdgTagPattern);
  WindowRule second;
  second.appIdPattern = "kitty";
  second.appIdRegex = std::regex(second.appIdPattern);
  second.xdgTagPattern = "^main-window$";
  second.xdgTagRegex = std::regex(second.xdgTagPattern);
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(!ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(ruleEqualityStillSeesAnXdgTagChange) {
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "game";
  first.xdgTagPattern = "^game-launcher$";
  WindowRule second;
  second.appIdPattern = "game";
  second.xdgTagPattern = "^game-running$";
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(ruleEqualityStillSeesAContentTypeChange) {
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  first.matchContentType = ContentType::Game;
  WindowRule second;
  second.appIdPattern = "kitty";
  second.matchContentType = ContentType::Video;
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(ruleEqualitySeesAnOptionChangeUnderTheSamePattern) {
  Config before;
  Config after;
  WindowRule first;
  first.appIdPattern = "kitty";
  WindowRule second;
  second.appIdPattern = "kitty";
  second.opacity = 0.9;
  before.windowRules.push_back(std::move(first));
  after.windowRules.push_back(std::move(second));

  CHECK(ConfigChange::between(before, after).windowRules);
}

UMBRIEL_TEST(identicalConfigsProduceNoRuntimeEffects) {
  const Config before;
  const Config after;
  CHECK(!ConfigEffects::between(before, after).any());
}

UMBRIEL_TEST(firstLoadInvalidatesEveryRuntimeConsumer) {
  const ConfigEffects effects = ConfigEffects::everything();
  CHECK(effects.outputState);
  CHECK(effects.tearingPolicy);
  CHECK(effects.directScanoutPolicy);
  CHECK(effects.workspaceInventory);
  CHECK(effects.workspaceLayout);
  CHECK(effects.sceneBlur);
  CHECK(effects.viewChrome);
  CHECK(effects.layerEffects);
  CHECK(effects.animation);
  CHECK(effects.input);
  CHECK(effects.overviewPresentation);
  CHECK(effects.internalUi);
}

UMBRIEL_TEST(anyColorChangeRefreshesEveryColorConsumer) {
  const Config before;
  Config after;
  after.colors.textPrimary[0] -= 0.1F;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK_EQ(effects.summary(), std::string("view chrome, internal UI, overview presentation"));
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceLayout);
  CHECK(!effects.sceneBlur);
  CHECK(!effects.input);
}

UMBRIEL_TEST(borderWidthRefreshesChromeAndWorkspaceLayout) {
  const Config before;
  Config after;
  after.appearance.borderWidth += 1;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.workspaceLayout);
  CHECK(effects.viewChrome);
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceInventory);
  CHECK(!effects.sceneBlur);
  CHECK(!effects.layerEffects);
  CHECK(!effects.input);
}

UMBRIEL_TEST(layoutGapDoesNotReapplyOutputState) {
  const Config before;
  Config after;
  after.layout.gap += 1;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.workspaceLayout);
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceInventory);
  CHECK(!effects.viewChrome);
}

UMBRIEL_TEST(layoutStrutsOnlyRefreshWorkspaceLayout) {
  const Config before;
  Config after;
  after.layout.struts.left = 32;
  after.layout.struts.bottom = -8;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.workspaceLayout);
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceInventory);
  CHECK(!effects.viewChrome);
  CHECK(!effects.layerEffects);
}

UMBRIEL_TEST(workspaceRuleStrutsOnlyRefreshWorkspaceLayout) {
  Config before;
  umbriel::WorkspaceConfig rule;
  rule.name = "dev";
  before.workspaceRules.push_back(rule);
  Config after = before;
  after.workspaceRules[0].layout.struts.left = 24;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.workspaceLayout);
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceInventory);
  CHECK(!effects.viewChrome);
  CHECK(!effects.layerEffects);
}

UMBRIEL_TEST(outputStateAndWorkspaceInventoryAreIndependent) {
  Config before;
  OutputRule original;
  original.name = "HEADLESS-1";
  before.outputs.push_back(original);

  Config stateChanged = before;
  stateChanged.outputs[0].scale = 2.0;
  const ConfigEffects stateEffects = ConfigEffects::between(before, stateChanged);
  CHECK(stateEffects.outputState);
  CHECK(!stateEffects.workspaceInventory);
  CHECK(!stateEffects.workspaceLayout);

  Config vrrChanged = before;
  vrrChanged.outputs[0].vrr = umbriel::VrrMode::Fullscreen;
  const ConfigEffects vrrEffects = ConfigEffects::between(before, vrrChanged);
  CHECK(vrrEffects.outputState);
  CHECK(!vrrEffects.workspaceInventory);

  Config hdrChanged = before;
  hdrChanged.outputs[0].hdr = umbriel::HdrMode::On;
  const ConfigEffects hdrEffects = ConfigEffects::between(before, hdrChanged);
  CHECK(hdrEffects.outputState);
  CHECK(!hdrEffects.workspaceInventory);

  Config sdrWhiteChanged = before;
  sdrWhiteChanged.outputs[0].sdrWhite = 300.0F;
  const ConfigEffects sdrWhiteEffects = ConfigEffects::between(before, sdrWhiteChanged);
  CHECK(sdrWhiteEffects.outputState);
  CHECK(!sdrWhiteEffects.workspaceInventory);

  Config inventoryChanged = before;
  inventoryChanged.outputs[0].workspaces = std::vector<std::string>{"1", "dev"};
  const ConfigEffects inventoryEffects = ConfigEffects::between(before, inventoryChanged);
  CHECK(!inventoryEffects.outputState);
  CHECK(inventoryEffects.workspaceInventory);
  CHECK(inventoryEffects.workspaceLayout);

  Config minimumChanged = before;
  minimumChanged.outputs[0].minWorkspaces = 3;
  const ConfigEffects minimumEffects = ConfigEffects::between(before, minimumChanged);
  CHECK(!minimumEffects.outputState);
  CHECK(minimumEffects.workspaceInventory);
  CHECK(minimumEffects.workspaceLayout);

  Config disabled = before;
  disabled.outputs[0].enabled = false;
  const ConfigEffects disableEffects = ConfigEffects::between(before, disabled);
  CHECK(disableEffects.outputState);
  CHECK(!disableEffects.workspaceInventory);
  CHECK(!disableEffects.workspaceLayout);

  const ConfigEffects reenableEffects = ConfigEffects::between(disabled, before);
  CHECK(reenableEffects.outputState);
  CHECK(!reenableEffects.workspaceInventory);
}

UMBRIEL_TEST(outputScrollingDefaultOnlyRefreshesWorkspaceLayout) {
  Config before;
  OutputRule output;
  output.name = "HEADLESS-1";
  before.outputs.push_back(output);

  Config after = before;
  after.outputs[0].layout.scrolling.defaultWidthFraction = 0.75;

  const ConfigChange change = ConfigChange::between(before, after);
  CHECK(change.outputs);
  CHECK(!change.layout);

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.workspaceLayout);
  CHECK(!effects.outputState);
  CHECK(!effects.tearingPolicy);
  CHECK(!effects.directScanoutPolicy);
  CHECK(!effects.workspaceInventory);
  CHECK(!effects.sceneBlur);
  CHECK(!effects.viewChrome);
  CHECK(!effects.layerEffects);
  CHECK(!effects.animation);
  CHECK(!effects.input);
  CHECK(!effects.overviewPresentation);
  CHECK(!effects.internalUi);
  CHECK(effects.invalidatesOverview());
  CHECK_EQ(effects.summary(), std::string("workspace layout"));
}

UMBRIEL_TEST(outputRuleNameSetChangesRefreshIdentityDependentEffects) {
  Config before;
  OutputRule connector;
  connector.name = "HDMI-A-1";
  before.outputs.push_back(connector);

  Config descriptorAdded = before;
  OutputRule descriptor;
  descriptor.name = "Microstep MSI G2712F CD6T084401192";
  descriptorAdded.outputs.push_back(descriptor);
  const ConfigEffects added = ConfigEffects::between(before, descriptorAdded);
  CHECK(added.outputState);
  CHECK(added.tearingPolicy);
  CHECK(added.directScanoutPolicy);
  CHECK(added.workspaceInventory);
  CHECK(added.workspaceLayout);

  Config caseOnly = before;
  caseOnly.outputs[0].name = "hdmi-a-1";
  const ConfigEffects caseEffects = ConfigEffects::between(before, caseOnly);
  CHECK(!caseEffects.outputState);
  CHECK(!caseEffects.tearingPolicy);
  CHECK(!caseEffects.directScanoutPolicy);
  CHECK(!caseEffects.workspaceInventory);
  CHECK(!caseEffects.workspaceLayout);
}

UMBRIEL_TEST(tearingPolicyDoesNotReapplyOutputStateOrInvalidateOverview) {
  Config before;
  OutputRule output;
  output.name = "HEADLESS-1";
  before.outputs.push_back(output);

  Config allowed = before;
  allowed.outputs[0].allowTearing = true;
  const ConfigEffects outputEffects = ConfigEffects::between(before, allowed);
  CHECK(outputEffects.tearingPolicy);
  CHECK(!outputEffects.outputState);
  CHECK(!outputEffects.workspaceInventory);
  CHECK(!outputEffects.workspaceLayout);
  CHECK(!outputEffects.invalidatesOverview());
  CHECK_EQ(outputEffects.summary(), std::string("tearing policy"));

  Config forcedByRule = before;
  WindowRule game;
  game.appIdPattern = "^game$";
  game.xdgTagPattern = "^game-running$";
  game.xdgTagRegex = std::regex(game.xdgTagPattern);
  game.matchContentType = ContentType::Game;
  game.allowTearing = true;
  forcedByRule.windowRules.push_back(game);
  const ConfigEffects ruleEffects = ConfigEffects::between(before, forcedByRule);
  CHECK(ruleEffects.tearingPolicy);
  CHECK(ruleEffects.viewChrome);
  CHECK(!ruleEffects.outputState);
  CHECK_EQ(ruleEffects.summary(), std::string("tearing policy, view chrome"));

  Config vetoedByRule = before;
  game.allowTearing = false;
  vetoedByRule.windowRules.push_back(game);
  CHECK(ConfigEffects::between(before, vetoedByRule).tearingPolicy);

  Config changedContentMatcher = forcedByRule;
  changedContentMatcher.windowRules[0].matchContentType = ContentType::Video;
  CHECK(ConfigEffects::between(forcedByRule, changedContentMatcher).tearingPolicy);

  Config changedTagMatcher = forcedByRule;
  changedTagMatcher.windowRules[0].xdgTagPattern = "^game-launcher$";
  changedTagMatcher.windowRules[0].xdgTagRegex = std::regex(changedTagMatcher.windowRules[0].xdgTagPattern);
  CHECK(ConfigEffects::between(forcedByRule, changedTagMatcher).tearingPolicy);

  Config unrelatedRule = before;
  WindowRule translucent;
  translucent.appIdPattern = "^terminal$";
  translucent.matchContentType = ContentType::Photo;
  translucent.opacity = 0.9;
  unrelatedRule.windowRules.push_back(translucent);
  const ConfigEffects unrelatedEffects = ConfigEffects::between(before, unrelatedRule);
  CHECK(!unrelatedEffects.tearingPolicy);
  CHECK(unrelatedEffects.viewChrome);
}

UMBRIEL_TEST(directScanoutPolicyForcesOnlyItsRuntimeEffect) {
  Config before;
  OutputRule output;
  output.name = "HEADLESS-1";
  before.outputs.push_back(output);

  Config disabled = before;
  disabled.outputs[0].directScanout = false;
  const ConfigEffects disableEffects = ConfigEffects::between(before, disabled);
  CHECK(disableEffects.directScanoutPolicy);
  CHECK(!disableEffects.outputState);
  CHECK(!disableEffects.tearingPolicy);
  CHECK(!disableEffects.workspaceInventory);
  CHECK(!disableEffects.workspaceLayout);
  CHECK(!disableEffects.invalidatesOverview());
  CHECK_EQ(disableEffects.summary(), std::string("direct scanout policy"));

  const ConfigEffects enableEffects = ConfigEffects::between(disabled, before);
  CHECK(enableEffects.directScanoutPolicy);
  CHECK(!enableEffects.outputState);

  Config additionalOutput = before;
  OutputRule second;
  second.name = "DP-1";
  additionalOutput.outputs.push_back(second);
  CHECK(ConfigEffects::between(before, additionalOutput).directScanoutPolicy);

  Config onlyDisabled;
  onlyDisabled.outputs.push_back(disabled.outputs[0]);
  CHECK(ConfigEffects::between(onlyDisabled, Config{}).directScanoutPolicy);

  Config onlyDefault;
  onlyDefault.outputs.push_back(before.outputs[0]);
  CHECK(ConfigEffects::between(onlyDefault, Config{}).directScanoutPolicy);
}

UMBRIEL_TEST(vrrPolicyTracksFullscreenOnlyWhenRequested) {
  CHECK(!umbriel::vrrEnabled(umbriel::VrrMode::Disabled, false));
  CHECK(!umbriel::vrrEnabled(umbriel::VrrMode::Disabled, true));
  CHECK(umbriel::vrrEnabled(umbriel::VrrMode::Always, false));
  CHECK(umbriel::vrrEnabled(umbriel::VrrMode::Always, true));
  CHECK(!umbriel::vrrEnabled(umbriel::VrrMode::Fullscreen, false));
  CHECK(umbriel::vrrEnabled(umbriel::VrrMode::Fullscreen, true));
}

UMBRIEL_TEST(hdrPolicyTracksEligibilityForEachMode) {
  CHECK(!umbriel::hdrEnabled(umbriel::HdrMode::Off, false, false));
  CHECK(umbriel::hdrEnabled(umbriel::HdrMode::On, false, false));
  CHECK(!umbriel::hdrEnabled(umbriel::HdrMode::Auto, true, false));
  CHECK(umbriel::hdrEnabled(umbriel::HdrMode::Auto, true, true));
  CHECK(!umbriel::hdrEnabled(umbriel::HdrMode::Fullscreen, false, false));
  CHECK(umbriel::hdrEnabled(umbriel::HdrMode::Fullscreen, true, false));
}

UMBRIEL_TEST(blurRulesAndInputReachOnlyTheirConsumers) {
  const Config before;

  Config blurred;
  blurred.appearance.blur.radius += 1;
  const ConfigEffects blurEffects = ConfigEffects::between(before, blurred);
  CHECK(blurEffects.sceneBlur);
  CHECK(blurEffects.viewChrome);
  CHECK(blurEffects.layerEffects);
  CHECK(!blurEffects.workspaceLayout);

  Config windowed;
  windowed.windowRules.push_back(WindowRule{});
  const ConfigEffects windowEffects = ConfigEffects::between(before, windowed);
  CHECK(windowEffects.viewChrome);
  CHECK(!windowEffects.layerEffects);

  Config layered;
  layered.layerRules.push_back(LayerRule{});
  const ConfigEffects layerEffects = ConfigEffects::between(before, layered);
  CHECK(layerEffects.layerEffects);
  CHECK(!layerEffects.viewChrome);

  Config inputChanged;
  inputChanged.input.keyboard.repeatRate += 1;
  const ConfigEffects inputEffects = ConfigEffects::between(before, inputChanged);
  CHECK(inputEffects.input);
  CHECK(!inputEffects.outputState);
  CHECK(!inputEffects.workspaceLayout);
  CHECK(!inputEffects.viewChrome);

  Config cursorChanged;
  cursorChanged.input.cursor.hardwareCursor = false;
  CHECK(ConfigEffects::between(before, cursorChanged).input);
}

UMBRIEL_TEST(ruleRequestingOptimizedBlurReachesTheOutputs) {
  Config before;
  before.appearance.blur.optimized = false;
  CHECK(!before.optimizedBlurNeeded());

  // Adding a window rule normally only touches view chrome, but a rule that
  // asks for the cached background blur has to create it on every output.
  Config windowed = before;
  WindowRule rule;
  rule.blurOptimized = true;
  windowed.windowRules.push_back(rule);
  CHECK(windowed.optimizedBlurNeeded());
  CHECK(ConfigEffects::between(before, windowed).sceneBlur);

  Config layered = before;
  LayerRule layerRule;
  layerRule.optimized = true;
  layered.layerRules.push_back(layerRule);
  CHECK(layered.optimizedBlurNeeded());
  CHECK(ConfigEffects::between(before, layered).sceneBlur);

  Config optedOut = before;
  WindowRule plain;
  plain.blurOptimized = false;
  optedOut.windowRules.push_back(plain);
  CHECK(!optedOut.optimizedBlurNeeded());
  CHECK(!ConfigEffects::between(before, optedOut).sceneBlur);
}

UMBRIEL_TEST(overviewInvalidationExcludesIrrelevantRuntimeEffects) {
  const Config before;

  Config overviewChanged;
  overviewChanged.overview.zoom += 0.1;
  const ConfigEffects overviewEffects = ConfigEffects::between(before, overviewChanged);
  CHECK(overviewEffects.overviewPresentation);
  CHECK(overviewEffects.invalidatesOverview());
  CHECK_EQ(overviewEffects.summary(), std::string("overview presentation"));

  Config generalChanged;
  generalChanged.general.autostart.emplace_back("true");
  const ConfigEffects generalEffects = ConfigEffects::between(before, generalChanged);
  CHECK(!generalEffects.any());
  CHECK(!generalEffects.invalidatesOverview());

  Config inputChanged;
  inputChanged.input.keyboard.repeatDelay += 1;
  const ConfigEffects inputEffects = ConfigEffects::between(before, inputChanged);
  CHECK(inputEffects.any());
  CHECK(!inputEffects.invalidatesOverview());

  Config hotCornerChanged;
  hotCornerChanged.hotCorners.corners[0].enabled = true;
  const ConfigEffects hotCornerEffects = ConfigEffects::between(before, hotCornerChanged);
  CHECK(hotCornerEffects.input);
  CHECK(!hotCornerEffects.overviewPresentation);
  CHECK(!hotCornerEffects.invalidatesOverview());
  CHECK(ConfigChange::between(before, hotCornerChanged).hotCorners);
}

UMBRIEL_TEST(modKeyReloadRefreshesOnlyInternalUi) {
  const Config before;
  Config after;
  after.general.modKey = ModifierKey::Control;

  const ConfigEffects effects = ConfigEffects::between(before, after);
  CHECK(effects.internalUi);
  CHECK(!effects.input);
  CHECK(!effects.outputState);
  CHECK(!effects.workspaceLayout);
  CHECK(!effects.viewChrome);
}

UMBRIEL_TEST(aFailedReloadResultCarriesNoChangesOrEffects) {
  const umbriel::ConfigReloadResult result;
  CHECK(!result.success);
  CHECK(!result.change.any());
  CHECK(!result.effects.any());
}

int main() { return RUN_TESTS(); }
