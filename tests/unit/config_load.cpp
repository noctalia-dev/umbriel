#include "check.h"
#include "config/store.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

using umbriel::ConfigDiagnostic;
using umbriel::ConfigStore;
using umbriel::HdrMode;
using umbriel::LayoutMode;
using umbriel::ModifierKey;
using umbriel::VrrMode;

namespace {
  bool containsDiagnostic(const ConfigStore& store, const std::string& text) {
    for (const ConfigDiagnostic& diagnostic : store.diagnostics()) {
      if (diagnostic.message.contains(text)) {
        return true;
      }
    }
    return false;
  }

  class TempConfig {
  public:
    TempConfig()
        : m_path(
              std::filesystem::temp_directory_path() / ("umbriel-config-load-" + std::to_string(getpid()) + ".toml")
          ),
          m_includePath(m_path.string() + ".include") {
      std::filesystem::remove(m_includePath);
    }
    ~TempConfig() {
      std::filesystem::remove(m_path);
      std::filesystem::remove(m_includePath);
    }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;

    void write(const std::string& contents) const {
      std::ofstream stream(m_path);
      stream << contents;
    }

    void writeInclude(const std::string& contents) const {
      std::ofstream stream(m_includePath);
      stream << contents;
    }

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] std::string includeName() const { return m_includePath.filename().string(); }

  private:
    std::filesystem::path m_path;
    std::filesystem::path m_includePath;
  };

  class TempConfigTree {
  public:
    TempConfigTree()
        : m_path(std::filesystem::temp_directory_path() / ("umbriel-config-tree-" + std::to_string(getpid()))) {
      std::filesystem::remove_all(m_path);
      std::filesystem::create_directories(m_path);
    }
    ~TempConfigTree() { std::filesystem::remove_all(m_path); }

    void write(const std::filesystem::path& relativePath, const std::string& contents) const {
      const std::filesystem::path path = m_path / relativePath;
      std::filesystem::create_directories(path.parent_path());
      std::ofstream stream(path);
      stream << contents;
    }

    [[nodiscard]] std::filesystem::path path(const std::filesystem::path& relativePath) const {
      return m_path / relativePath;
    }

  private:
    std::filesystem::path m_path;
  };

  class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, const std::string& value) : m_name(name) {
      if (const char* previous = std::getenv(name)) {
        m_previous = previous;
      }
      setenv(name, value.c_str(), 1);
    }
    ~ScopedEnvironment() {
      if (m_previous) {
        setenv(m_name.c_str(), m_previous->c_str(), 1);
      } else {
        unsetenv(m_name.c_str());
      }
    }

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
  };
} // namespace

UMBRIEL_TEST(defaultConfigLookupPrefersUserThenSystem) {
  const TempConfigTree tree;
  const std::filesystem::path userHome = tree.path("user");
  const std::filesystem::path systemDir = tree.path("system");
  const std::filesystem::path systemConfig = systemDir / "umbriel/config.toml";
  const std::filesystem::path userConfig = userHome / "umbriel/config.toml";
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", userHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());

  ConfigStore& store = umbriel::configStore();
  store.load(nullptr);

  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);
  CHECK(!store.fileMissing());

  tree.write("user/umbriel/config.toml", "[layout]\ngap = 19\n");
  store.load(nullptr);

  CHECK_EQ(store.rootPath(), userConfig);
  CHECK_EQ(store.config().layout.gap, 19);
  CHECK(!store.fileMissing());
}

UMBRIEL_TEST(sharedLayoutAndNumberReadersPreserveConfigBehavior) {
  const TempConfig file;
  file.write(R"(
unknown_root_key = true
[general]
prefer_no_csd = false

[appearance]
prefer_no_csd = true


[layout]
mode = "dwindle"
width_presets = [0.05, 0.5, 2.0]

[layout.scrolling]
center_underfull_strip = false
always_center_single_column = true
[layout.dwindle]
preserve_split = true

[output.DP-1]
workspaces = ["dev"]
scale = 9.0

[[workspace]]
name = "dev"

[workspace.layout]
mode = "scrolling"
width_presets = [0.25, 0.75]

[workspace.layout.scrolling]
center_underfull_strip = true
[workspace.layout.dwindle]
preserve_split = false
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().layout.mode == LayoutMode::Dwindle);
  CHECK_EQ(store.config().layout.widthPresets.size(), size_t{3});
  CHECK_EQ(store.config().layout.widthPresets[0], 0.1);
  CHECK_EQ(store.config().layout.widthPresets[1], 0.5);
  CHECK_EQ(store.config().layout.widthPresets[2], 1.0);
  CHECK(!store.config().layout.scrolling.centerUnderfullStrip);
  CHECK(store.config().layout.dwindle.preserveSplit);
  CHECK(store.config().appearance.preferNoCsd);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].scale.has_value());
  CHECK_EQ(*store.config().outputs[0].scale, 4.0);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.mode == LayoutMode::Scrolling);
  CHECK(store.config().workspaceRules[0].layout.widthPresets.has_value());
  CHECK_EQ(store.config().workspaceRules[0].layout.widthPresets->size(), size_t{2});
  CHECK(store.config().workspaceRules[0].layout.scrolling.centerUnderfullStrip == true);
  CHECK(store.config().workspaceRules[0].layout.dwindle.preserveSplit == false);
  CHECK(containsDiagnostic(store, "unknown key unknown_root_key"));
  CHECK(containsDiagnostic(store, "output.DP-1.scale = 9"));
  CHECK(containsDiagnostic(store, "unknown key layout.scrolling.always_center_single_column"));
  CHECK(containsDiagnostic(store, "unknown key general.prefer_no_csd"));
}

