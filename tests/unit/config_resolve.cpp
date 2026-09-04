#include "check.h"
#include "config/resolve.h"

#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using umbriel::Config;
using umbriel::ContentType;
using umbriel::LayerRule;
using umbriel::LayoutMode;
using umbriel::OutputIdentity;
using umbriel::OutputRule;
using umbriel::SecurityContextRule;
using umbriel::VrrMode;
using umbriel::WindowRule;
using umbriel::WorkspaceConfig;

namespace {
  constexpr OutputIdentity identity(
      std::string_view connector, std::string_view make = {}, std::string_view model = {}, std::string_view serial = {}
  ) {
    return {.connector = connector, .make = make, .model = model, .serial = serial};
  }
} // namespace

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
  config.layout.struts = {.left = 1, .right = 2, .top = 3, .bottom = 4};
  config.appearance.borderWidth = 2;
  config.layout.master.position = umbriel::MasterPosition::Right;
  config.layout.master.defaultWidthFraction = 0.58;
  config.layout.master.newOnTop = false;
  config.layout.dwindle.preserveSplit = true;

  WorkspaceConfig global;
  global.name = "dev";
  global.layout.gap = 12;
  global.layout.struts.left = 10;
  global.layout.struts.top = 30;
  global.layout.scrolling.defaultWidthFraction = 0.6;
  global.layout.master.defaultWidthFraction = 0.6;
  global.layout.master.newOnTop = true;
  global.layout.dwindle.preserveSplit = false;
  config.workspaceRules.push_back(std::move(global));

  WorkspaceConfig dpOne;
  dpOne.name = "dev";
  dpOne.output = "DP-1";
  dpOne.layout.gap = 20;
  dpOne.layout.struts.right = 20;
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

  const auto onDpOne = umbriel::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK(onDpOne.mode == LayoutMode::Dwindle);
  CHECK_EQ(onDpOne.gap, 20);
  CHECK_EQ(onDpOne.totalGap, 24);
  CHECK_EQ(onDpOne.edgePad, 22);
  CHECK_EQ(onDpOne.struts.left, 10);
  CHECK_EQ(onDpOne.struts.right, 20);
  CHECK_EQ(onDpOne.struts.top, 30);
  CHECK_EQ(onDpOne.struts.bottom, 4);
  CHECK(onDpOne.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*onDpOne.scrolling.defaultWidthFraction, 0.6);
  CHECK(onDpOne.master.position == umbriel::MasterPosition::Left);
  CHECK_EQ(onDpOne.master.defaultWidthFraction, 0.7);
  CHECK(!onDpOne.master.newOnTop);
  CHECK(onDpOne.dwindle.preserveSplit);

  const auto onDpTwo = umbriel::resolveWorkspaceLayout(config, identity("DP-2"), "dev", 0);
  CHECK(onDpTwo.mode == LayoutMode::Scrolling);
  CHECK_EQ(onDpTwo.gap, 30);
  CHECK_EQ(onDpTwo.struts.left, 10);
  CHECK_EQ(onDpTwo.struts.right, 2);
  CHECK_EQ(onDpTwo.struts.top, 30);
  CHECK_EQ(onDpTwo.struts.bottom, 4);
  CHECK(onDpTwo.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*onDpTwo.scrolling.defaultWidthFraction, 0.6);
  CHECK(onDpTwo.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(onDpTwo.master.defaultWidthFraction, 0.6);
  CHECK(onDpTwo.master.newOnTop);
  CHECK(!onDpTwo.dwindle.preserveSplit);

  const auto elsewhere = umbriel::resolveWorkspaceLayout(config, identity("HDMI-A-1"), "dev", 0);
  CHECK_EQ(elsewhere.gap, 12);
  CHECK_EQ(elsewhere.struts.left, 10);
  CHECK_EQ(elsewhere.struts.right, 2);
  CHECK_EQ(elsewhere.struts.top, 30);
  CHECK_EQ(elsewhere.struts.bottom, 4);
  CHECK(elsewhere.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*elsewhere.scrolling.defaultWidthFraction, 0.6);
  CHECK(elsewhere.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(elsewhere.master.defaultWidthFraction, 0.6);
  CHECK(elsewhere.master.newOnTop);
  CHECK(!elsewhere.dwindle.preserveSplit);
}

UMBRIEL_TEST(outputScrollingDefaultPrecedesWorkspaceRulesAndFollowsGlobalLayout) {
  Config config;
  config.layout.scrolling.defaultWidthFraction = 0.2;

  OutputRule output;
  output.name = "DP-1";
  output.layout.scrolling.defaultWidthFraction = 0.3;
  config.outputs.push_back(std::move(output));

  WorkspaceConfig globalWorkspace;
  globalWorkspace.name = "dev";
  globalWorkspace.layout.scrolling.defaultWidthFraction = 0.4;
  config.workspaceRules.push_back(std::move(globalWorkspace));

  WorkspaceConfig outputWorkspace;
  outputWorkspace.name = "dev";
  outputWorkspace.output = "DP-1";
  outputWorkspace.layout.scrolling.defaultWidthFraction = 0.5;
  config.workspaceRules.push_back(std::move(outputWorkspace));

  const auto outputSpecific = umbriel::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK_EQ(*outputSpecific.scrolling.defaultWidthFraction, 0.5);

  const auto outputDefault = umbriel::resolveWorkspaceLayout(config, identity("DP-1"), "chat", 1);
  CHECK_EQ(*outputDefault.scrolling.defaultWidthFraction, 0.3);

  const auto workspaceDefault = umbriel::resolveWorkspaceLayout(config, identity("DP-2"), "dev", 0);
  CHECK_EQ(*workspaceDefault.scrolling.defaultWidthFraction, 0.4);

  const auto globalDefault = umbriel::resolveWorkspaceLayout(config, identity("DP-2"), "chat", 1);
  CHECK_EQ(*globalDefault.scrolling.defaultWidthFraction, 0.2);
}

UMBRIEL_TEST(outputSpecificWorkspaceRulesBeatLaterGlobalRules) {
  Config config;

  WorkspaceConfig specific;
  specific.name = "dev";
  specific.output = "microstep msi g2712f cd6t084401192";
  specific.layout.gap = 20;
  config.workspaceRules.push_back(std::move(specific));

  WorkspaceConfig global;
  global.name = "dev";
  global.layout.gap = 12;
  config.workspaceRules.push_back(std::move(global));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const auto onMonitor = umbriel::resolveWorkspaceLayout(config, monitor, "dev", 0);
  CHECK_EQ(onMonitor.gap, 20);

  const auto elsewhere = umbriel::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK_EQ(elsewhere.gap, 12);
}

UMBRIEL_TEST(omittedScrollingDefaultWidthRemainsUnset) {
  Config config;

  const auto global = umbriel::resolveGlobalLayout(config);
  CHECK(!global.scrolling.defaultWidthFraction.has_value());

  const auto workspace = umbriel::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
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

  const auto staticSet = umbriel::resolveWorkspacesForOutput(config, identity("DP-1"));
  CHECK(!staticSet.dynamic);
  CHECK_EQ(staticSet.workspaces.size(), size_t{2});
  CHECK_EQ(staticSet.workspaces[0].name, std::string{"main"});
  CHECK_EQ(staticSet.workspaces[1].name, std::string{"web"});
  CHECK_EQ(staticSet.workspaces[1].layout.gap, 24);

  const auto dynamicSet = umbriel::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(dynamicSet.dynamic);
  CHECK_EQ(dynamicSet.workspaces.size(), size_t{1});
  CHECK_EQ(dynamicSet.workspaces[0].name, std::string{"1"});

  config.workspaces.emptyAbove = true;
  const auto dynamicSetWithEmptyAbove = umbriel::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(dynamicSetWithEmptyAbove.dynamic);
  CHECK_EQ(dynamicSetWithEmptyAbove.workspaces.size(), size_t{2});
  if (dynamicSetWithEmptyAbove.workspaces.size() == 2) {
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[0].name, std::string{"1"});
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[1].name, std::string{"2"});
  }
}

