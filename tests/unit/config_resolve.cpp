#include "check.h"
#include "config/resolve.h"

#include <regex>
#include <string>
#include <utility>
#include <vector>

using umbriel::Config;
using umbriel::ContentType;
using umbriel::LayerRule;
using umbriel::LayoutMode;
using umbriel::OutputRule;
using umbriel::VrrMode;
using umbriel::WindowRule;
using umbriel::WorkspaceConfig;

UMBRIEL_TEST(globalLayoutUsesTheCallerOwnedConfig) {
  Config first;
  first.layout.gap = 11;
  first.appearance.borderWidth = 5;
  first.appearance.outerBorderWidth = 3;
  const umbriel::ResolvedLayoutConfig firstLayout = umbriel::resolveGlobalLayout(first);

  CHECK_EQ(firstLayout.gap, 11);
  CHECK_EQ(firstLayout.totalGap, 27);
  CHECK_EQ(firstLayout.edgePad, 19);
  CHECK_EQ(first.layoutGap(), firstLayout.totalGap);
  CHECK_EQ(first.layoutEdgePad(), firstLayout.edgePad);
  CHECK(!firstLayout.dwindle.preserveSplit);

  Config second;
  second.layout.gap = 4;
  second.appearance.borderWidth = 1;
  second.appearance.outerBorderWidth = 0;
  const umbriel::ResolvedLayoutConfig secondLayout = umbriel::resolveGlobalLayout(second);

  CHECK_EQ(secondLayout.totalGap, 6);
  CHECK_EQ(secondLayout.edgePad, 5);
  CHECK(firstLayout != secondLayout);
}

UMBRIEL_TEST(workspaceOverridesApplyGlobalThenOutputSpecificRules) {
  Config config;
  config.layout.gap = 8;
  config.appearance.borderWidth = 2;
  config.layout.master.position = umbriel::MasterPosition::Right;
  config.layout.master.defaultWidthFraction = 0.58;
  config.layout.master.newOnTop = false;
  config.layout.dwindle.preserveSplit = true;

  WorkspaceConfig global;
  global.name = "dev";
  global.layout.gap = 12;
  global.layout.scrolling.defaultWidthFraction = 0.6;
  global.layout.master.defaultWidthFraction = 0.6;
  global.layout.master.newOnTop = true;
  global.layout.dwindle.preserveSplit = false;
  config.workspaceRules.push_back(std::move(global));

  WorkspaceConfig dpOne;
  dpOne.name = "dev";
  dpOne.output = "DP-1";
  dpOne.layout.gap = 20;
  dpOne.layout.mode = LayoutMode::Dwindle;
  dpOne.layout.master.position = umbriel::MasterPosition::Left;
  dpOne.layout.master.defaultWidthFraction = 0.7;
  dpOne.layout.master.newOnTop = false;
  dpOne.layout.dwindle.preserveSplit = true;
  config.workspaceRules.push_back(std::move(dpOne));

  WorkspaceConfig dpTwo;
  dpTwo.name = "dev";
  dpTwo.output = "DP-2";
  dpTwo.layout.gap = 30;
  config.workspaceRules.push_back(std::move(dpTwo));

  const auto onDpOne = umbriel::resolveWorkspaceLayout(config, "DP-1", "dev", 0);
  CHECK(onDpOne.mode == LayoutMode::Dwindle);
  CHECK_EQ(onDpOne.gap, 20);
  CHECK_EQ(onDpOne.totalGap, 24);
  CHECK_EQ(onDpOne.edgePad, 22);
  CHECK(onDpOne.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*onDpOne.scrolling.defaultWidthFraction, 0.6);
  CHECK(onDpOne.master.position == umbriel::MasterPosition::Left);
  CHECK_EQ(onDpOne.master.defaultWidthFraction, 0.7);
  CHECK(!onDpOne.master.newOnTop);
  CHECK(onDpOne.dwindle.preserveSplit);

  const auto onDpTwo = umbriel::resolveWorkspaceLayout(config, "DP-2", "dev", 0);
  CHECK(onDpTwo.mode == LayoutMode::Scrolling);
  CHECK_EQ(onDpTwo.gap, 30);
  CHECK(onDpTwo.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*onDpTwo.scrolling.defaultWidthFraction, 0.6);
  CHECK(onDpTwo.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(onDpTwo.master.defaultWidthFraction, 0.6);
  CHECK(onDpTwo.master.newOnTop);
  CHECK(!onDpTwo.dwindle.preserveSplit);

  const auto elsewhere = umbriel::resolveWorkspaceLayout(config, "HDMI-A-1", "dev", 0);
  CHECK_EQ(elsewhere.gap, 12);
  CHECK(elsewhere.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*elsewhere.scrolling.defaultWidthFraction, 0.6);
  CHECK(elsewhere.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(elsewhere.master.defaultWidthFraction, 0.6);
  CHECK(elsewhere.master.newOnTop);
  CHECK(!elsewhere.dwindle.preserveSplit);
}

UMBRIEL_TEST(omittedScrollingDefaultWidthRemainsUnset) {
  Config config;

  const auto global = umbriel::resolveGlobalLayout(config);
  CHECK(!global.scrolling.defaultWidthFraction.has_value());

  const auto workspace = umbriel::resolveWorkspaceLayout(config, "DP-1", "dev", 0);
  CHECK(!workspace.scrolling.defaultWidthFraction.has_value());
}

UMBRIEL_TEST(workspaceInventoryResolvesStaticAndDynamicOutputs) {
  Config config;
  OutputRule fixed;
  fixed.name = "DP-1";
  fixed.workspaces = std::vector<std::string>{"main", "web"};
  config.outputs.push_back(std::move(fixed));

  WorkspaceConfig web;
  web.name = "web";
  web.output = "DP-1";
  web.layout.gap = 24;
  config.workspaceRules.push_back(std::move(web));

  const auto staticSet = umbriel::resolveWorkspacesForOutput(config, "DP-1");
  CHECK(!staticSet.dynamic);
  CHECK_EQ(staticSet.workspaces.size(), size_t{2});
  CHECK_EQ(staticSet.workspaces[0].name, std::string{"main"});
  CHECK_EQ(staticSet.workspaces[1].name, std::string{"web"});
  CHECK_EQ(staticSet.workspaces[1].layout.gap, 24);

  const auto dynamicSet = umbriel::resolveWorkspacesForOutput(config, "DP-2");
  CHECK(dynamicSet.dynamic);
  CHECK_EQ(dynamicSet.workspaces.size(), size_t{1});
  CHECK_EQ(dynamicSet.workspaces[0].name, std::string{"1"});

  config.workspaces.emptyAbove = true;
  const auto dynamicSetWithEmptyAbove = umbriel::resolveWorkspacesForOutput(config, "DP-2");
  CHECK(dynamicSetWithEmptyAbove.dynamic);
  CHECK_EQ(dynamicSetWithEmptyAbove.workspaces.size(), size_t{2});
  if (dynamicSetWithEmptyAbove.workspaces.size() == 2) {
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[0].name, std::string{"1"});
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[1].name, std::string{"2"});
  }
}