UMBRIEL_TEST(dwindlePreserveSplitDefaultsToFalse) {
  const TempConfig file;
  file.write("[layout]\n");
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK(!store.config().layout.dwindle.preserveSplit);
}

UMBRIEL_TEST(masterLayoutReadersLoadGlobalAndWorkspaceSettings) {
  const TempConfig file;
  file.write(R"(
[layout]
mode = "master"

[layout.master]
position = "right"
default_width_fraction = 0.05
surprise = true

[output.DP-1]
workspaces = ["dev"]

[[workspace]]
name = "dev"

[workspace.layout.master]
position = "left"
default_width_fraction = 0.7
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().layout.mode == LayoutMode::Master);
  CHECK(store.config().layout.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(store.config().layout.master.defaultWidthFraction, 0.1);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.master.position == umbriel::MasterPosition::Left);
  CHECK(store.config().workspaceRules[0].layout.master.defaultWidthFraction.has_value());
  CHECK_EQ(*store.config().workspaceRules[0].layout.master.defaultWidthFraction, 0.7);
  CHECK(containsDiagnostic(store, "layout.master.default_width_fraction = 0.05 out of range, clamped to 0.1"));
  CHECK(containsDiagnostic(store, "unknown key layout.master.surprise"));
}

UMBRIEL_TEST(scrollingDefaultWidthIsOptional) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[layout.scrolling]\ncenter_underfull_strip = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.defaultWidthFraction.has_value());

  file.write("[layout.scrolling]\ndefault_width_fraction = 0.75\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.scrolling.defaultWidthFraction.has_value());
  CHECK_EQ(*store.config().layout.scrolling.defaultWidthFraction, 0.75);

  file.write("[layout.scrolling]\ncenter_underfull_strip = true\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.defaultWidthFraction.has_value());
}

UMBRIEL_TEST(expandSingleColumnParsesAndDefaultsToFalse) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("");

  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.expandSingleColumn);

  file.write("[layout.scrolling]\nexpand_single_column = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.scrolling.expandSingleColumn);

  file.write("[layout.scrolling]\nexpand_single_column = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.expandSingleColumn);
}

UMBRIEL_TEST(modKeyIsUserConfigurable) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[general]\nmod_key = \"Ctrl\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().general.modKey == ModifierKey::Control);

  file.write("[general]\nmod_key = \"win\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().general.modKey == ModifierKey::Super);

  file.write("[general]\nmod_key = \"Meta\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().general.modKey.has_value());
  CHECK(containsDiagnostic(store, "unknown general.mod_key"));
}

UMBRIEL_TEST(keybindTableLoadsAllowWhenLocked) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[keybinds]\n"
      "\"XF86AudioRaiseVolume\" = { action = \"spawn:volume-up\", allow_when_locked = true }\n"
      "\"XF86AudioLowerVolume\" = \"spawn:volume-down\"\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().keybinds.size(), size_t{2});

  bool allowedWhenLocked = false;
  bool defaultsToBlocked = false;
  for (const auto& bind : store.config().keybinds) {
    allowedWhenLocked = allowedWhenLocked || bind.allowWhenLocked;
    defaultsToBlocked = defaultsToBlocked || !bind.allowWhenLocked;
  }
  CHECK(allowedWhenLocked);
  CHECK(defaultsToBlocked);
  CHECK(!containsDiagnostic(store, "allow_when_locked"));
}

UMBRIEL_TEST(keybindTableLoadsPostActionSubmaps) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[keybinds]\n"
      "\"submap[outer],1\" = { action = \"workspace-switch:2\", submap = \"reset\" }\n"
      "\"submap[outer],2\" = { action = \"workspace-switch:3\", submap = \"inner\", repeat = true }\n"
      "\"submap[outer],3\" = { action = \"workspace-switch:4\", repeat = true }\n"
  );
  CHECK(store.reload().success);

  bool resets = false;
  bool entersInner = false;
  bool remainsPersistent = false;
  for (const auto& bind : store.config().keybinds) {
    if (!bind.submapAfter.has_value()) {
      remainsPersistent = remainsPersistent || (bind.submap == "outer" && bind.repeat);
      continue;
    }
    CHECK(!bind.repeat);
    resets = resets || umbriel::isSubmapReset(*bind.submapAfter);
    entersInner = entersInner || bind.submapAfter->name == "inner";
  }
  CHECK(resets);
  CHECK(entersInner);
  CHECK(remainsPersistent);
  CHECK(!containsDiagnostic(store, "submap"));

  file.write(
      "[keybinds]\n"
      "\"submap[outer],1\" = { action = \"workspace-switch:2\", submap = \"\" }\n"
      "\"submap[outer],2\" = { action = \"workspace-switch:3\", submap = \"disable\" }\n"
      "\"submap[outer],3\" = { action = \"workspace-switch:4\", submap = \"invalid]name\" }\n"
  );
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "submap must be a non-empty name"));
  CHECK(std::ranges::none_of(store.config().keybinds, [](const auto& bind) { return bind.submap == "outer"; }));
}