UMBRIEL_TEST(dynamicWorkspaceMinimumIsPerOutput) {
  Config config;
  OutputRule padded;
  padded.name = "DP-1";
  padded.minWorkspaces = 3;
  config.outputs.push_back(std::move(padded));

  CHECK_EQ(umbriel::resolveDynamicWorkspaceMinimum(config, identity("DP-1")), size_t{3});
  CHECK_EQ(umbriel::resolveDynamicWorkspaceMinimum(config, identity("DP-2")), size_t{1});

  const auto paddedSet = umbriel::resolveWorkspacesForOutput(config, identity("DP-1"));
  CHECK(paddedSet.dynamic);
  CHECK_EQ(paddedSet.workspaces.size(), size_t{3});
  if (paddedSet.workspaces.size() == 3) {
    CHECK_EQ(paddedSet.workspaces[0].name, std::string{"1"});
    CHECK_EQ(paddedSet.workspaces[2].name, std::string{"3"});
  }

  const auto bareSet = umbriel::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(bareSet.dynamic);
  CHECK_EQ(bareSet.workspaces.size(), size_t{1});

  // The leading empty is an extra entry only where it exceeds the minimum.
  config.workspaces.emptyAbove = true;
  CHECK_EQ(umbriel::resolveWorkspacesForOutput(config, identity("DP-1")).workspaces.size(), size_t{3});
  CHECK_EQ(umbriel::resolveWorkspacesForOutput(config, identity("DP-2")).workspaces.size(), size_t{2});
}
UMBRIEL_TEST(workspaceRulesMatchConnectorAndDescriptorWithoutOutputSection) {
  Config config;

  WorkspaceConfig connector;
  connector.index = 1;
  connector.output = "hdmi-a-1";
  connector.layout.gap = 18;
  config.workspaceRules.push_back(std::move(connector));

  WorkspaceConfig descriptor;
  descriptor.index = 1;
  descriptor.output = "microstep msi g2712f cd6t084401192";
  descriptor.layout.mode = LayoutMode::Dwindle;
  config.workspaceRules.push_back(std::move(descriptor));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const auto resolved = umbriel::resolveWorkspaceLayout(config, monitor, "1", 0);
  CHECK_EQ(resolved.gap, 18);
  CHECK(resolved.mode == LayoutMode::Dwindle);
}