UMBRIEL_TEST(fixedWorkspacePositionSelectsItsUniqueOutput) {
  Config config;

  OutputRule primary;
  primary.name = "DP-1";
  primary.workspaces = std::vector<std::string>{"1", "2", "3", "4"};
  config.outputs.push_back(std::move(primary));

  OutputRule chat;
  chat.name = "HDMI-A-1";
  chat.workspaces = std::vector<std::string>{"CHAT"};
  config.outputs.push_back(std::move(chat));

  OutputRule dynamic;
  dynamic.name = "DP-2";
  config.outputs.push_back(std::move(dynamic));

  CHECK_EQ(umbriel::uniqueFixedWorkspaceOwner(config, 3), config.outputs.data());
  CHECK(umbriel::uniqueFixedWorkspaceOwner(config, 0) == nullptr);
  CHECK(umbriel::uniqueFixedWorkspaceOwner(config, 4) == nullptr);
}

UMBRIEL_TEST(windowRulesMergeMatchingFieldsInOrder) {
  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.opacity = 0.5;
  app.blur = true;
  app.defaultFocused = false;
  app.defaultPinned = true;
  app.focusOnActivate = false;
  app.vrr = VrrMode::Disabled;
  app.allowTearing = false;
  app.hdr = umbriel::HdrMode::Off;
  app.defaultPosition = umbriel::WindowPosition{
      .x = 12,
      .y = 24,
      .anchor = umbriel::WindowPositionAnchor::TopRight,
  };
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "shell";
  title.titleRegex = std::regex(title.titlePattern);
  title.opacity = 0.8;
  title.focusOnActivate = true;
  title.vrr = VrrMode::Always;
  title.allowTearing = true;
  title.hdr = umbriel::HdrMode::On;
  title.defaultPinned = false;
  config.windowRules.push_back(std::move(title));

  WindowRule unfocused;
  unfocused.matchFocused = false;
  unfocused.defaultFloating = true;
  config.windowRules.push_back(std::move(unfocused));

  const auto resolved = umbriel::resolveWindowRules(config, "foot", "project shell", "", ContentType::None, false);
  CHECK(resolved.opacity && *resolved.opacity == 0.8);
  CHECK(resolved.blur && *resolved.blur);
  CHECK(resolved.defaultFloating && *resolved.defaultFloating);
  CHECK(resolved.defaultPosition.has_value());
  CHECK_EQ(resolved.defaultPosition->x, 12);
  CHECK_EQ(resolved.defaultPosition->y, 24);
  CHECK(resolved.defaultPosition->anchor == umbriel::WindowPositionAnchor::TopRight);
  CHECK(resolved.defaultFocused && !*resolved.defaultFocused);
  CHECK(resolved.defaultPinned && !*resolved.defaultPinned);
  CHECK(resolved.focusOnActivate && *resolved.focusOnActivate);
  CHECK(resolved.vrr == VrrMode::Always);
  CHECK(resolved.allowTearing && *resolved.allowTearing);
  CHECK(resolved.hdr == umbriel::HdrMode::On);

  const auto appOnly = umbriel::resolveWindowRules(config, "foot", "editor", "", ContentType::None, false);
  CHECK(appOnly.defaultPinned && *appOnly.defaultPinned);
  CHECK(appOnly.vrr == VrrMode::Disabled);
  CHECK(appOnly.allowTearing && !*appOnly.allowTearing);
  CHECK(appOnly.hdr == umbriel::HdrMode::Off);

  const auto focused = umbriel::resolveWindowRules(config, "foot", "project shell", "", ContentType::None, true);
  CHECK(focused.opacity && *focused.opacity == 0.8);
  CHECK(!focused.defaultFloating);
  CHECK(umbriel::anyWindowRuleHasTitlePattern(config));
}