UMBRIEL_TEST(hotCornersLoadActionsAndValidate) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[hot_corners.top_left]\nenabled = true\ndelay_ms = 750\naction = \"overview-open\"\n"
      "[hot_corners.bottom_right]\nenabled = true\ndelay_ms = 125\naction = \"spawn:notify-send corner\"\n"
  );
  CHECK(store.reload().success);
  CHECK(store.config().hotCorners.corners[0].enabled);
  CHECK_EQ(store.config().hotCorners.corners[0].delayMs, 750);
  CHECK(store.config().hotCorners.corners[0].action.has_value());
  CHECK(
      store.config().hotCorners.corners[0].action
      && store.config().hotCorners.corners[0].action->action == umbriel::KeybindAction::OverviewOpen
  );
  CHECK(!store.config().hotCorners.corners[1].enabled);
  CHECK(!store.config().hotCorners.corners[2].enabled);
  CHECK(store.config().hotCorners.corners[3].enabled);
  CHECK_EQ(store.config().hotCorners.corners[3].delayMs, 125);
  CHECK(store.config().hotCorners.corners[3].action.has_value());
  CHECK(
      store.config().hotCorners.corners[3].action
      && store.config().hotCorners.corners[3].action->action == umbriel::KeybindAction::Spawn
  );

  file.write("[hot_corners.top_right]\nenabled = true\ndelay_ms = -1\naction = \"not-an-action\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().hotCorners.corners[1].delayMs, 0);
  CHECK(!store.config().hotCorners.corners[1].action.has_value());
  CHECK(containsDiagnostic(store, "invalid hot_corners.top_right.action \"not-an-action\""));
  CHECK(containsDiagnostic(store, "hot_corners.top_right.delay_ms = -1"));
}

UMBRIEL_TEST(overviewBackgroundBlurLoads) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nbackground_blur = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.backgroundBlur);
}

UMBRIEL_TEST(overviewShortcutConfigurationLoads) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nshortcuts = false\nshortcut_keys = \"asdf\"\nbadge_color = \"#12345678\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.shortcuts);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"asdf"});
  CHECK(store.config().overview.badgeColor.has_value());
  const std::array<float, 4> badgeColor = store.config().overview.badgeColor.value_or(std::array<float, 4>{});
  CHECK_EQ(badgeColor[0], 18.0F / 255.0F);
  CHECK_EQ(badgeColor[1], 52.0F / 255.0F);
  CHECK_EQ(badgeColor[2], 86.0F / 255.0F);
  CHECK_EQ(badgeColor[3], 120.0F / 255.0F);
}