UMBRIEL_TEST(descriptorOutputRuleOverridesConnectorFallback) {
  Config config;
  config.layout.scrolling.defaultWidthFraction = 0.5;

  OutputRule connector;
  connector.name = "HDMI-A-1";
  connector.workspaces = std::vector<std::string>{"fallback"};
  connector.layout.scrolling.defaultWidthFraction = 0.25;
  config.outputs.push_back(std::move(connector));

  OutputRule descriptor;
  descriptor.name = "Microstep MSI G2712F CD6T084401192";
  descriptor.workspaces = std::vector<std::string>{"specific"};
  descriptor.layout.scrolling.defaultWidthFraction = 0.75;
  config.outputs.push_back(std::move(descriptor));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const OutputRule* selected = umbriel::findOutputRule(config, monitor);
  CHECK(selected != nullptr);
  if (selected != nullptr) {
    CHECK_EQ(selected->name, std::string{"Microstep MSI G2712F CD6T084401192"});
    CHECK_EQ(*selected->layout.scrolling.defaultWidthFraction, 0.75);
  }

  const auto resolved = umbriel::resolveWorkspacesForOutput(config, monitor);
  CHECK(!resolved.dynamic);
  CHECK_EQ(resolved.workspaces.size(), size_t{1});
  if (resolved.workspaces.size() == 1) {
    CHECK_EQ(resolved.workspaces[0].name, std::string{"specific"});
    CHECK_EQ(*resolved.workspaces[0].layout.scrolling.defaultWidthFraction, 0.75);
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
  app.defaultScrollingColumn = "browser-stack";
  app.defaultScrollingColumnOrder = 20;
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
  title.defaultScrollingColumn = "terminals";
  title.defaultScrollingColumnOrder = 10;
  config.windowRules.push_back(std::move(title));

  WindowRule unfocused;
  unfocused.matchFocused = false;
  unfocused.defaultFloating = true;
  config.windowRules.push_back(std::move(unfocused));

  const auto resolved =
      umbriel::resolveWindowRules(config, "foot", "project shell", std::nullopt, ContentType::None, false, 0);
  CHECK(resolved.opacity && *resolved.opacity == 0.8);
  CHECK(resolved.blur && *resolved.blur);
  CHECK(resolved.defaultFloating && *resolved.defaultFloating);
  CHECK(resolved.defaultPosition.has_value());
  CHECK_EQ(resolved.defaultPosition->x, 12);
  CHECK_EQ(resolved.defaultPosition->y, 24);
  CHECK(resolved.defaultPosition->anchor == umbriel::WindowPositionAnchor::TopRight);
  CHECK(resolved.defaultFocused && !*resolved.defaultFocused);
  CHECK(resolved.defaultPinned && !*resolved.defaultPinned);
  CHECK(resolved.defaultScrollingColumn && *resolved.defaultScrollingColumn == "terminals");
  CHECK(resolved.defaultScrollingColumnOrder && *resolved.defaultScrollingColumnOrder == 10);
  CHECK(resolved.focusOnActivate && *resolved.focusOnActivate);
  CHECK(resolved.vrr == VrrMode::Always);
  CHECK(resolved.allowTearing && *resolved.allowTearing);
  CHECK(resolved.hdr == umbriel::HdrMode::On);

  const auto appOnly = umbriel::resolveWindowRules(config, "foot", "editor", std::nullopt, ContentType::None, false, 0);
  CHECK(appOnly.defaultPinned && *appOnly.defaultPinned);
  CHECK(appOnly.defaultScrollingColumn && *appOnly.defaultScrollingColumn == "browser-stack");
  CHECK(appOnly.defaultScrollingColumnOrder && *appOnly.defaultScrollingColumnOrder == 20);
  CHECK(appOnly.vrr == VrrMode::Disabled);
  CHECK(appOnly.allowTearing && !*appOnly.allowTearing);
  CHECK(appOnly.hdr == umbriel::HdrMode::Off);

  const auto focused =
      umbriel::resolveWindowRules(config, "foot", "project shell", std::nullopt, ContentType::None, true, 0);
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

  const auto resolved =
      umbriel::resolveWindowRules(config, "utility", std::nullopt, std::nullopt, ContentType::None, false, 0);
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
  game.matchAtStartup = true;
  game.opacity = 0.75;
  config.windowRules.push_back(std::move(game));

  WindowRule afterStartup;
  afterStartup.matchContentType = ContentType::Game;
  afterStartup.matchAtStartup = false;
  afterStartup.defaultFloating = true;
  config.windowRules.push_back(std::move(afterStartup));

  WindowRule none;
  none.matchContentType = ContentType::None;
  none.defaultFloating = true;
  config.windowRules.push_back(std::move(none));

  const auto matchingGame =
      umbriel::resolveWindowRules(config, "runner", "now playing", std::nullopt, ContentType::Game, false, 0);
  CHECK(matchingGame.opacity && *matchingGame.opacity == 0.75);
  CHECK(!matchingGame.defaultFloating);

  const auto wrongApp =
      umbriel::resolveWindowRules(config, "launcher", "now playing", std::nullopt, ContentType::Game, false, 0);
  CHECK(!wrongApp.opacity);
  const auto wrongTitle =
      umbriel::resolveWindowRules(config, "runner", "paused", std::nullopt, ContentType::Game, false, 0);
  CHECK(!wrongTitle.opacity);
  const auto wrongFocus =
      umbriel::resolveWindowRules(config, "runner", "now playing", std::nullopt, ContentType::Game, true, 0);
  CHECK(!wrongFocus.opacity);
  const auto afterStartupRule = umbriel::resolveWindowRules(
      config, "runner", "now playing", std::nullopt, ContentType::Game, false, umbriel::kStartupWindowRuleDurationMs
  );
  CHECK(!afterStartupRule.opacity);
  CHECK(afterStartupRule.defaultFloating && *afterStartupRule.defaultFloating);

  const auto matchingPhoto =
      umbriel::resolveWindowRules(config, "viewer", "photo", std::nullopt, ContentType::Photo, false, 0);
  CHECK(matchingPhoto.opacity && *matchingPhoto.opacity == 0.25);

  const auto matchingNone =
      umbriel::resolveWindowRules(config, "terminal", "shell", std::nullopt, ContentType::None, false, 0);
  CHECK(matchingNone.defaultFloating && *matchingNone.defaultFloating);

  const auto video = umbriel::resolveWindowRules(config, "viewer", "video", std::nullopt, ContentType::Video, false, 0);
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
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-launcher", ContentType::Game, false, 0);
  CHECK(matchingLauncher.opacity && *matchingLauncher.opacity == 0.9);
  CHECK(matchingLauncher.defaultFloating && *matchingLauncher.defaultFloating);

  const auto matchingRunning =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Game, false, 0);
  CHECK(matchingRunning.opacity && *matchingRunning.opacity == 0.5);

  const auto matchingSecondTag =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-settings", ContentType::Game, false, 0);
  CHECK(matchingSecondTag.opacity && *matchingSecondTag.opacity == 0.5);

  const auto wrongApp =
      umbriel::resolveWindowRules(config, "launcher", "now playing", "game-running", ContentType::Game, false, 0);
  CHECK(wrongApp.opacity && *wrongApp.opacity == 0.25);
  const auto wrongTitle =
      umbriel::resolveWindowRules(config, "runner", "paused", "game-running", ContentType::Game, false, 0);
  CHECK(wrongTitle.opacity && *wrongTitle.opacity == 0.25);
  const auto wrongContent =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Video, false, 0);
  CHECK(wrongContent.opacity && *wrongContent.opacity == 0.25);
  const auto wrongFocus =
      umbriel::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Game, true, 0);
  CHECK(wrongFocus.opacity && *wrongFocus.opacity == 0.25);

  const auto missingTag =
      umbriel::resolveWindowRules(config, "runner", "now playing", std::nullopt, ContentType::Game, false, 0);
  CHECK(!missingTag.opacity);
  CHECK(!missingTag.defaultFloating);
  const auto unknownTag =
      umbriel::resolveWindowRules(config, "runner", "now playing", "browser", ContentType::Game, false, 0);
  CHECK(!unknownTag.opacity);
  CHECK(!unknownTag.defaultFloating);
}