UMBRIEL_TEST(windowRulesMergeFractionSizingLastWriterWins) {
  Config config;

  WindowRule first;
  first.appIdPattern = "^utility$";
  first.appIdRegex = std::regex(first.appIdPattern);
  first.defaultFloating = true;
  first.defaultWidth = 0.5;
  first.defaultHeight = 0.6;
  config.windowRules.push_back(std::move(first));

  WindowRule second;
  second.appIdPattern = "^utility$";
  second.appIdRegex = std::regex(second.appIdPattern);
  second.defaultWidth = 0.75;
  config.windowRules.push_back(std::move(second));

  const auto resolved = umbriel::resolveWindowRules(config, "utility", "", "", ContentType::None, false);
  CHECK(resolved.defaultFloating && *resolved.defaultFloating);
  // Later rules overwrite only the fields they set.
  CHECK(resolved.defaultWidth && *resolved.defaultWidth == 0.75);
  CHECK(resolved.defaultHeight && *resolved.defaultHeight == 0.6);
}

UMBRIEL_TEST(windowRulesMatchContentTypesAndComposeSelectors) {
  Config config;

  WindowRule photo;
  photo.matchContentType = ContentType::Photo;
  photo.opacity = 0.25;
  config.windowRules.push_back(std::move(photo));

  WindowRule game;
  game.appIdPattern = "^runner$";
  game.appIdRegex = std::regex(game.appIdPattern);
  game.titlePattern = "playing";
  game.titleRegex = std::regex(game.titlePattern);
  game.matchContentType = ContentType::Game;
  game.matchFocused = false;
  game.opacity = 0.75;
  config.windowRules.push_back(std::move(game));

  WindowRule none;
  none.matchContentType = ContentType::None;
  none.defaultFloating = true;
  config.windowRules.push_back(std::move(none));

  const auto matchingGame = umbriel::resolveWindowRules(config, "runner", "now playing", "", ContentType::Game, false);
  CHECK(matchingGame.opacity && *matchingGame.opacity == 0.75);

  const auto wrongApp = umbriel::resolveWindowRules(config, "launcher", "now playing", "", ContentType::Game, false);
  CHECK(!wrongApp.opacity);
  const auto wrongTitle = umbriel::resolveWindowRules(config, "runner", "paused", "", ContentType::Game, false);
  CHECK(!wrongTitle.opacity);
  const auto wrongFocus = umbriel::resolveWindowRules(config, "runner", "now playing", "", ContentType::Game, true);
  CHECK(!wrongFocus.opacity);

  const auto matchingPhoto = umbriel::resolveWindowRules(config, "viewer", "photo", "", ContentType::Photo, false);
  CHECK(matchingPhoto.opacity && *matchingPhoto.opacity == 0.25);

  const auto matchingNone = umbriel::resolveWindowRules(config, "terminal", "shell", "", ContentType::None, false);
  CHECK(matchingNone.defaultFloating && *matchingNone.defaultFloating);

  const auto video = umbriel::resolveWindowRules(config, "viewer", "video", "", ContentType::Video, false);
  CHECK(!video.opacity);
  CHECK(!video.defaultFloating);
}