UMBRIEL_TEST(overviewShortcutKeysRejectInvalidValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nshortcut_keys = \"a\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"1234567890"});
  CHECK(containsDiagnostic(store, "expected at least 2 characters"));

  file.write("[overview]\nshortcut_keys = \"aA\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"1234567890"});
  CHECK(containsDiagnostic(store, "duplicate key"));

  file.write("[overview]\nshortcut_keys = \"a b\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"1234567890"});
  CHECK(containsDiagnostic(store, "invalid character 0x20"));

  file.write("[overview]\nshortcut_keys = 12\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"1234567890"});
  CHECK(containsDiagnostic(store, "expected string"));
}

UMBRIEL_TEST(overviewBadgeColorRejectsInvalidValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nbadge_color = \"not-a-color\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.badgeColor.has_value());
  CHECK(containsDiagnostic(store, "overview.badge_color (invalid color"));
}

UMBRIEL_TEST(cornerRadiusClampsToItsRange) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[appearance]\ncorner_radius = 64\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().appearance.cornerRadius, 64);
  CHECK(!containsDiagnostic(store, "corner_radius"));

  file.write("[appearance]\ncorner_radius = 500\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().appearance.cornerRadius, 100);
  CHECK(containsDiagnostic(store, "appearance.corner_radius = 500 out of range, clamped to 100"));
}

UMBRIEL_TEST(middleClickPasteLoadsAndDefaultsEnabled) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[input]\nmiddle_click_paste = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().input.middleClickPaste);

  file.write("[input]\nmiddle_click_paste = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.middleClickPaste);

  file.write("[input]\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.middleClickPaste);
}

UMBRIEL_TEST(outputVrrPolicyLoadsAndDefaultsDisabled) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nvrr = \"fullscreen\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].vrr == VrrMode::Fullscreen);

  file.write("[output.DP-1]\nvrr = \"always\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].vrr == VrrMode::Always);

  file.write("[output.DP-1]\nvrr = \"disabled\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].vrr == VrrMode::Disabled);

  file.write("[output.DP-1]\nvrr = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].vrr == VrrMode::Disabled);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.vrr"));

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].vrr == VrrMode::Disabled);
}

UMBRIEL_TEST(outputTearingPermissionLoadsAndDefaultsDisabled) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\ntearing = true\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].allowTearing);

  file.write("[output.DP-1]\ntearing = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].allowTearing);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].allowTearing);

  file.write("[output.DP-1]\ntearing = \"yes\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].allowTearing);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.tearing (expected boolean)"));
}

UMBRIEL_TEST(outputDirectScanoutPolicyLoadsAndDefaultsEnabled) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\ndirect_scanout = false\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(!store.config().outputs[0].directScanout);

  file.write("[output.DP-1]\ndirect_scanout = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].directScanout);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].directScanout);

  file.write("[output.DP-1]\ndirect_scanout = \"no\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].directScanout);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.direct_scanout (expected boolean)"));
}