UMBRIEL_TEST(windowRulesMatchEmptyIdentityOnlyWhenTheClientSetIt) {
  Config config;

  WindowRule blankTitle;
  blankTitle.titlePattern = "^$";
  blankTitle.titleRegex = std::regex(blankTitle.titlePattern);
  blankTitle.defaultFloating = true;
  config.windowRules.push_back(std::move(blankTitle));

  WindowRule blankAppId;
  blankAppId.appIdPattern = "^$";
  blankAppId.appIdRegex = std::regex(blankAppId.appIdPattern);
  blankAppId.opacity = 0.5;
  config.windowRules.push_back(std::move(blankAppId));

  WindowRule blankTag;
  blankTag.xdgTagPattern = "^$";
  blankTag.xdgTagRegex = std::regex(blankTag.xdgTagPattern);
  blankTag.defaultMaximize = true;
  config.windowRules.push_back(std::move(blankTag));

  // A window whose title, app ID, and tag are all set to the empty string.
  const auto allEmpty = umbriel::resolveWindowRules(config, "", "", "", ContentType::None, false, 0);
  CHECK(allEmpty.defaultFloating && *allEmpty.defaultFloating);
  CHECK(allEmpty.opacity && *allEmpty.opacity == 0.5);
  CHECK(allEmpty.defaultMaximize && *allEmpty.defaultMaximize);

  // A window that never sent any of them matches nothing: an unset value is not an empty one.
  const auto allUnset =
      umbriel::resolveWindowRules(config, std::nullopt, std::nullopt, std::nullopt, ContentType::None, false, 0);
  CHECK(!allUnset.defaultFloating);
  CHECK(!allUnset.opacity);
  CHECK(!allUnset.defaultMaximize);

  // Selectors are independent: an empty title matches while a non-empty app ID and an unset tag do not.
  const auto emptyTitleOnly =
      umbriel::resolveWindowRules(config, "firefox", "", std::nullopt, ContentType::None, false, 0);
  CHECK(emptyTitleOnly.defaultFloating && *emptyTitleOnly.defaultFloating);
  CHECK(!emptyTitleOnly.opacity);
  CHECK(!emptyTitleOnly.defaultMaximize);
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

  LayerRule blank;
  blank.namespacePattern = "^$";
  blank.namespaceRegex = std::regex(blank.namespacePattern);
  blank.blur = true;
  config.layerRules.push_back(std::move(blank));

  // Layer surfaces name themselves at creation, so an empty namespace is a value a rule can match.
  const auto blankNamespace = umbriel::resolveLayerRules(config, "");
  CHECK(blankNamespace.blur && *blankNamespace.blur);
  CHECK(!umbriel::resolveLayerRules(config, std::nullopt).blur);
}