UMBRIEL_TEST(windowRulesMatchXdgTagsAndComposeSelectors) {
  Config config;

  WindowRule anyGameTag;
  anyGameTag.xdgTagPattern = "^game-";
  anyGameTag.xdgTagRegex = std::regex(anyGameTag.xdgTagPattern);
  anyGameTag.opacity = 0.25;
  config.windowRules.push_back(std::move(anyGameTag));

  WindowRule launcher;
  launcher.xdgTagPattern = "^game-launcher$";
  launcher.xdgTagRegex = std::regex(launcher.xdgTagPattern);
  launcher.opacity = 0.9;
  launcher.defaultFloating = true;
  config.windowRules.push_back(std::move(launcher));

  WindowRule running;
  running.appIdPattern = "^runner$";
  running.appIdRegex = std::regex(running.appIdPattern);
  running.titlePattern = "playing";
  running.titleRegex = std::regex(running.titlePattern);
  running.xdgTagPattern = "^game-(running|settings)$";
  running.xdgTagRegex = std::regex(running.xdgTagPattern);
  running.matchContentType = ContentType::Game;
  running.matchFocused = false;
  running.opacity = 0.5;
  config.windowRules.push_back(std::move(running));

  const auto matchingLauncher =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-launcher", ContentType::Game, false);
  CHECK(matchingLauncher.opacity && *matchingLauncher.opacity == 0.9);
  CHECK(matchingLauncher.defaultFloating && *matchingLauncher.defaultFloating);

  const auto matchingRunning =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Game, false);
  CHECK(matchingRunning.opacity && *matchingRunning.opacity == 0.5);

  const auto matchingSecondTag =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-settings", ContentType::Game, false);
  CHECK(matchingSecondTag.opacity && *matchingSecondTag.opacity == 0.5);

  const auto wrongApp =
      umbriel::resolveWindowRules(config, "launcher", "now playing", "game-running", ContentType::Game, false);
  CHECK(wrongApp.opacity && *wrongApp.opacity == 0.25);
  const auto wrongTitle =
      umbriel::resolveWindowRules(config, "runner", "paused", "game-running", ContentType::Game, false);
  CHECK(wrongTitle.opacity && *wrongTitle.opacity == 0.25);
  const auto wrongContent =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Video, false);
  CHECK(wrongContent.opacity && *wrongContent.opacity == 0.25);
  const auto wrongFocus =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Game, true);
  CHECK(wrongFocus.opacity && *wrongFocus.opacity == 0.25);

  const auto missingTag = umbriel::resolveWindowRules(config, "runner", "now playing", "", ContentType::Game, false);
  CHECK(!missingTag.opacity);
  CHECK(!missingTag.defaultFloating);
  const auto unknownTag =
      umbriel::resolveWindowRules(config, "runner", "now playing", "browser", ContentType::Game, false);
  CHECK(!unknownTag.opacity);
  CHECK(!unknownTag.defaultFloating);
}

UMBRIEL_TEST(windowVrrRuleOverridesTheOutputPolicy) {
  CHECK(umbriel::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Always, false));
  CHECK(!umbriel::effectiveVrrEnabled(VrrMode::Always, false, VrrMode::Disabled, false));
  CHECK(!umbriel::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Fullscreen, false));
  CHECK(umbriel::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Fullscreen, true));
  CHECK(umbriel::effectiveVrrEnabled(VrrMode::Fullscreen, true, std::nullopt, false));
}

UMBRIEL_TEST(tearingRequiresTheOutputGateAndUsesTheWindowOverride) {
  CHECK(!umbriel::tearingEnabled(false, std::nullopt, true));
  CHECK(!umbriel::tearingEnabled(false, true, true));
  CHECK(umbriel::tearingEnabled(true, std::nullopt, true));
  CHECK(!umbriel::tearingEnabled(true, std::nullopt, false));
  CHECK(umbriel::tearingEnabled(true, true, false));
  CHECK(!umbriel::tearingEnabled(true, false, true));
}

UMBRIEL_TEST(layerRulesMergeMatchingFieldsInOrder) {
  Config config;

  LayerRule first;
  first.namespacePattern = "^panel$";
  first.namespaceRegex = std::regex(first.namespacePattern);
  first.blur = true;
  first.ignoreAlpha = 0.2;
  config.layerRules.push_back(std::move(first));

  LayerRule second;
  second.namespacePattern = "^panel$";
  second.namespaceRegex = std::regex(second.namespacePattern);
  second.ignoreAlpha = 0.75;
  second.optimized = true;
  config.layerRules.push_back(std::move(second));

  const auto resolved = umbriel::resolveLayerRules(config, "panel");
  CHECK(resolved.blur && *resolved.blur);
  CHECK(resolved.ignoreAlpha && *resolved.ignoreAlpha == 0.75);
  CHECK(resolved.optimized && *resolved.optimized);

  const auto unmatched = umbriel::resolveLayerRules(config, "wallpaper");
  CHECK(!unmatched.blur);
  CHECK(!unmatched.ignoreAlpha);
  CHECK(!unmatched.optimized);
}

int main() { return RUN_TESTS(); }