UMBRIEL_TEST(outputHdrPolicyAndSdrWhiteLoad) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nhdr = \"on\"\nsdr_white = 300\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].hdr == HdrMode::On);
  CHECK_EQ(store.config().outputs[0].sdrWhite, 300.0F);

  file.write("[output.DP-1]\nhdr = \"off\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].hdr == HdrMode::Off);
  CHECK_EQ(store.config().outputs[0].sdrWhite, 203.0F);

  file.write("[output.DP-1]\nhdr = \"auto\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].hdr == HdrMode::Auto);
  CHECK_EQ(store.config().outputs[0].sdrWhite, 203.0F);

  file.write("[output.DP-1]\nhdr = \"fullscreen\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].hdr == HdrMode::Fullscreen);
  CHECK_EQ(store.config().outputs[0].sdrWhite, 203.0F);

  file.write("[output.DP-1]\nhdr = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].hdr == HdrMode::Off);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.hdr"));
}

UMBRIEL_TEST(windowOutputPoliciesLoadAndRejectInvalidValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\nmatch.app_id = \"^game$\"\nvrr = \"always\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].vrr == VrrMode::Always);

  file.write("[[window_rule]]\nvrr = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].vrr);
  CHECK(containsDiagnostic(store, "ignoring window_rule.vrr"));

  file.write("[[window_rule]]\nmatch.app_id = \"^game$\"\nhdr = \"fullscreen\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].hdr == HdrMode::Fullscreen);

  file.write("[[window_rule]]\nhdr = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].hdr);
  CHECK(containsDiagnostic(store, "ignoring window_rule.hdr"));
}

UMBRIEL_TEST(windowTearingOverrideLoadsAsAnOptionalBoolean) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\nmatch.app_id = \"^game$\"\ntearing = true\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].allowTearing && *store.config().windowRules[0].allowTearing);

  file.write("[[window_rule]]\ntearing = false\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules[0].allowTearing && !*store.config().windowRules[0].allowTearing);

  file.write("[[window_rule]]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].allowTearing);

  file.write("[[window_rule]]\ntearing = \"yes\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].allowTearing);
  CHECK(containsDiagnostic(store, "ignoring window_rule.tearing (expected boolean)"));
}

UMBRIEL_TEST(outputEnabledFlagParsesAndDefaultsTrue) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nenabled = false\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(!store.config().outputs[0].enabled);

  file.write("[output.DP-1]\nenabled = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].enabled);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].enabled);

  file.write("[output.DP-1]\nenabled = \"yes\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].enabled);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.enabled"));
}

UMBRIEL_TEST(semanticColorsLoadFromTheirOwnSection) {
  const TempConfig file;
  file.write(R"(
[colors]
background = "#01020304"
text_primary = "#11121314"
text_muted = "#21222324"
accent_primary = "#31323334"
accent_secondary = "#41424344"
warning = "#51525354"
error = "#61626364"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& colors = store.config().colors;

  CHECK(result.success);
  CHECK_EQ(colors.background[0], 1.0F / 255.0F);
  CHECK_EQ(colors.background[3], 4.0F / 255.0F);
  CHECK_EQ(colors.textPrimary[0], 17.0F / 255.0F);
  CHECK_EQ(colors.textMuted[0], 33.0F / 255.0F);
  CHECK_EQ(colors.accentPrimary[0], 49.0F / 255.0F);
  CHECK_EQ(colors.accentSecondary[0], 65.0F / 255.0F);
  CHECK_EQ(colors.warning[0], 81.0F / 255.0F);
  CHECK_EQ(colors.error[0], 97.0F / 255.0F);
  CHECK(!containsDiagnostic(store, "unknown key colors"));
}

UMBRIEL_TEST(missingIncludesRemainPendingUntilTheyLoad) {
  const TempConfig file;
  file.write("[include]\nfiles = [\"" + file.includeName() + "\"]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult missing = store.reload();

  CHECK(missing.success);
  CHECK(store.missingIncludes());
  CHECK(containsDiagnostic(store, "include not found"));

  file.writeInclude("[colors]\naccent_primary = \"#123456FF\"\n");
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK(!store.missingIncludes());
  CHECK(!containsDiagnostic(store, "include not found"));
  CHECK_EQ(store.config().colors.accentPrimary[0], 18.0F / 255.0F);
}

UMBRIEL_TEST(mainFileOverridesIncludedFiles) {
  // Noctalia's rendered theme lands in an include file; the user's root config must win on conflicts while still
  // picking up keys the include alone provides. This is what lets users override generated theme colors.
  const TempConfig file;
  file.write(
      R"(
[colors]
accent_primary = "#ABCDEF00"
[include]
files = [")"
      + file.includeName()
      + R"("]
)"
  );
  file.writeInclude("[colors]\naccent_primary = \"#123456FF\"\nbackground = \"#222222FF\"\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK_EQ(store.config().colors.accentPrimary[0], 171.0F / 255.0F);
  CHECK_EQ(store.config().colors.background[0], 34.0F / 255.0F);
}

UMBRIEL_TEST(activationPolicyLoadsGloballyAndPerWindow) {
  const TempConfig file;
  file.write(R"(
[general]
focus_on_activate = true

[[window_rule]]
match.app_id = "^game$"
default_focused = false
default_pinned = true
focus_on_activate = false
default_position = { x = 32, y = 48, anchor = "bottom_left" }

[[window_rule]]
match.app_id = "^centered$"
default_position = { x = 0, y = 0 }
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().general.focusOnActivate);
  CHECK_EQ(store.config().windowRules.size(), size_t{2});
  CHECK(store.config().windowRules[0].defaultFocused.has_value());
  CHECK(!*store.config().windowRules[0].defaultFocused);
  CHECK(store.config().windowRules[0].defaultPinned.has_value());
  CHECK(*store.config().windowRules[0].defaultPinned);
  CHECK(store.config().windowRules[0].focusOnActivate.has_value());
  CHECK(!*store.config().windowRules[0].focusOnActivate);
  CHECK(store.config().windowRules[0].defaultPosition.has_value());
  CHECK_EQ(store.config().windowRules[0].defaultPosition->x, 32);
  CHECK_EQ(store.config().windowRules[0].defaultPosition->y, 48);
  CHECK(store.config().windowRules[0].defaultPosition->anchor == umbriel::WindowPositionAnchor::BottomLeft);
  CHECK(store.config().windowRules[1].defaultPosition.has_value());
  CHECK(store.config().windowRules[1].defaultPosition->anchor == umbriel::WindowPositionAnchor::Center);
}

UMBRIEL_TEST(restoredMaximizePolicyLoadsAndDefaultsOff) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[general]\nhonor_restored_maximize = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().general.honorRestoredMaximize);

  file.write("[general]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().general.honorRestoredMaximize);
}

UMBRIEL_TEST(deviceInputOverridesLoadAndMatchExactNames) {
  const TempConfig file;
  file.write(R"(
[input.keyboard]
layout = "us"
repeat_rate = 25

[input.touchpad]
tap = true
natural_scroll = true
accel_profile = "adaptive"
sensitivity = 0.1
scroll_factor = 1.5
disable_while_typing = true

[input.mouse]
accel_profile = "custom 0.2 0.0 0.5 1.0 2.0"
sensitivity = 0.25

[[input.device]]
name = "Acme Split Keyboard"
layout = ""
variant = ""
repeat_rate = 40
repeat_delay = 250

[[input.device]]
name = "Acme Precision Touchpad"
tap = false
natural_scroll = false
accel_profile = "flat"
sensitivity = -0.5
disable_while_typing = false

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = -0.5
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& input = store.config().input;

  CHECK(result.success);
  CHECK(input.mouse.accelProfile.has_value());
  CHECK(input.mouse.accelProfile->kind == umbriel::AccelProfile::Kind::Custom);
  CHECK_EQ(input.mouse.accelProfile->step, 0.2);
  CHECK_EQ(input.mouse.accelProfile->points, std::vector<double>({0.0, 0.5, 1.0, 2.0}));
  CHECK_EQ(input.mouse.sensitivity, 0.25);
  CHECK(input.touchpad.accelProfile.has_value());
  if (input.touchpad.accelProfile.has_value()) {
    CHECK(input.touchpad.accelProfile->kind == umbriel::AccelProfile::Kind::Adaptive);
  }
  CHECK(input.touchpad.sensitivity == std::optional<double>(0.1));
  CHECK(input.touchpad.scrollFactor == std::optional<double>(1.5));
  CHECK(input.touchpad.disableWhileTyping == std::optional<bool>(true));
  CHECK_EQ(input.devices.size(), size_t{3});

  const auto* keyboard = input.findDevice("Acme Split Keyboard");
  CHECK(keyboard != nullptr);
  if (keyboard != nullptr) {
    CHECK(keyboard->layout == std::optional<std::string>(""));
    CHECK(keyboard->variant == std::optional<std::string>(""));
    CHECK(keyboard->repeatRate == std::optional<int>(40));
    CHECK(keyboard->repeatDelay == std::optional<int>(250));
  }

  const auto* touchpad = input.findDevice("Acme Precision Touchpad");
  CHECK(touchpad != nullptr);
  if (touchpad != nullptr) {
    CHECK(touchpad->tap == std::optional<bool>(false));
    CHECK(touchpad->naturalScroll == std::optional<bool>(false));
    CHECK(touchpad->accelProfile.has_value());
    if (touchpad->accelProfile.has_value()) {
      CHECK(touchpad->accelProfile->kind == umbriel::AccelProfile::Kind::Flat);
    }
    CHECK(touchpad->sensitivity == std::optional<double>(-0.5));
    CHECK(touchpad->disableWhileTyping == std::optional<bool>(false));
  }

  const auto* mouse = input.findDevice("Acme Gaming Mouse");
  CHECK(mouse != nullptr);
  if (mouse != nullptr) {
    CHECK(mouse->accelProfile.has_value());
    CHECK(mouse->accelProfile->kind == umbriel::AccelProfile::Kind::Flat);
    CHECK(mouse->sensitivity == std::optional<double>(-0.5));
  }

  CHECK(input.findDevice("acme split keyboard") == nullptr);
  CHECK(input.findDevice("Acme") == nullptr);
}

UMBRIEL_TEST(mouseAccelerationPreservesDeviceProfileByDefault) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.mouse.accelProfile.has_value());
  CHECK_EQ(defaults.input.mouse.sensitivity, 0.0);
}

UMBRIEL_TEST(touchpadAccelerationDefaultsToUnset) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.touchpad.accelProfile.has_value());
  CHECK(!defaults.input.touchpad.sensitivity.has_value());
}

UMBRIEL_TEST(touchpadScrollFactorDefaultsToUnset) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.touchpad.scrollFactor.has_value());
}

UMBRIEL_TEST(touchpadDisableWhileTypingDefaultsToUnset) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.touchpad.disableWhileTyping.has_value());
}