UMBRIEL_TEST(securityContextRulesGrantGlobalsByMetadata) {
  Config config;

  SecurityContextRule scoped;
  scoped.sandboxEnginePattern = "^org\\.flatpak$";
  scoped.sandboxEngineRegex = std::regex(scoped.sandboxEnginePattern);
  scoped.appIdPattern = "^org\\.example\\.Bar$";
  scoped.appIdRegex = std::regex(scoped.appIdPattern);
  scoped.allowGlobals = {"zwlr_layer_shell_v1"};
  config.securityContextRules.push_back(std::move(scoped));

  SecurityContextRule wildcard;
  wildcard.allowGlobals = {"ext_idle_notifier_v1"};
  config.securityContextRules.push_back(std::move(wildcard));

  SecurityContextRule partial;
  partial.appIdPattern = "example";
  partial.appIdRegex = std::regex(partial.appIdPattern);
  partial.allowGlobals = {"zwlr_screencopy_manager_v1"};
  config.securityContextRules.push_back(std::move(partial));

  const auto grants = [&](const char* engine, const char* appId, std::string_view global) {
    const std::vector<std::string> globals = umbriel::securityContextRuleGlobals(config, engine, appId);
    return std::ranges::find(globals, global) != globals.end();
  };

  CHECK(grants("org.flatpak", "org.example.Bar", "zwlr_layer_shell_v1"));
  // Every specified match key must hold.
  CHECK(!grants("org.flatpak", "org.example.Other", "zwlr_layer_shell_v1"));
  CHECK(!grants("waypak", "org.example.Bar", "zwlr_layer_shell_v1"));
  // Absent metadata never satisfies a pattern.
  CHECK(!grants(nullptr, "org.example.Bar", "zwlr_layer_shell_v1"));
  // A rule without match keys applies to every client, but only for its own globals.
  CHECK(grants(nullptr, nullptr, "ext_idle_notifier_v1"));
  CHECK_EQ(umbriel::securityContextRuleGlobals(config, nullptr, nullptr).size(), size_t{1});
  // The whole value must match; a substring is not enough.
  CHECK(!grants("org.flatpak", "org.example.Bar", "zwlr_screencopy_manager_v1"));
  CHECK(grants(nullptr, "example", "zwlr_screencopy_manager_v1"));
  // Matching rules pool their globals.
  CHECK_EQ(umbriel::securityContextRuleGlobals(config, "org.flatpak", "org.example.Bar").size(), size_t{2});
}

int main() { return RUN_TESTS(); }