UMBRIEL_TEST(touchpadTapDefaultsToEnabled) {
  const umbriel::Config defaults;
  CHECK(defaults.input.touchpad.tap == std::optional<bool>(true));
}

UMBRIEL_TEST(cursorFollowsFocusDefaultsToDisabled) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.cursor.followsFocus);
}

UMBRIEL_TEST(hardwareCursorCanBeDisabled) {
  const TempConfig file;
  file.write(R"(
[input.cursor]
hardware_cursor = false
follows_focus = true
hide_when_typing = true
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.cursor.hardwareCursor);
  CHECK(store.config().input.cursor.followsFocus);
  CHECK(store.config().input.cursor.hideWhenTyping);
  CHECK(!containsDiagnostic(store, "unknown key input.cursor.hardware_cursor"));
  CHECK(!containsDiagnostic(store, "unknown key input.cursor.follows_focus"));
  CHECK(!containsDiagnostic(store, "unknown key input.cursor.hide_when_typing"));
}

UMBRIEL_TEST(cursorHideTimeoutLoads) {
  const TempConfig file;
  file.write(R"(
[input.cursor]
hide_timeout_ms = 1500
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().input.cursor.hideTimeoutMs, 1500);
  CHECK(!containsDiagnostic(store, "unknown key input.cursor.hide_timeout_ms"));

  file.write("[input.cursor]\nhide_timeout = 15\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.cursor.hideTimeoutMs, 0);
  CHECK(containsDiagnostic(store, "unknown key input.cursor.hide_timeout"));
}

UMBRIEL_TEST(invalidCustomAccelerationCurveIsRejected) {
  const TempConfig file;
  file.write(R"(
[input.mouse]
accel_profile = "custom 0.2 1.0"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.mouse.accelProfile.has_value());
  CHECK(containsDiagnostic(store, "custom <step> <points...>"));
}

UMBRIEL_TEST(keyboardOptionsLoadGloballyAndPerDevice) {
  const TempConfig file;
  file.write(R"(
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"

[[input.device]]
name = "Acme Split Keyboard"
layout = "us,fr"
options = "grp:win_space_toggle"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& input = store.config().input;

  CHECK(result.success);
  CHECK(!containsDiagnostic(store, "unknown key input.keyboard.options"));
  CHECK(!containsDiagnostic(store, "invalid XKB configuration"));
  CHECK_EQ(input.keyboard.layout, std::string{"us,de"});
  CHECK_EQ(input.keyboard.options, std::string{"grp:alt_shift_toggle"});

  const auto* device = input.findDevice("Acme Split Keyboard");
  CHECK(device != nullptr);
  if (device != nullptr) {
    CHECK(device->layout == std::optional<std::string>("us,fr"));
    CHECK(device->options == std::optional<std::string>("grp:win_space_toggle"));
  }
}

UMBRIEL_TEST(tabletConfigLoads) {
  const TempConfig file;
  file.write(R"(
[input.tablet]
enabled = false
map_to_output = "DP-1"
map_to_focused_output = true
map_to_focused_window = true
left_handed = true
calibration_matrix = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& tablet = store.config().input.tablet;

  CHECK(result.success);
  CHECK(!tablet.enabled);
  CHECK_EQ(tablet.mapToOutput, std::string{"DP-1"});
  CHECK(tablet.mapToFocusedOutput);
  CHECK(tablet.mapToFocusedWindow);
  CHECK(tablet.leftHanded);
  CHECK(tablet.calibrationMatrix.has_value());
  if (tablet.calibrationMatrix.has_value()) {
    CHECK_EQ(*tablet.calibrationMatrix, (std::array<float, 6>{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}));
  }
}

UMBRIEL_TEST(tabletCalibrationMatrixRejectsWrongShape) {
  const TempConfig file;
  file.write(R"(
[input.tablet]
calibration_matrix = [1.0, 2.0, 3.0, 4.0, 5.0]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.tablet.calibrationMatrix.has_value());
  CHECK(containsDiagnostic(store, "calibration_matrix"));

  const TempConfig stringElement;
  stringElement.write(R"(
[input.tablet]
calibration_matrix = [1.0, 2.0, 3.0, "x", 5.0, 6.0]
)");

  store.setRootPath(stringElement.path(), true);
  const umbriel::ConfigReloadResult second = store.reload();

  CHECK(second.success);
  CHECK(!store.config().input.tablet.calibrationMatrix.has_value());
  CHECK(containsDiagnostic(store, "calibration_matrix"));
}

UMBRIEL_TEST(tabletConfigDefaults) {
  const umbriel::Config defaults;
  const auto& tablet = defaults.input.tablet;
  CHECK(tablet.enabled);
  CHECK_EQ(tablet.mapToOutput, std::string{});
  CHECK(!tablet.mapToFocusedOutput);
  CHECK(!tablet.mapToFocusedWindow);
  CHECK(!tablet.leftHanded);
  CHECK(!tablet.calibrationMatrix.has_value());
}

UMBRIEL_TEST(animationUsesCanonicalTopLevelNamespace) {
  const TempConfig file;
  file.write(R"(
[animation]
enabled = false
duration_ms = 320
curve = "linear"

[animation.beziers]
custom = [0.1, 0.2, 0.3, 1.0]

[animation.springs]
bouncy = { damping = 0.5, stiffness = 200 }

[animation.windows_in]
enabled = false
duration_ms = 450
curve = "custom"
style = "zoom"
scale = 0.7

[animation.windows_out]
curve = "bouncy"
style = "slide"

[animation.overview]
enabled = false
duration_ms = 700
curve = "custom"

[animation.scratchpad]
dim = 0.4
blur = true
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();
  const auto& animation = store.config().animation;

  CHECK(result.success);
  CHECK(!animation.enabled);
  CHECK_EQ(animation.durationMs, 320);
  CHECK(animation.curve.easing == umbriel::Easing::Linear);
  CHECK(!animation.windowsIn.enabled);
  CHECK_EQ(animation.windowsIn.durationMs, 450);
  CHECK(animation.windowsIn.curve.easing == umbriel::Easing::CustomBezier);
  CHECK_EQ(animation.windowsIn.style, std::string{"zoom"});
  CHECK_EQ(animation.windowsIn.scale, 0.7);
  CHECK(animation.windowsOut.curve.easing == umbriel::Easing::Spring);
  CHECK_EQ(animation.windowsOut.style, std::string{"slide"});
  CHECK(!animation.overview.enabled);
  CHECK_EQ(animation.overview.durationMs, 700);
  CHECK(animation.overview.curve.easing == umbriel::Easing::CustomBezier);
  CHECK_EQ(animation.windowsMove.durationMs, 320);
  CHECK_EQ(animation.scratchpad.dim, 0.4);
  CHECK(animation.scratchpad.blur);

  file.write(R"(
[appearance.animations]
enabled = false

[animations]
enabled = false

[animation.fade]
enabled = true
)");
  CHECK(store.reload().success);
  CHECK(store.config().animation.enabled);
  CHECK(!store.config().animation.layers.enabled);
  CHECK(containsDiagnostic(store, "unknown key appearance.animations"));
  CHECK(containsDiagnostic(store, "unknown key animations"));
  CHECK(containsDiagnostic(store, "unknown key animation.fade"));
}

UMBRIEL_TEST(environmentRequiresStringValuesAndPortableNames) {
  const TempConfig file;
  file.write(R"(
[environment]
DXVK_HDR = "1"
_PRIVATE = "kept"
"9INVALID" = "ignored"
"HAS-HYPHEN" = "ignored"
NOT_A_STRING = 1
WAYLAND_DISPLAY = "wrong"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().environment.variables.size(), size_t{2});
  CHECK(
      std::ranges::find(store.config().environment.variables, std::pair{std::string{"DXVK_HDR"}, std::string{"1"}})
      != store.config().environment.variables.end()
  );
  CHECK(
      std::ranges::find(store.config().environment.variables, std::pair{std::string{"_PRIVATE"}, std::string{"kept"}})
      != store.config().environment.variables.end()
  );
  CHECK(containsDiagnostic(store, R"(ignoring environment key "9INVALID" (expected [A-Za-z_][A-Za-z0-9_]*))"));
  CHECK(containsDiagnostic(store, R"(ignoring environment key "HAS-HYPHEN" (expected [A-Za-z_][A-Za-z0-9_]*))"));
  CHECK(containsDiagnostic(store, "ignoring environment.NOT_A_STRING (expected string)"));
  CHECK(containsDiagnostic(store, "ignoring environment.WAYLAND_DISPLAY (reserved by Umbriel)"));
  CHECK(!containsDiagnostic(store, "unknown key environment.DXVK_HDR"));
  CHECK(!containsDiagnostic(store, "unknown key environment._PRIVATE"));
}

UMBRIEL_TEST(packagedAnimationDefaultsMatchCompiledDefaults) {
  std::filesystem::path root = std::filesystem::current_path();
  while (!std::filesystem::exists(root / "examples/config.toml")) {
    const std::filesystem::path parent = root.parent_path();
    if (parent == root) {
      CHECK(false);
      return;
    }
    root = parent;
  }

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(root / "examples/config.toml", true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().animation == umbriel::Config{}.animation);
}

int main() { return RUN_TESTS(); }
