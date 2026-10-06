#include "check.h"
#include "config/resolve.h"
#include "config/store.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <linux/input-event-codes.h>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>

using umbriel::ConfigDiagnostic;
using umbriel::ConfigStore;
using umbriel::ContentType;
using umbriel::HdrMode;
using umbriel::LayoutMode;
using umbriel::ModifierKey;
using umbriel::TrackLayout;
using umbriel::VrrMode;
using umbriel::WindowDragToggle;

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
  CHECK(store.load(nullptr));

  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);
  CHECK(!store.fileMissing());

  tree.write("user/umbriel/config.toml", "[layout]\ngap = 19\n");
  CHECK(store.load(nullptr));

  CHECK_EQ(store.rootPath(), userConfig);
  CHECK_EQ(store.config().layout.gap, 19);
  CHECK(!store.fileMissing());
}

UMBRIEL_TEST(implicitConfigReloadAdoptsAndReleasesHigherPriorityUserPath) {
  const TempConfigTree tree;
  const std::filesystem::path userHome = tree.path("user");
  const std::filesystem::path systemDir = tree.path("system");
  const std::filesystem::path systemConfig = systemDir / "umbriel/config.toml";
  const std::filesystem::path userConfig = userHome / "umbriel/config.toml";
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", userHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());

  ConfigStore& store = umbriel::configStore();
  CHECK(store.load(nullptr));

  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);

  tree.write("user/umbriel/config.toml", "[layout]\ngap = 19\n");
  const umbriel::ConfigReloadResult adopted = store.reload();

  CHECK(adopted.success);
  CHECK_EQ(store.rootPath(), userConfig);
  CHECK_EQ(store.config().layout.gap, 19);

  std::filesystem::remove(userConfig);
  const umbriel::ConfigReloadResult released = store.reload();

  CHECK(released.success);
  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);
}

UMBRIEL_TEST(malformedNewUserConfigKeepsActiveSystemConfigAndRetriesAfterCorrection) {
  const TempConfigTree tree;
  const std::filesystem::path userHome = tree.path("user");
  const std::filesystem::path systemDir = tree.path("system");
  const std::filesystem::path systemConfig = systemDir / "umbriel/config.toml";
  const std::filesystem::path userConfig = userHome / "umbriel/config.toml";
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", userHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());

  ConfigStore& store = umbriel::configStore();
  CHECK(store.load(nullptr));
  const uint64_t generation = store.generation();

  tree.write("user/umbriel/config.toml", "[layout\n");
  const umbriel::ConfigReloadResult malformed = store.reload();

  CHECK(!malformed.success);
  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);
  CHECK_EQ(store.generation(), generation);
  CHECK(std::ranges::find(store.watchPaths(), userConfig) != store.watchPaths().end());
  CHECK(std::ranges::find(store.watchPaths(), systemConfig) != store.watchPaths().end());

  tree.write("user/umbriel/config.toml", "[layout]\ngap = 19\n");
  const umbriel::ConfigReloadResult corrected = store.reload();

  CHECK(corrected.success);
  CHECK_EQ(store.rootPath(), userConfig);
  CHECK_EQ(store.config().layout.gap, 19);
  CHECK_EQ(store.generation(), generation + 1);
}

UMBRIEL_TEST(implicitConfigReloadKeepsCandidatesCapturedAtInitialLoad) {
  const TempConfigTree tree;
  const std::filesystem::path initialUserHome = tree.path("initial-user");
  const std::filesystem::path changedUserHome = tree.path("changed-user");
  const std::filesystem::path systemDir = tree.path("system");
  const std::filesystem::path systemConfig = systemDir / "umbriel/config.toml";
  const std::filesystem::path changedUserConfig = changedUserHome / "umbriel/config.toml";
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", initialUserHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());

  ConfigStore& store = umbriel::configStore();
  CHECK(store.load(nullptr));
  const std::vector<std::filesystem::path> initialWatchPaths = store.watchPaths();

  tree.write("changed-user/umbriel/config.toml", "[layout]\ngap = 29\n");
  const ScopedEnvironment changedConfigHome("XDG_CONFIG_HOME", changedUserHome.string());
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.rootPath(), systemConfig);
  CHECK_EQ(store.config().layout.gap, 17);
  CHECK_EQ(store.watchPaths(), initialWatchPaths);
  CHECK(std::ranges::find(store.watchPaths(), changedUserConfig) == store.watchPaths().end());
}

UMBRIEL_TEST(explicitConfigReloadStaysPinnedWhenUserConfigAppears) {
  const TempConfigTree tree;
  const std::filesystem::path userHome = tree.path("user");
  const std::filesystem::path systemDir = tree.path("system");
  const std::filesystem::path explicitConfig = tree.path("chosen.toml");
  tree.write("chosen.toml", "[layout]\ngap = 23\n");
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", userHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());

  ConfigStore& store = umbriel::configStore();
  const std::string explicitPath = explicitConfig.string();
  CHECK(store.load(explicitPath.c_str()));

  CHECK_EQ(store.rootPath(), explicitConfig);
  CHECK_EQ(store.config().layout.gap, 23);

  tree.write("user/umbriel/config.toml", "[layout]\ngap = 29\n");
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.rootPath(), explicitConfig);
  CHECK_EQ(store.config().layout.gap, 23);
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
extent_presets = [0.05, 0.5, 2.0]

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
extent_presets = [0.25, 0.75]

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
  CHECK_EQ(store.config().layout.extentPresets.size(), size_t{3});
  CHECK_EQ(store.config().layout.extentPresets[0], 0.1);
  CHECK_EQ(store.config().layout.extentPresets[1], 0.5);
  CHECK_EQ(store.config().layout.extentPresets[2], 1.0);
  CHECK(!store.config().layout.scrolling.centerUnderfullStrip);
  CHECK(store.config().layout.dwindle.preserveSplit);
  CHECK(store.config().appearance.preferNoCsd);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].scale.has_value());
  CHECK_EQ(*store.config().outputs[0].scale, 4.0);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.mode == LayoutMode::Scrolling);
  CHECK(store.config().workspaceRules[0].layout.extentPresets.has_value());
  CHECK_EQ(store.config().workspaceRules[0].layout.extentPresets->size(), size_t{2});
  CHECK(store.config().workspaceRules[0].layout.scrolling.centerUnderfullStrip == true);
  CHECK(store.config().workspaceRules[0].layout.dwindle.preserveSplit == false);
  CHECK(containsDiagnostic(store, "unknown key unknown_root_key"));
  CHECK(containsDiagnostic(store, "output.DP-1.scale = 9"));
  CHECK(containsDiagnostic(store, "unknown key layout.scrolling.always_center_single_column"));
  CHECK(containsDiagnostic(store, "unknown key general.prefer_no_csd"));
}

UMBRIEL_TEST(rejectsRemovedWidthPresetKey) {
  const TempConfig file;
  file.write("[layout]\nwidth_presets = [0.75]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK_EQ(store.config().layout.extentPresets.size(), size_t{3});
  CHECK(containsDiagnostic(store, "unknown key layout.width_presets"));
}

UMBRIEL_TEST(backgroundDefaultsOpaque) {
  const umbriel::Config config;
  CHECK_EQ(config.colors.background[3], 1.0F);
}

UMBRIEL_TEST(scratchpadDefinitionsLoadUniqueNames) {
  const TempConfig file;
  file.write(R"(
[[scratchpad]]
name = "term"

[[scratchpad]]
name = "music"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().scratchpads.size(), size_t{2});
  CHECK_EQ(store.config().scratchpads[0].name, std::string{"term"});
  CHECK_EQ(store.config().scratchpads[1].name, std::string{"music"});
  CHECK(!containsDiagnostic(store, "unknown key scratchpad"));
}

UMBRIEL_TEST(scratchpadDefinitionsLoadSpawnWhenEmpty) {
  const TempConfig file;
  file.write(R"(
[[scratchpad]]
name = "sysmon"
spawn_when_empty = "kitty --class btop btop"

[[scratchpad]]
name = "music"

[[scratchpad]]
name = "todo"
spawn_when_empty = 7
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().scratchpads.size(), size_t{3});
  CHECK_EQ(store.config().scratchpads[0].spawnWhenEmpty, std::string{"kitty --class btop btop"});
  CHECK(store.config().scratchpads[1].spawnWhenEmpty.empty());
  CHECK(store.config().scratchpads[2].spawnWhenEmpty.empty());
  CHECK(containsDiagnostic(store, "ignoring scratchpad[2].spawn_when_empty (expected string)"));
}

UMBRIEL_TEST(scratchpadDefinitionsRequireValidUniqueNames) {
  const TempConfig file;
  file.write("[[scratchpad]]\nname = \"kept\"\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  const umbriel::Config previous = store.config();

  const std::array invalid{
      std::pair{
          std::string{"scratchpad = \"named\"\n"}, std::string{"scratchpad must be a [[scratchpad]] array of tables"}
      },
      std::pair{std::string{"[[scratchpad]]\n"}, std::string{"scratchpad[0] must set name"}},
      std::pair{std::string{"[[scratchpad]]\nname = 7\n"}, std::string{"scratchpad[0].name must be a string"}},
      std::pair{std::string{"[[scratchpad]]\nname = \"\"\n"}, std::string{"scratchpad[0].name must not be empty"}},
      std::pair{
          std::string{"[[scratchpad]]\nname = \"default\"\n"},
          std::string{"scratchpad[0].name 'default' is reserved for the implicit scratchpad"}
      },
      std::pair{
          std::string{"[[scratchpad]]\nname = \"term\"\n[[scratchpad]]\nname = \"term\"\n"},
          std::string{"scratchpad[1].name duplicates scratchpad name 'term'"}
      },
  };

  for (const auto& [contents, expectedDiagnostic] : invalid) {
    file.write(contents);
    const umbriel::ConfigReloadResult result = store.reload();
    CHECK(!result.success);
    CHECK(store.config() == previous);
    CHECK(containsDiagnostic(store, expectedDiagnostic));
  }
}

UMBRIEL_TEST(scratchpadDefinitionsReportUnknownKeysWithoutDiscardingTheEntry) {
  const TempConfig file;
  file.write(R"(
[[scratchpad]]
name = "term"
output = "DP-1"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().scratchpads.size(), size_t{1});
  CHECK_EQ(store.config().scratchpads[0].name, std::string{"term"});
  CHECK(containsDiagnostic(store, "unknown key scratchpad[0].output"));
}

UMBRIEL_TEST(implicitScratchpadActionsAcceptOnlyTheDefaultTarget) {
  const TempConfig file;
  file.write(R"(
[keybinds]
"Mod+1" = "scratchpad-toggle"
"Mod+2" = "window-move-to-scratchpad:default"
"Mod+3" = "scratchpad-focus-next:other"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  const auto countTarget = [&](std::string_view name) {
    size_t count = 0;
    for (const auto& keybind : store.config().keybinds) {
      const auto* target = umbriel::payloadIf<umbriel::ScratchpadArg>(keybind);
      count += target != nullptr && target->name == name ? 1U : 0U;
    }
    return count;
  };

  CHECK(result.success);
  CHECK(store.config().scratchpads.empty());
  CHECK_EQ(countTarget(""), size_t{1});
  CHECK_EQ(countTarget("default"), size_t{1});
  CHECK_EQ(countTarget("other"), size_t{0});
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+3' (unknown scratchpad 'other')"));
}

UMBRIEL_TEST(namedScratchpadActionsRequireAConfiguredName) {
  const TempConfig file;
  file.write(R"(
[[scratchpad]]
name = "term"

[keybinds]
"Mod+1" = "window-restore-from-scratchpad"
"Mod+2" = "window-toggle-scratchpad:term"
"Mod+3" = "scratchpad-toggle:missing"
"Mod+4" = "window-move-to-scratchpad:default"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  const auto countTarget = [&](std::string_view name) {
    size_t count = 0;
    for (const auto& keybind : store.config().keybinds) {
      const auto* target = umbriel::payloadIf<umbriel::ScratchpadArg>(keybind);
      count += target != nullptr && target->name == name ? 1U : 0U;
    }
    return count;
  };

  CHECK(result.success);
  CHECK_EQ(store.config().scratchpads.size(), size_t{1});
  CHECK_EQ(countTarget(""), size_t{0});
  CHECK_EQ(countTarget("term"), size_t{1});
  CHECK_EQ(countTarget("missing"), size_t{0});
  CHECK_EQ(countTarget("default"), size_t{0});
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+1' (scratchpad name required)"));
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+3' (unknown scratchpad 'missing')"));
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+4' (unknown scratchpad 'default')"));
}

UMBRIEL_TEST(dwindlePreserveSplitDefaultsToFalse) {
  const TempConfig file;
  file.write("[layout]\n");
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK(!store.config().layout.dwindle.preserveSplit);
}

UMBRIEL_TEST(layoutStrutsLoadGloballyAndPerWorkspace) {
  const TempConfig file;
  file.write(R"(
[layout.struts]
left = -12
right = 24
top = 36
bottom = -48
surprise = 1

[output.DP-1]
workspaces = ["dev"]

[[workspace]]
name = "dev"

[workspace.layout.struts]
left = 50
bottom = -8
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().layout.struts.left, -12);
  CHECK_EQ(store.config().layout.struts.right, 24);
  CHECK_EQ(store.config().layout.struts.top, 36);
  CHECK_EQ(store.config().layout.struts.bottom, -48);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  const auto& overrides = store.config().workspaceRules[0].layout.struts;
  CHECK(overrides.left.has_value());
  CHECK_EQ(*overrides.left, 50);
  CHECK(!overrides.right.has_value());
  CHECK(!overrides.top.has_value());
  CHECK(overrides.bottom.has_value());
  CHECK_EQ(*overrides.bottom, -8);
  CHECK(containsDiagnostic(store, "unknown key layout.struts.surprise"));
}

UMBRIEL_TEST(masterLayoutReadersLoadGlobalAndWorkspaceSettings) {
  const TempConfig file;
  file.write(R"(
[layout]
mode = "master"

[layout.master]
position = "right"
default_width_fraction = 0.05
new_on_top = false
new_becomes_master = true
surprise = true

[output.DP-1]
workspaces = ["dev"]

[[workspace]]
name = "dev"

[workspace.layout.master]
position = "left"
default_width_fraction = 0.7
new_on_top = true
new_becomes_master = false
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().layout.mode == LayoutMode::Master);
  CHECK(store.config().layout.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(store.config().layout.master.defaultWidthFraction, 0.1);
  CHECK(!store.config().layout.master.newOnTop);
  CHECK(store.config().layout.master.newBecomesMaster);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.master.position == umbriel::MasterPosition::Left);
  CHECK(store.config().workspaceRules[0].layout.master.defaultWidthFraction.has_value());
  CHECK_EQ(*store.config().workspaceRules[0].layout.master.defaultWidthFraction, 0.7);
  CHECK(store.config().workspaceRules[0].layout.master.newOnTop == true);
  CHECK(store.config().workspaceRules[0].layout.master.newBecomesMaster.has_value());
  CHECK(store.config().workspaceRules[0].layout.master.newBecomesMaster == false);
  CHECK(containsDiagnostic(store, "layout.master.default_width_fraction = 0.05 out of range, clamped to 0.1"));
  CHECK(containsDiagnostic(store, "unknown key layout.master.surprise"));
}

UMBRIEL_TEST(tabReadersLoadLayoutAppearanceColorsAndRules) {
  const TempConfig file;
  file.write(R"(
[layout.tabs]
default_display = "tabbed"
new_tab_position = "after_active"
wrap_focus = false
scroll_switches_tabs = false
middle_click_closes = true

[appearance.tab_bar]
style = "indicator"
position = "bottom"
height = 4
font = "Inter Bold 9"
tab_gap = 0
corner_radius = 1
title_format = "{index}: {title}"
title_align = "left"
hide_when_single = true
max_tabs = 5
overflow = "shrink"

[colors.tab_bar]
urgent = "#FF000080"

[output.DP-1]
workspaces = ["dev"]

[[workspace]]
name = "dev"

[workspace.layout.tabs]
default_display = "normal"

[[window_rule]]
match.app_id = "^firefox$"
default_column_display = "tabbed"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  const umbriel::Config& config = store.config();
  CHECK(config.layout.tabs.defaultDisplay == umbriel::ColumnDisplay::Tabbed);
  CHECK(config.layout.tabs.newTabPosition == umbriel::NewTabPosition::AfterActive);
  CHECK(!config.layout.tabs.wrapFocus);
  CHECK(!config.layout.tabs.scrollSwitchesTabs);
  CHECK(config.layout.tabs.middleClickCloses);
  CHECK(config.appearance.tabBar.style == umbriel::TabBarLook::Indicator);
  CHECK(config.appearance.tabBar.position == umbriel::TabBarPosition::Bottom);
  CHECK_EQ(config.appearance.tabBar.height, 4);
  CHECK_EQ(config.appearance.tabBar.font, std::string{"Inter Bold 9"});
  CHECK_EQ(config.appearance.tabBar.tabGap, 0);
  CHECK_EQ(config.appearance.tabBar.cornerRadius, 1);
  CHECK_EQ(config.appearance.tabBar.titleFormat, std::string{"{index}: {title}"});
  CHECK(config.appearance.tabBar.titleAlign == umbriel::TitleAlign::Left);
  CHECK(config.appearance.tabBar.hideWhenSingle);
  CHECK_EQ(config.appearance.tabBar.maxTabs, 5);
  CHECK(config.appearance.tabBar.overflow == umbriel::TabOverflow::Shrink);
  CHECK_EQ(config.colors.tabBar.urgent[3], 128.0F / 255.0F);
  CHECK_EQ(config.workspaceRules.size(), size_t{1});
  CHECK(config.workspaceRules[0].layout.tabs.defaultDisplay == umbriel::ColumnDisplay::Normal);
  CHECK_EQ(config.windowRules.size(), size_t{1});
  CHECK(config.windowRules[0].defaultColumnDisplay == umbriel::ColumnDisplay::Tabbed);

  // The bar's space is part of the layout, so resolution carries it with the behavior keys.
  const umbriel::ResolvedLayoutConfig resolved = umbriel::resolveGlobalLayout(config);
  CHECK(resolved.tabs.defaultDisplay == umbriel::ColumnDisplay::Tabbed);
  CHECK_EQ(resolved.tabs.barHeight, 4);
  CHECK(resolved.tabs.barPosition == umbriel::TabBarPosition::Bottom);
  CHECK(resolved.tabs.hideWhenSingle);
}

UMBRIEL_TEST(masterPositionAcceptsCenterAndRejectsOtherValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[layout.master]\nposition = \"center\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.master.position == umbriel::MasterPosition::Center);

  file.write("[layout.master]\nposition = \"middle\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.master.position == umbriel::MasterPosition::Left);
}

UMBRIEL_TEST(scrollingDefaultExtentIsOptional) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[layout.scrolling]\ncenter_underfull_strip = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.defaultExtentFraction.has_value());

  file.write("[layout.scrolling]\ndefault_extent_fraction = 0.75\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.scrolling.defaultExtentFraction.has_value());
  CHECK_EQ(*store.config().layout.scrolling.defaultExtentFraction, 0.75);

  file.write("[layout.scrolling]\ndefault_width_fraction = 0.25\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.defaultExtentFraction.has_value());
  CHECK(containsDiagnostic(store, "unknown key layout.scrolling.default_width_fraction"));

  file.write("[layout.scrolling]\ncenter_underfull_strip = true\n");
  CHECK(store.reload().success);
  CHECK(!store.config().layout.scrolling.defaultExtentFraction.has_value());
}

UMBRIEL_TEST(outputScrollingDefaultExtentUsesNarrowLayoutScope) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[output.DP-1.layout]
gap = 12

[output.DP-1.layout.scrolling]
default_extent_fraction = 0.05
center_focused = true
)");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].layout.scrolling.defaultExtentFraction.has_value());
  if (store.config().outputs[0].layout.scrolling.defaultExtentFraction) {
    CHECK_EQ(*store.config().outputs[0].layout.scrolling.defaultExtentFraction, 0.1);
  }
  CHECK(containsDiagnostic(
      store, "output.DP-1.layout.scrolling.default_extent_fraction = 0.05 out of range, clamped to 0.1"
  ));
  CHECK(containsDiagnostic(store, "unknown key output.DP-1.layout.gap"));
  CHECK(containsDiagnostic(store, "unknown key output.DP-1.layout.scrolling.center_focused"));

  file.write("[output.DP-1]\nenabled = true\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].layout.scrolling.defaultExtentFraction.has_value());
}

UMBRIEL_TEST(outputWorkspaceAxisAcceptsOnlyItsTwoNames) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nworkspace_axis = \"horizontal\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].workspaceAxis == umbriel::WorkspaceAxis::Horizontal);
  CHECK(!containsDiagnostic(store, "unknown key output.DP-1.workspace_axis"));

  file.write("[output.DP-1]\nworkspace_axis = \"sideways\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].workspaceAxis == umbriel::WorkspaceAxis::Vertical);

  file.write("[output.DP-1]\nworkspace_axis = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].workspaceAxis == umbriel::WorkspaceAxis::Vertical);

  file.write("[output.DP-1]\nenabled = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].workspaceAxis == umbriel::WorkspaceAxis::Vertical);
}

// The configurable strip direction is gone: both spellings are ordinary unknown keys.
UMBRIEL_TEST(scrollingDirectionKeysAreUnknown) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[layout.scrolling]
direction = "vertical"

[[workspace]]
index = 1
layout.scrolling.direction = "vertical"
)");
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "unknown key layout.scrolling.direction"));
  CHECK(containsDiagnostic(store, "unknown key workspace[0].layout.scrolling.direction"));
}

UMBRIEL_TEST(centerFocusedReadsItsModeVocabulary) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[layout.scrolling]
center_focused = "on_overflow"

[[workspace]]
index = 1
layout.scrolling.center_focused = "always"
)");
  CHECK(store.reload().success);
  CHECK(store.config().layout.scrolling.centerFocused == umbriel::CenterFocusedColumn::OnOverflow);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.scrolling.centerFocused == umbriel::CenterFocusedColumn::Always);

  file.write("[layout.scrolling]\ncenter_focused = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().layout.scrolling.centerFocused == umbriel::CenterFocusedColumn::Never);
  CHECK(!containsDiagnostic(store, "unknown key layout.scrolling.center_focused"));
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

UMBRIEL_TEST(keybindTableLoadsAllowWhenInhibited) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[keybinds]\n"
      "\"Mod+Escape\" = { action = \"shortcuts-inhibit-toggle\", allow_when_inhibited = true }\n"
      "\"Mod+Return\" = \"spawn:terminal\"\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().keybinds.size(), size_t{2});

  bool allowedWhenInhibited = false;
  bool defaultsToBlocked = false;
  for (const auto& bind : store.config().keybinds) {
    allowedWhenInhibited = allowedWhenInhibited || bind.allowWhenInhibited;
    defaultsToBlocked = defaultsToBlocked || !bind.allowWhenInhibited;
  }
  CHECK(allowedWhenInhibited);
  CHECK(defaultsToBlocked);
  CHECK(!containsDiagnostic(store, "allow_when_inhibited"));
}

UMBRIEL_TEST(keybindTablePreservesWorkspaceReferenceKinds) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[keybinds]\n"
      "\"Mod+2\" = \"workspace-switch:2\"\n"
      "\"Mod+Ctrl+2\" = 'workspace-switch:\"2\"'\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().keybinds.size(), size_t{2});

  bool foundPosition = false;
  bool foundNumericName = false;
  for (const auto& bind : store.config().keybinds) {
    const auto* workspace = umbriel::payloadIf<umbriel::WorkspaceArg>(bind);
    if (workspace == nullptr) {
      continue;
    }
    if (const auto* index = std::get_if<umbriel::WorkspaceIndex>(&workspace->reference)) {
      foundPosition = foundPosition || index->value == 2;
    }
    if (const auto* name = std::get_if<umbriel::WorkspaceName>(&workspace->reference)) {
      foundNumericName = foundNumericName || name->value == "2";
    }
  }
  CHECK(foundPosition);
  CHECK(foundNumericName);
}

UMBRIEL_TEST(keybindTableLoadsCooldown) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[keybinds]\n\"Mod+Return\" = { action = \"spawn:terminal\", cooldown_ms = 150 }\n");
  CHECK(store.reload().success);
  CHECK(std::ranges::any_of(store.config().keybinds, [](const auto& bind) { return bind.cooldownMs == 150; }));
  CHECK(!containsDiagnostic(store, "cooldown_ms"));

  file.write("[keybinds]\n\"Mod+Return\" = { action = \"spawn:terminal\", cooldown_ms = 3600001 }\n");
  CHECK(store.reload().success);
  CHECK(std::ranges::any_of(store.config().keybinds, [](const auto& bind) { return bind.cooldownMs == 3600000; }));
  CHECK(containsDiagnostic(store, "cooldown_ms = 3600001 out of range, clamped to 3600000"));
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

UMBRIEL_TEST(implicitScratchpadHotCornersAcceptOnlyTheDefaultTarget) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[hot_corners.top_left]
enabled = true
action = "scratchpad-toggle"

[hot_corners.top_right]
enabled = true
action = "window-move-to-scratchpad:default"

[hot_corners.bottom_left]
enabled = true
action = "scratchpad-focus-next:missing"
)");
  CHECK(store.reload().success);

  const auto& corners = store.config().hotCorners.corners;
  CHECK(corners[0].action.has_value());
  CHECK(corners[1].action.has_value());
  CHECK(!corners[2].action.has_value());
  const auto* bare = corners[0].action ? umbriel::payloadIf<umbriel::ScratchpadArg>(*corners[0].action) : nullptr;
  const auto* explicitDefault =
      corners[1].action ? umbriel::payloadIf<umbriel::ScratchpadArg>(*corners[1].action) : nullptr;
  CHECK(bare != nullptr);
  CHECK(bare != nullptr && bare->name.empty());
  CHECK(explicitDefault != nullptr);
  CHECK(explicitDefault != nullptr && explicitDefault->name == "default");
  CHECK(containsDiagnostic(store, "ignoring hot_corners.bottom_left.action (unknown scratchpad 'missing')"));
}

UMBRIEL_TEST(namedScratchpadHotCornersRequireAConfiguredName) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[[scratchpad]]
name = "term"

[hot_corners.top_left]
enabled = true
action = "scratchpad-toggle"

[hot_corners.top_right]
enabled = true
action = "window-toggle-scratchpad:term"

[hot_corners.bottom_left]
enabled = true
action = "window-restore-from-scratchpad:missing"
)");
  CHECK(store.reload().success);

  const auto& corners = store.config().hotCorners.corners;
  CHECK(!corners[0].action.has_value());
  CHECK(corners[1].action.has_value());
  CHECK(!corners[2].action.has_value());
  const auto* configured = corners[1].action ? umbriel::payloadIf<umbriel::ScratchpadArg>(*corners[1].action) : nullptr;
  CHECK(configured != nullptr);
  CHECK(configured != nullptr && configured->name == "term");
  CHECK(containsDiagnostic(store, "ignoring hot_corners.top_left.action (scratchpad name required)"));
  CHECK(containsDiagnostic(store, "ignoring hot_corners.bottom_left.action (unknown scratchpad 'missing')"));
}

UMBRIEL_TEST(overviewBackgroundBlurLoads) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nbackground_blur = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.backgroundBlur);
}

UMBRIEL_TEST(overviewScrollFactorLoadsIndependently) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[overview]\nscroll_factor_horizontal = 0.7\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.scrollFactorHorizontal, 0.7);
  CHECK_EQ(store.config().overview.scrollFactorVertical, 1.0);
  file.write("[overview]\nscroll_factor_horizontal = 1.2\nscroll_factor_vertical = 0.8\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.scrollFactorHorizontal, 1.2);
  CHECK_EQ(store.config().overview.scrollFactorVertical, 0.8);
  file.write("[overview]\nzoom = 0.5\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().overview.scrollFactorHorizontal, 1.0);
  CHECK_EQ(store.config().overview.scrollFactorVertical, 1.0);
}

UMBRIEL_TEST(touchpadScrollFactorLoadsAsScalarOrTable) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[input.touchpad]\nscroll_factor = 1.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.touchpad.scrollFactor.has_value());
  CHECK_EQ(store.config().input.touchpad.scrollFactor->horizontal, (1.5));
  CHECK_EQ(store.config().input.touchpad.scrollFactor->vertical, (1.5));
  file.write("[input.touchpad]\nscroll_factor = { horizontal = 0.7 }\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.touchpad.scrollFactor->horizontal, (0.7));
  CHECK(!store.config().input.touchpad.scrollFactor->vertical.has_value());
  file.write("[input.touchpad]\nscroll_factor = { horizontal = 1.2, vertical = 0.8 }\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.touchpad.scrollFactor->horizontal, (1.2));
  CHECK_EQ(store.config().input.touchpad.scrollFactor->vertical, (0.8));
  file.write("[input.touchpad]\ntap = true\n");
  CHECK(store.reload().success);
  CHECK(!store.config().input.touchpad.scrollFactor.has_value());
  file.write("[input.touchpad]\nscroll_factor = \"fast\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().input.touchpad.scrollFactor.has_value());
}

UMBRIEL_TEST(overviewWorkspaceCurveLoadsAndFallsBackToItsSpring) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[animation.overview]\nworkspace_curve = \"easeout\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().animation.overview.workspaceCurve.easing == umbriel::Easing::EaseOutCubic);
  file.write("[animation.overview]\nworkspace_curve = \"spring:0.6,120\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().animation.overview.workspaceCurve.easing == umbriel::Easing::Spring);
  CHECK_EQ(store.config().animation.overview.workspaceCurve.spring.damping, 0.6);
  CHECK_EQ(store.config().animation.overview.workspaceCurve.spring.stiffness, 120.0);
  file.write("[animation.overview]\nduration_ms = 300\n");
  CHECK(store.reload().success);
  CHECK(store.config().animation.overview.workspaceCurve.easing == umbriel::Easing::Spring);
  CHECK_EQ(store.config().animation.overview.workspaceCurve.spring.stiffness, 1000.0);
}

UMBRIEL_TEST(durationBesideASpringCurveIsReportedAsInert) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  // A spring derives its own length, so the duration next to it reaches nothing and must not look honoured.
  file.write("[animation.windows_in]\nduration_ms = 200\ncurve = \"spring:1,1000\"\n");
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "animation.windows_in.duration_ms has no effect"));

  // The same duration with a duration-based curve is honoured and silent.
  file.write("[animation.windows_in]\nduration_ms = 200\ncurve = \"easeout\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().animation.windowsIn.durationMs, 200);
  CHECK(!containsDiagnostic(store, "has no effect"));

  // A shared spring curve makes every event derive its length, which leaves the shared duration inert too.
  file.write("[animation]\nduration_ms = 200\ncurve = \"spring:1,1000\"\n");
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "animation.duration_ms has no effect"));

  // One duration-based event is enough for the shared duration to reach something.
  file.write(
      "[animation]\nduration_ms = 200\ncurve = \"spring:1,1000\"\n\n[animation.workspaces]\ncurve = \"easeout\"\n"
  );
  CHECK(store.reload().success);
  CHECK(!containsDiagnostic(store, "animation.duration_ms has no effect"));
  CHECK_EQ(store.config().animation.workspaces.durationMs, 200);
}

UMBRIEL_TEST(overviewWorkspaceWallpaperLoads) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nzoom = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().overview.workspaceWallpaper);

  file.write("[overview]\nworkspace_wallpaper = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.workspaceWallpaper);
}

UMBRIEL_TEST(overviewShortcutConfigurationLoads) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[overview]\nshortcuts = false\nshortcut_keys = \"asdf\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().overview.shortcuts);
  CHECK_EQ(store.config().overview.shortcutKeys, std::string{"asdf"});
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

UMBRIEL_TEST(colorsSectionOwnsEveryColor) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[colors]\ninsert_hint = \"#11223344\"\nbackdrop = \"#55667788\"\nshadow = \"#99AABBCC\"\n"
      "[colors.border]\nfocused = \"#01020304\"\nunfocused = \"#05060708\"\nouter = \"#11121314\"\n"
      "[colors.overview]\nbackground_tint = \"#15161718\"\nworkspace_background = \"#191A1B1C\"\n"
      "badge = \"#12345678\"\n"
  );
  CHECK(store.reload().success);
  const auto& colors = store.config().colors;
  CHECK_EQ(colors.insertHint[0], 17.0F / 255.0F);
  CHECK_EQ(colors.backdrop[1], 102.0F / 255.0F);
  CHECK_EQ(colors.shadow[3], 204.0F / 255.0F);
  CHECK_EQ(colors.border.focused[3], 4.0F / 255.0F);
  CHECK_EQ(colors.border.unfocused[0], 5.0F / 255.0F);
  CHECK_EQ(colors.border.outer[0], 17.0F / 255.0F);
  CHECK_EQ(colors.overview.backgroundTint[1], 22.0F / 255.0F);
  CHECK_EQ(colors.overview.workspaceBackground[2], 27.0F / 255.0F);
  CHECK_EQ(colors.overview.badge[0], 18.0F / 255.0F);
  CHECK_EQ(colors.overview.badge[3], 120.0F / 255.0F);
}

// Per-window border colors override the global [colors.border] defaults and are read from window rules.
UMBRIEL_TEST(windowRuleBorderColorsLoad) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[[window_rule]]\nmatch.is_scratchpad = true\n"
      "border_color_focused = \"#E5C07BFF\"\nborder_color_unfocused = \"#5C4A2AFF\"\n"
      "border_color_outer = \"#2A2010FF\"\n"
      "[[window_rule]]\nmatch.app_id = \"^foot$\"\nborder_color_focused = \"#FF6B6BFF\"\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{2});

  const auto& scratchpad = store.config().windowRules[0];
  CHECK(scratchpad.matchScratchpad && *scratchpad.matchScratchpad);
  CHECK(scratchpad.borderColorFocused && (*scratchpad.borderColorFocused)[0] == 229.0F / 255.0F);
  CHECK(scratchpad.borderColorFocused && (*scratchpad.borderColorFocused)[3] == 1.0F);
  CHECK(scratchpad.borderColorUnfocused && (*scratchpad.borderColorUnfocused)[1] == 74.0F / 255.0F);
  CHECK(scratchpad.borderColorOuter && (*scratchpad.borderColorOuter)[2] == 16.0F / 255.0F);
  CHECK(!containsDiagnostic(store, "unknown key window_rule.border_color_outer"));

  // Each key is independent: a rule that sets only some of them leaves the rest unset.
  const auto& foot = store.config().windowRules[1];
  CHECK(foot.borderColorFocused && (*foot.borderColorFocused)[0] == 1.0F);
  CHECK(!foot.borderColorUnfocused);
  CHECK(!foot.borderColorOuter);

  // A non-color value is rejected on the same keys.
  file.write("[[window_rule]]\nborder_color_focused = 12\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].borderColorFocused);
}

// Colors are recognized only inside [colors]; anywhere else they are ordinary
// unknown keys.
UMBRIEL_TEST(colorKeysOutsideTheColorsSectionAreUnknown) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  const umbriel::Config defaults;
  file.write("[appearance]\nborder_focused = \"#FFFFFFFF\"\nbackdrop_color = \"#000000FF\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().colors.border.focused[0], defaults.colors.border.focused[0]);
  CHECK(containsDiagnostic(store, "appearance.border_focused"));
  CHECK(containsDiagnostic(store, "appearance.backdrop_color"));

  file.write("[overview]\nbadge_color = \"#FFFFFFFF\"\nworkspace_background = \"#FFFFFFFF\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().colors.overview.badge[0], defaults.colors.overview.badge[0]);
  CHECK(containsDiagnostic(store, "overview.badge_color"));
  CHECK(containsDiagnostic(store, "overview.workspace_background"));
}

UMBRIEL_TEST(colorsRejectInvalidValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::Config defaults;

  file.write("[colors.overview]\nbadge = \"not-a-color\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().colors.overview.badge[0], defaults.colors.overview.badge[0]);
  CHECK(containsDiagnostic(store, "colors.overview.badge (invalid color"));
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

UMBRIEL_TEST(windowDragToggleReadsItsVocabularyAndDefaultsOff) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[input]\nwindow_drag_toggle = \"floating\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.windowDragToggle == WindowDragToggle::Floating);
  CHECK(!containsDiagnostic(store, "unknown key input.window_drag_toggle"));

  file.write("[input]\nwindow_drag_toggle = \"pinned\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.windowDragToggle == WindowDragToggle::Pinned);

  file.write("[input]\nwindow_drag_toggle = \"none\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.windowDragToggle == WindowDragToggle::None);

  file.write("[input]\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.windowDragToggle == WindowDragToggle::None);

  file.write("[input]\nwindow_drag_toggle = \"maximized\"\n");
  CHECK(store.reload().success);
  CHECK(store.config().input.windowDragToggle == WindowDragToggle::None);
  CHECK(containsDiagnostic(store, "ignoring input.window_drag_toggle"));
}

UMBRIEL_TEST(outputNamesDifferingOnlyByCaseAreRejectedAsDuplicates) {
  const TempConfig file;
  file.write(R"(
[output.DP-1]
scale = 1.5

[output.dp-1]
scale = 2.0
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(containsDiagnostic(store, "duplicate output section"));
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

UMBRIEL_TEST(outputBitDepthLoadsAndDefaults8) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK_EQ(store.config().outputs[0].bitDepth, 8);

  file.write("[output.DP-1]\nbit_depth = 10\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].bitDepth, 10);

  file.write("[output.DP-1]\nbit_depth = 8\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].bitDepth, 8);

  file.write("[output.DP-1]\nbit_depth = 12\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].bitDepth, 8);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.bit_depth"));

  file.write("[output.DP-1]\nbit_depth = \"ten\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].bitDepth, 8);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.bit_depth"));
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

  file.write("[[window_rule]]\nmatch.app_id = \"^game$\"\nhdr = \"fullscreen\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].hdr == HdrMode::Fullscreen);

  file.write("[[window_rule]]\nhdr = \"sometimes\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].hdr);
}

UMBRIEL_TEST(windowRuleWorkspaceTargetPreservesIntegerAndStringSelectors) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\ndefault_workspace = 2\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& positionTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* position = positionTarget ? std::get_if<umbriel::WorkspaceIndex>(&*positionTarget) : nullptr;
  CHECK(position != nullptr);
  CHECK(position != nullptr && position->value == 2);
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = 64\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& limitTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* limit = limitTarget ? std::get_if<umbriel::WorkspaceIndex>(&*limitTarget) : nullptr;
  CHECK(limit != nullptr);
  CHECK(limit != nullptr && limit->value == 64);

  file.write("[[window_rule]]\ndefault_workspace = \"CHAT\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& nameTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* name = nameTarget ? std::get_if<umbriel::WorkspaceName>(&*nameTarget) : nullptr;
  CHECK(name != nullptr);
  CHECK(name != nullptr && name->value == "CHAT");

  // A numeric-looking string remains a name. It must not silently become a
  // positional selector during parsing.
  file.write("[[window_rule]]\ndefault_workspace = \"2\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& numericNameTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* numericName = numericNameTarget ? std::get_if<umbriel::WorkspaceName>(&*numericNameTarget) : nullptr;
  CHECK(numericName != nullptr);
  CHECK(numericName != nullptr && numericName->value == "2");

  file.write("[[window_rule]]\ndefault_workspace = \"\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = false\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());

  file.write("[[window_rule]]\ndefault_workspace = 0\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());

  file.write("[[window_rule]]\ndefault_workspace = 65\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
}

UMBRIEL_TEST(windowRuleDefaultScratchpadTargetsConfiguredInventory) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\ndefault_scratchpad = \"terminal\"\n[[scratchpad]]\nname = \"terminal\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].defaultScratchpad == "terminal");
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_scratchpad"));

  file.write("[[window_rule]]\ndefault_scratchpad = \"default\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].defaultScratchpad == "default");

  file.write("[[window_rule]]\ndefault_scratchpad = \"terminal\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultScratchpad);
  CHECK(store.config().windowRules[0].opacity == 0.5);
  CHECK(containsDiagnostic(store, "unknown scratchpad 'terminal'"));

  file.write("[[scratchpad]]\nname = \"terminal\"\n[[window_rule]]\ndefault_scratchpad = \"default\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultScratchpad);
  CHECK(containsDiagnostic(store, "unknown scratchpad 'default'"));

  file.write("[[scratchpad]]\nname = \"terminal\"\n[[window_rule]]\ndefault_scratchpad = \"missing\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultScratchpad);
  CHECK(store.config().windowRules[0].opacity == 0.5);

  file.write("[[window_rule]]\ndefault_scratchpad = \"\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultScratchpad);
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_scratchpad"));

  file.write("[[window_rule]]\ndefault_scratchpad = 1\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultScratchpad);
}

UMBRIEL_TEST(securityContextRulesLoadAndKeepTheManagerBlocked) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[[security_context_rule]]\n"
      "match.sandbox_engine = '^org\\.flatpak$'\n"
      "match.app_id = '^org\\.example\\.Bar$'\n"
      "allow_globals = [\"zwlr_layer_shell_v1\"]\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().securityContextRules.size(), size_t{1});
  CHECK(
      store.config().securityContextRules[0].sandboxEnginePatterns.patterns
      == std::vector<std::string>{"^org\\.flatpak$"}
  );
  CHECK(
      store.config().securityContextRules[0].appIdPatterns.patterns == std::vector<std::string>{"^org\\.example\\.Bar$"}
  );
  CHECK_EQ(store.config().securityContextRules[0].allowGlobals.size(), size_t{1});

  file.write("[[security_context_rule]]\nmatch.app_id = '['\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());

  // Listing the manager is stripped with a warning; the rest of the rule loads.
  file.write(
      "[[security_context_rule]]\nallow_globals = [\"wp_security_context_manager_v1\", \"zwlr_layer_shell_v1\"]\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().securityContextRules.size(), size_t{1});
  CHECK_EQ(store.config().securityContextRules[0].allowGlobals.size(), size_t{1});
  CHECK(store.config().securityContextRules[0].allowGlobals[0] == "zwlr_layer_shell_v1");
  CHECK(containsDiagnostic(store, "ignoring wp_security_context_manager_v1 in security_context_rule.allow_globals"));

  // A mistake rejects the rule rather than widening it to every restricted client.
  file.write("[[security_context_rule]]\nmatch.ap_id = 'org.example.Bar'\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "ignoring security_context_rule (unknown key in match)"));

  file.write("[[security_context_rule]]\nmatch.app_id = 'org.example.Bar'\nallow_glbals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "ignoring security_context_rule (unknown key)"));

  file.write("[[security_context_rule]]\nmatch.app_id = ''\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());

  file.write("[[security_context_rule]]\nmatch.sandbox_engine = 5\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());

  file.write("[[security_context_rule]]\nmatch.app_id = 'org.example.Bar'\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "ignoring security_context_rule (allow_globals is empty)"));

  // Only the manager listed leaves nothing to grant.
  file.write("[[security_context_rule]]\nallow_globals = [\"wp_security_context_manager_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "ignoring security_context_rule (allow_globals is empty)"));
}

UMBRIEL_TEST(windowContentTypeMatcherLoadsFixedVocabulary) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  const auto checkValue = [&](const std::string& value, ContentType expected) {
    file.write("[[window_rule]]\nmatch.content_type = \"" + value + "\"\nopacity = 0.9\n");
    CHECK(store.reload().success);
    CHECK_EQ(store.config().windowRules.size(), size_t{1});
    CHECK(store.config().windowRules[0].matchContentType == expected);
    CHECK(!containsDiagnostic(store, "unknown key window_rule.match.content_type"));
  };
  checkValue("none", ContentType::None);
  checkValue("photo", ContentType::Photo);
  checkValue("video", ContentType::Video);
  checkValue("game", ContentType::Game);

  file.write("[[window_rule]]\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].matchContentType);

  file.write("[[window_rule]]\nmatch.content_type = 42\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.content_type = \"stream\"\nmatch.is_focused = true\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.content_type = \"Game\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
}

UMBRIEL_TEST(windowStartupMatcherLoadsBoolean) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\nmatch.at_startup = true\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].matchAtStartup == true);
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.at_startup"));

  file.write("[[window_rule]]\nmatch.at_startup = false\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].matchAtStartup == false);

  file.write("[[window_rule]]\nmatch.at_startup = \"yes\"\nmatch.is_focused = true\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));
}

UMBRIEL_TEST(layerMatchersLoadLayerAndStartup) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[layer_rule]]\nmatch.layer = \"top\"\nmatch.at_startup = true\nblur = true\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().layerRules.size(), size_t{1});
  CHECK(store.config().layerRules[0].matchLayer == umbriel::LayerShellLayer::Top);
  CHECK(store.config().layerRules[0].matchAtStartup == true);
  CHECK(!containsDiagnostic(store, "unknown key layer_rule.match.layer"));
  CHECK(!containsDiagnostic(store, "unknown key layer_rule.match.at_startup"));

  // A layer selector that names no layer rejects the rule, as a wrong value would blur every layer instead.
  file.write("[[layer_rule]]\nmatch.layer = \"sideways\"\nblur = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().layerRules.empty());
}

UMBRIEL_TEST(windowStateMatchersLoadBooleans) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[[window_rule]]\nmatch.is_floating = true\nmatch.is_pinned = false\nmatch.is_scratchpad = true\nopacity = "
      "0.9\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].matchFloating == true);
  CHECK(store.config().windowRules[0].matchPinned == false);
  CHECK(store.config().windowRules[0].matchScratchpad == true);

  file.write("[[window_rule]]\nmatch.is_floating = \"yes\"\nmatch.is_pinned = 1\nmatch.is_scratchpad = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_floating"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_pinned"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_scratchpad"));
}

UMBRIEL_TEST(windowXdgTagMatcherLoadsRegexAndRejectsInvalidValues) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\nmatch.xdg_tag = \"^(game-launcher|game-running)$\"\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const umbriel::RegexPatterns& tag = store.config().windowRules[0].xdgTagPatterns;
  CHECK(tag.patterns == std::vector<std::string>{"^(game-launcher|game-running)$"});
  CHECK(std::regex_search("game-launcher", tag.regexes[0]));
  CHECK(std::regex_search("game-running", tag.regexes[0]));
  CHECK(!std::regex_search("game-settings", tag.regexes[0]));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.xdg_tag"));

  // An array of patterns is the same selector: any one of them matching is enough.
  file.write("[[window_rule]]\nmatch.xdg_tag = [\"^game-launcher$\", \"^game-running$\"]\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK_EQ(
      store.config().windowRules[0].xdgTagPatterns.patterns,
      (std::vector<std::string>{"^game-launcher$", "^game-running$"})
  );
  CHECK(std::regex_search("game-running", store.config().windowRules[0].xdgTagPatterns.regexes[1]));

  // An empty array places no constraint, like leaving the key out.
  file.write("[[window_rule]]\nmatch.app_id = []\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].appIdPatterns.patterns.empty());

  file.write("[[window_rule]]\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].xdgTagPatterns.patterns.empty());

  file.write("[[window_rule]]\nmatch.xdg_tag = 42\nmatch.is_focused = true\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.xdg_tag = \"[\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  // One pattern that does not compile drops the whole rule rather than the rest of the array.
  file.write("[[window_rule]]\nmatch.app_id = [\"^foot$\", \"[\"]\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  // An element that is not a string rejects the rule just as a bad regex does.
  file.write("[[window_rule]]\nmatch.app_id = [\"^foot$\", 42]\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(containsDiagnostic(store, "ignoring window_rule[0].match.app_id (expected array of strings)"));
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
}

UMBRIEL_TEST(windowRuleFloatingSizeTablesLoadIndependentAxesAndClamp) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[[window_rule]]\n"
      "match.app_id = \"^utility$\"\n"
      "default_floating = true\n"
      "default_floating_size = { width = 0.5, height = 0.6 }\n"
      "default_floating_size_px = { width = 640, height = 480 }\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& rule = store.config().windowRules[0];
  CHECK(rule.defaultFloatingWidth && *rule.defaultFloatingWidth == 0.5);
  CHECK(rule.defaultFloatingHeight && *rule.defaultFloatingHeight == 0.6);
  CHECK(rule.defaultFloatingWidthPx && *rule.defaultFloatingWidthPx == 640);
  CHECK(rule.defaultFloatingHeightPx && *rule.defaultFloatingHeightPx == 480);

  // Each axis is optional, and out-of-range fractions clamp independently.
  file.write("[[window_rule]]\ndefault_floating_size = { width = 3.0, height = 0.01 }\n");
  CHECK(store.reload().success);
  CHECK(
      store.config().windowRules[0].defaultFloatingWidth && *store.config().windowRules[0].defaultFloatingWidth == 1.0
  );
  CHECK(
      store.config().windowRules[0].defaultFloatingHeight && *store.config().windowRules[0].defaultFloatingHeight == 0.1
  );

  file.write("[[window_rule]]\ndefault_floating_size = { width = 0.5 }\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules[0].defaultFloatingWidth);
  CHECK(!store.config().windowRules[0].defaultFloatingHeight);

  file.write("[[window_rule]]\ndefault_floating_size = [0.5, 0.6]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].defaultFloatingWidth);
  CHECK(!store.config().windowRules[0].defaultFloatingHeight);

  file.write("[[window_rule]]\ndefault_floating_width = 0.5\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].defaultFloatingWidth);

  // Non-numeric values are ignored with a diagnostic.
  file.write("[[window_rule]]\ndefault_scrolling_extent = \"half\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].defaultScrollingExtent);
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

UMBRIEL_TEST(outputWorkspaceInventoryPreservesCountAndNames) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nworkspaces = 9\n[output.DP-2]\nworkspaces = [\"3\", \"CHAT\"]\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{2});
  if (store.config().outputs.size() == 2) {
    const auto& count = store.config().outputs[0].workspaces;
    const auto& names = store.config().outputs[1].workspaces;
    CHECK(count.has_value());
    CHECK(names.has_value());
    CHECK(count && std::get_if<size_t>(&*count) != nullptr);
    CHECK(count && std::get_if<size_t>(&*count) != nullptr && *std::get_if<size_t>(&*count) == 9);
    const auto* values = names ? std::get_if<std::vector<std::string>>(&*names) : nullptr;
    CHECK(values != nullptr);
    CHECK(values != nullptr && *values == std::vector<std::string>({"3", "CHAT"}));
  }
}

UMBRIEL_TEST(outputMinWorkspacesLoadsAndRequiresDynamicWorkspaces) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\nmin_workspaces = 4\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK_EQ(store.config().outputs[0].minWorkspaces, 4);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].minWorkspaces, 1);

  file.write("[output.DP-1]\nworkspaces = \"dynamic\"\nmin_workspaces = 3\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].minWorkspaces, 3);

  file.write("[output.DP-1]\nmin_workspaces = 99\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs[0].minWorkspaces, 64);
  CHECK(containsDiagnostic(store, "output.DP-1.min_workspaces = 99 out of range, clamped to 64"));

  file.write("[output.DP-1]\nworkspaces = [\"dev\", \"web\"]\nmin_workspaces = 3\n");
  CHECK(!store.reload().success);
  CHECK(containsDiagnostic(store, "output.DP-1.min_workspaces requires dynamic workspaces"));
  CHECK(!containsDiagnostic(store, "unknown key output.DP-1.min_workspaces"));
}

// The wrap switch is an ordinary per-output boolean: it defaults off, survives a
// reload without the key, and rejects a non-boolean the way its neighbours do.
UMBRIEL_TEST(outputCyclicWorkspacesLoadsAndDefaultsOff) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[output.DP-1]\ncyclic_workspaces = true\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].cyclicWorkspaces);

  file.write("[output.DP-1]\ncyclic_workspaces = false\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].cyclicWorkspaces);

  file.write("[output.DP-1]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].cyclicWorkspaces);

  file.write("[output.DP-1]\ncyclic_workspaces = \"yes\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].cyclicWorkspaces);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.cyclic_workspaces (expected boolean)"));
}

UMBRIEL_TEST(dynamicNamedWorkspaceDeclarationsReserveEmptySentinelCapacity) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  const auto declarations = [](size_t count, std::string_view prefix, std::string_view output = {}) {
    std::string text;
    for (size_t index = 0; index < count; ++index) {
      text += "[[workspace]]\nname = \"" + std::string(prefix) + std::to_string(index) + "\"\n";
      if (!output.empty()) {
        text += "output = \"" + std::string(output) + "\"\n";
      }
    }
    return text;
  };

  file.write("[output.DP-1]\nworkspaces = \"dynamic\"\n\n[[workspace]]\nname = \"chat\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});

  file.write(declarations(63, "global-"));
  CHECK(store.reload().success);

  file.write(declarations(64, "global-"));
  CHECK(!store.reload().success);
  CHECK(containsDiagnostic(store, "exceeds the limit of 63 named workspaces for unscoped dynamic outputs"));

  file.write("[workspaces]\nempty_above = true\n\n" + declarations(63, "global-"));
  CHECK(!store.reload().success);
  CHECK(containsDiagnostic(store, "exceeds the limit of 62 named workspaces for unscoped dynamic outputs"));

  file.write(declarations(63, "left-", "DP-1") + declarations(63, "right-", "DP-2"));
  CHECK(store.reload().success);
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

UMBRIEL_TEST(unknownIncludeKeysRejectReload) {
  // The merge erases `include` before the config readers run, so the merge is the only place that can report a typo in
  // this section. Every file's own `include` table is checked, not just the root's.
  const TempConfig file;
  file.write("[include]\nfiles = [\"" + file.includeName() + "\"]\ndirs = [\"themes\"]\n");
  file.writeInclude("[include]\npaths = []\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::Config previous = store.config();
  const uint64_t generation = store.generation();
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(!loaded.success);
  CHECK(store.config() == previous);
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "unknown key include.dirs"));
  CHECK(containsDiagnostic(store, "unknown key include.paths"));
  CHECK(!containsDiagnostic(store, "unknown key include.files"));
}

UMBRIEL_TEST(missingOptionalIncludesAreSilentWatchedAndLoadWhenCreated) {
  const TempConfigTree tree;
  const std::filesystem::path optional = tree.path("generated/colors.toml");
  tree.write("config.toml", "[include.optional]\nfiles = [\"generated/colors.toml\"]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  const umbriel::ConfigReloadResult missing = store.reload();

  CHECK(missing.success);
  CHECK(!store.missingIncludes());
  CHECK(!containsDiagnostic(store, "include not found"));
  CHECK(std::ranges::find(store.watchPaths(), optional) != store.watchPaths().end());

  tree.write("generated/colors.toml", "[colors]\naccent_primary = \"#123456FF\"\n");
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK(!store.missingIncludes());
  CHECK(!containsDiagnostic(store, "include not found"));
  CHECK_EQ(store.config().colors.accentPrimary[0], 18.0F / 255.0F);
}

UMBRIEL_TEST(optionalIncludesExpandPathsAndApplyAfterRequiredIncludes) {
  const TempConfigTree tree;
  const ScopedEnvironment home("HOME", tree.path("home").string());
  const ScopedEnvironment generated("UMBRIEL_OPTIONAL_INCLUDE", tree.path("generated.toml").string());
  tree.write("required.toml", "[layout]\ngap = 11\n");
  tree.write("generated.toml", "[layout]\ngap = 22\n");
  tree.write("home/override.toml", "[layout]\ngap = 33\n");
  tree.write(
      "config.toml",
      "[include]\nfiles = [\"required.toml\"]\n"
      "[include.optional]\nfiles = [\"$UMBRIEL_OPTIONAL_INCLUDE\", \"~/override.toml\"]\n"
  );

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK_EQ(store.config().layout.gap, 33);
  CHECK(!containsDiagnostic(store, "unknown key include.optional"));
}

UMBRIEL_TEST(malformedOptionalIncludeRejectsReload) {
  const TempConfigTree tree;
  tree.write(
      "config.toml",
      "[layout]\ngap = 7\n"
      "[include.optional]\nfiles = [\"generated.toml\"]\n"
  );

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const uint64_t generation = store.generation();

  tree.write("generated.toml", "[layout\n");
  const umbriel::ConfigReloadResult malformed = store.reload();

  CHECK(!malformed.success);
  CHECK_EQ(store.generation(), generation);
  CHECK_EQ(store.config().layout.gap, 7);
  CHECK(!store.diagnostics().empty());
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

UMBRIEL_TEST(ruleCollectionsAccumulateAcrossIncludesWhilePlainArraysReplace) {
  // Rules are a collection every file contributes to, so an include and the including file both apply, in merge
  // order. Every other array is one value: appending would grow a fixed-arity array past what its reader accepts and
  // would make an overridden autostart list run the include's commands as well.
  const TempConfigTree tree;
  tree.write(
      "rules.toml",
      "[general]\nautostart = [\"from-include\"]\n"
      "[output.DP-1]\nposition = [0, 0]\n"
      "[[window_rule]]\nmatch.app_id = \"^from-include$\"\n"
      "[[layer_rule]]\nmatch.namespace = \"^bar$\"\nblur = true\n"
  );
  tree.write(
      "config.toml",
      "[include]\nfiles = [\"rules.toml\"]\n"
      "[general]\nautostart = [\"from-root\"]\n"
      "[output.DP-1]\nposition = [3072, 0]\n"
      "[[window_rule]]\nmatch.app_id = \"^from-root$\"\n"
  );

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  std::vector<std::string> patterns;
  for (const umbriel::WindowRule& rule : store.config().windowRules) {
    for (const std::string& pattern : rule.appIdPatterns.patterns) {
      patterns.push_back(pattern);
    }
  }
  std::vector<std::string> namespaces;
  for (const umbriel::LayerRule& rule : store.config().layerRules) {
    for (const std::string& pattern : rule.namespacePatterns.patterns) {
      namespaces.push_back(pattern);
    }
  }
  const std::vector<std::string> expectedPatterns{"^from-include$", "^from-root$"};
  const std::vector<std::string> expectedNamespaces{"^bar$"};
  const std::vector<std::string> expectedAutostart{"from-root"};
  const std::array<int, 2> expectedPosition{3072, 0};
  const auto output =
      std::ranges::find_if(store.config().outputs, [](const umbriel::OutputRule& rule) { return rule.name == "DP-1"; });
  const bool foundOutput = output != store.config().outputs.end();

  CHECK(loaded.success);
  CHECK(patterns == expectedPatterns);
  CHECK(namespaces == expectedNamespaces);
  CHECK(store.config().general.autostart == expectedAutostart);
  CHECK(foundOutput && output->position.has_value());
  CHECK(foundOutput && output->position.value_or(std::array<int, 2>{}) == expectedPosition);
  CHECK(!containsDiagnostic(store, "position"));
}

// Decoration keys are read from a window rule like every other effect key, so a
// per-window frame, corner radius, and shadow reach the resolve path.
UMBRIEL_TEST(windowRuleDecorationKeysAreRead) {
  const TempConfig file;
  file.write(
      "[[window_rule]]\n"
      "match.app_id = \"^csd-app$\"\n"
      "border_width = 0\n"
      "outer_border_width = 6\n"
      "corner_radius = 0\n"
      "shadow = false\n"
  );

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  if (store.config().windowRules.empty()) {
    return;
  }
  const umbriel::WindowRule& rule = store.config().windowRules.front();
  CHECK(rule.borderWidth && *rule.borderWidth == 0);
  CHECK(rule.outerBorderWidth && *rule.outerBorderWidth == 6);
  CHECK(rule.cornerRadius && *rule.cornerRadius == 0);
  CHECK(rule.shadow && !*rule.shadow);
  CHECK(store.diagnostics().empty());
}

UMBRIEL_TEST(emptyRuleArrayDropsRulesFromIncludes) {
  const TempConfig file;
  file.write("window_rule = []\n[include]\nfiles = [\"" + file.includeName() + "\"]\n");
  file.writeInclude("[[window_rule]]\nmatch.app_id = \"^dropped$\"\ndefault_floating = true\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult loaded = store.reload();

  CHECK(loaded.success);
  CHECK(store.config().windowRules.empty());
}

UMBRIEL_TEST(duplicateDeviceRuleAcrossIncludesIsRejected) {
  // Device rules accumulate like any other collection, so restating one in a later file is the same duplicate the
  // reader already rejects within a single file, and the whole reload is refused rather than silently dropping the
  // include's other rules.
  const TempConfigTree tree;
  tree.write("input.toml", "[[input.device]]\nname = \"Acme Keyboard\"\nrepeat_rate = 40\n");
  tree.write(
      "config.toml",
      "[include]\nfiles = [\"input.toml\"]\n"
      "[[input.device]]\nname = \"Acme Mouse\"\nsensitivity = -0.5\n"
  );

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.devices.size(), size_t{2});
  CHECK(store.config().input.findDevice("Acme Keyboard") != nullptr);
  CHECK(store.config().input.findDevice("Acme Mouse") != nullptr);

  tree.write(
      "config.toml",
      "[include]\nfiles = [\"input.toml\"]\n"
      "[[input.device]]\nname = \"Acme Keyboard\"\nrepeat_rate = 60\n"
  );
  const umbriel::ConfigReloadResult duplicate = store.reload();

  CHECK(!duplicate.success);
  CHECK(containsDiagnostic(store, "duplicates device 'Acme Keyboard'"));
  CHECK_EQ(store.config().input.devices.size(), size_t{2});
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
default_scrolling_column = "browser-stack"
default_scrolling_column_order = 20
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
  CHECK(store.config().windowRules[0].defaultScrollingColumn == "browser-stack");
  CHECK(store.config().windowRules[0].defaultScrollingColumnOrder == 20);
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

UMBRIEL_TEST(screencastDynamicConfirmationDefaultsOnAndCanBeDisabled) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[screencast]\ndisable_dynamic_confirmation = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().screenCast.disableDynamicConfirmation);

  file.write("[screencast]\n");
  CHECK(store.reload().success);
  CHECK(!store.config().screenCast.disableDynamicConfirmation);
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
left_handed = true
accel_profile = "adaptive"
sensitivity = 0.1
scroll_factor = { horizontal = 0.8, vertical = 0.6 }
disable_while_typing = true
disable_on_external_mouse = true
click_method = "button_areas"
tap_button_map = "left_middle_right"

[input.mouse]
left_handed = true
accel_profile = "custom 0.2 0.0 0.5 1.0 2.0"
sensitivity = 0.25
scroll_button = "MouseForward"
scroll_button_lock = true

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
left_handed = false
accel_profile = "flat"
sensitivity = -0.5
disable_while_typing = false
click_method = "clickfinger"
tap_button_map = "left_right_middle"

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = -0.5
scroll_button = "MouseBack"
scroll_button_lock = false
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
  CHECK(input.mouse.scrollButton == std::optional<uint32_t>(BTN_EXTRA));
  CHECK(input.mouse.scrollButtonLock == std::optional<bool>(true));
  CHECK(input.mouse.leftHanded == std::optional<bool>(true));
  CHECK(input.touchpad.accelProfile.has_value());
  if (input.touchpad.accelProfile.has_value()) {
    CHECK(input.touchpad.accelProfile->kind == umbriel::AccelProfile::Kind::Adaptive);
  }
  CHECK(input.touchpad.sensitivity == std::optional<double>(0.1));
  CHECK(input.touchpad.scrollFactor.has_value());
  CHECK(input.touchpad.scrollFactor->horizontal == std::optional<double>(0.8));
  CHECK(input.touchpad.scrollFactor->vertical == std::optional<double>(0.6));
  CHECK(input.touchpad.disableWhileTyping == std::optional<bool>(true));
  CHECK(input.touchpad.disableOnExternalMouse == std::optional<bool>(true));
  CHECK(input.touchpad.clickMethod == std::optional(umbriel::ClickMethod::ButtonAreas));
  CHECK(input.touchpad.tapButtonMap == std::optional(umbriel::TapButtonMap::LeftMiddleRight));
  CHECK(input.touchpad.leftHanded == std::optional<bool>(true));
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
    CHECK(touchpad->leftHanded == std::optional<bool>(false));
    CHECK(touchpad->accelProfile.has_value());
    if (touchpad->accelProfile.has_value()) {
      CHECK(touchpad->accelProfile->kind == umbriel::AccelProfile::Kind::Flat);
    }
    CHECK(touchpad->sensitivity == std::optional<double>(-0.5));
    CHECK(touchpad->disableWhileTyping == std::optional<bool>(false));
    CHECK(touchpad->clickMethod == std::optional(umbriel::ClickMethod::ClickFinger));
    CHECK(touchpad->tapButtonMap == std::optional(umbriel::TapButtonMap::LeftRightMiddle));
  }

  const auto* mouse = input.findDevice("Acme Gaming Mouse");
  CHECK(mouse != nullptr);
  if (mouse != nullptr) {
    CHECK(mouse->accelProfile.has_value());
    CHECK(mouse->accelProfile->kind == umbriel::AccelProfile::Kind::Flat);
    CHECK(mouse->sensitivity == std::optional<double>(-0.5));
    CHECK(!mouse->clickMethod.has_value());
    CHECK(mouse->scrollButton == std::optional<uint32_t>(BTN_SIDE));
    CHECK(mouse->scrollButtonLock == std::optional<bool>(false));
    CHECK(!mouse->leftHanded.has_value());
  }

  CHECK(input.findDevice("acme split keyboard") == nullptr);
  CHECK(input.findDevice("Acme") == nullptr);
}

UMBRIEL_TEST(mouseAccelerationPreservesDeviceProfileByDefault) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.mouse.accelProfile.has_value());
  CHECK_EQ(defaults.input.mouse.sensitivity, 0.0);
}

UMBRIEL_TEST(mouseScrollButtonDefaultsToUnset) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.mouse.scrollButton.has_value());
  CHECK(!defaults.input.mouse.scrollButtonLock.has_value());
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

UMBRIEL_TEST(touchpadDisableOnExternalMouseDefaultsToUnset) {
  const umbriel::Config defaults;
  CHECK(!defaults.input.touchpad.disableOnExternalMouse.has_value());
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

UMBRIEL_TEST(invalidClickMethodIsRejectedAndStillClaimsTheKey) {
  const TempConfig file;
  file.write(R"(
[input.touchpad]
click_method = "button-areas"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.touchpad.clickMethod.has_value());
  CHECK(!containsDiagnostic(store, "unknown key input.touchpad.click_method"));
}

UMBRIEL_TEST(invalidTapButtonMapIsRejectedAndStillClaimsTheKey) {
  const TempConfig file;
  file.write(R"(
[input.touchpad]
tap_button_map = "lmr"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.touchpad.tapButtonMap.has_value());
  CHECK(!containsDiagnostic(store, "unknown key input.touchpad.tap_button_map"));
}

UMBRIEL_TEST(scrollButtonRejectsEvdevCodesAndStillClaimsTheKey) {
  const TempConfig file;
  file.write(R"(
[input.mouse]
scroll_button = 275
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(!store.config().input.mouse.scrollButton.has_value());
  CHECK(!containsDiagnostic(store, "unknown key input.mouse.scroll_button"));

  file.write("[input.mouse]\nscroll_button = \"button8\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().input.mouse.scrollButton.has_value());
}

UMBRIEL_TEST(scrollButtonReportsBindsItTakesOver) {
  const TempConfig file;
  file.write(R"(
[input.mouse]
scroll_button = "MouseBack"

[keybinds]
"Mod+MouseBack" = "overview-toggle"
"Mod+MouseForward" = "overview-close"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().input.mouse.scrollButton == std::optional<uint32_t>(BTN_SIDE));
  CHECK(containsDiagnostic(store, "input.mouse.scroll_button claims MouseBack for scrolling"));

  file.write(R"(
[input.mouse]
scroll_button = "MouseBack"

[keybinds]
"Mod+MouseForward" = "overview-close"
)");
  CHECK(store.reload().success);
  CHECK(!containsDiagnostic(store, "claims MouseBack for scrolling"));
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

UMBRIEL_TEST(keyboardTrackLayoutLoadsAndRejectsUnknownValues) {
  const TempConfig file;
  file.write("[input.keyboard]\ntrack_layout = \"window\"\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.keyboard.trackLayout, TrackLayout::Window);
  CHECK(!containsDiagnostic(store, "unknown key input.keyboard.track_layout"));

  file.write("[input.keyboard]\ntrack_layout = \"surface\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().input.keyboard.trackLayout, TrackLayout::Global);
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

UMBRIEL_TEST(animationEffectsResolveIncludedPresetsAcrossAllEventsAndTrackContentChanges) {
  const TempConfigTree tree;
  const std::array sections{"windows_in", "windows_out", "windows_move",  "workspaces", "overview",
                            "scratchpad", "border",      "dim_unfocused", "layers"};
  std::string theme = "[effects.preset.reveal]\nkind = 'animation'\nshader = 'effect.glsl'\n";
  for (const char* section : sections) {
    theme += std::format("[animation.{}]\neffect = 'reveal'\n", section);
  }
  tree.write("config.toml", "[include]\nfiles = ['theme/animation.toml']\n");
  tree.write("theme/animation.toml", theme);
  tree.write("theme/effect.glsl", "first shader");
  tree.write("effect.glsl", "wrong source directory");
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& animation = store.config().animation;
  const std::array effects{&animation.windowsIn.effect,  &animation.windowsOut.effect,   &animation.windowsMove.effect,
                           &animation.workspaces.effect, &animation.overview.effect,     &animation.scratchpad.effect,
                           &animation.border.effect,     &animation.dimUnfocused.effect, &animation.layers.effect};
  for (const auto* effect : effects) {
    CHECK_EQ(*effect, std::string("reveal"));
  }
  const umbriel::EffectPreset* reveal = umbriel::findEffectPreset(store.config().effects, "reveal");
  CHECK(reveal != nullptr);
  if (reveal != nullptr) {
    CHECK_EQ(reveal->shader.code, std::string("first shader"));
    CHECK(reveal->shader.file == tree.path("theme/effect.glsl"));
  }
  CHECK(!containsDiagnostic(store, "unknown key"));
  CHECK_EQ(std::ranges::count(store.watchPaths(), tree.path("theme/effect.glsl")), 1);

  tree.write("theme/effect.glsl", "edited shader");
  const auto edited = store.reload();
  CHECK(edited.success);
  CHECK(edited.effects.effects);
  CHECK(!edited.effects.animation);
  reveal = umbriel::findEffectPreset(store.config().effects, "reveal");
  CHECK(reveal != nullptr && reveal->shader.code == "edited shader");

  tree.write("theme/replacement.glsl", "replacement shader");
  tree.write(
      "theme/animation.toml",
      "[effects.preset.reveal]\nkind = 'animation'\nshader = 'replacement.glsl'\n[animation.windows_in]\neffect = "
      "'reveal'\n"
  );
  CHECK(store.reload().success);
  CHECK(std::ranges::find(store.watchPaths(), tree.path("theme/effect.glsl")) == store.watchPaths().end());
  CHECK(store.config().animation.layers.effect.empty());
  CHECK_EQ(store.config().animation.windowsIn.effect, std::string("reveal"));
  reveal = umbriel::findEffectPreset(store.config().effects, "reveal");
  CHECK(reveal != nullptr && reveal->shader.code == "replacement shader");
}

UMBRIEL_TEST(removedAnimationShaderKeyIsUnknown) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[animation.windows_in]\nshader = \"reveal.glsl\"\n");
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "unknown key animation.windows_in.shader"));
  CHECK(store.config().animation.windowsIn.effect.empty());
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
style = "popin"
scale = 0.6

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
  CHECK_EQ(animation.windowsOut.style, std::string{"popin"});
  CHECK_EQ(animation.windowsOut.scale, 0.6);
  CHECK(!animation.overview.enabled);
  CHECK_EQ(animation.overview.durationMs, 700);
  CHECK(animation.overview.curve.easing == umbriel::Easing::CustomBezier);
  // The shared curve reaches every duration-based event, but the filmstrip settle keeps its spring until asked.
  CHECK(animation.overview.workspaceCurve.easing == umbriel::Easing::Spring);
  CHECK_EQ(animation.overview.workspaceCurve.spring.stiffness, 1000.0);
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
WLR_DRM_DEVICES = "/dev/dri/card0"
WLR_RENDER_DRM_DEVICE = "/dev/dri/renderD128"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().environment.variables.size(), size_t{4});
  CHECK(
      std::ranges::find(store.config().environment.variables, std::pair{std::string{"DXVK_HDR"}, std::string{"1"}})
      != store.config().environment.variables.end()
  );
  CHECK(
      std::ranges::find(store.config().environment.variables, std::pair{std::string{"_PRIVATE"}, std::string{"kept"}})
      != store.config().environment.variables.end()
  );
  CHECK(
      std::ranges::find(
          store.config().environment.variables, std::pair{std::string{"WLR_DRM_DEVICES"}, std::string{"/dev/dri/card0"}}
      )
      != store.config().environment.variables.end()
  );
  CHECK(
      std::ranges::find(
          store.config().environment.variables,
          std::pair{std::string{"WLR_RENDER_DRM_DEVICE"}, std::string{"/dev/dri/renderD128"}}
      )
      != store.config().environment.variables.end()
  );
  CHECK(containsDiagnostic(store, R"(ignoring environment key "9INVALID" (expected [A-Za-z_][A-Za-z0-9_]*))"));
  CHECK(containsDiagnostic(store, R"(ignoring environment key "HAS-HYPHEN" (expected [A-Za-z_][A-Za-z0-9_]*))"));
  CHECK(containsDiagnostic(store, "ignoring environment.NOT_A_STRING (expected string)"));
  CHECK(containsDiagnostic(store, "ignoring environment.WAYLAND_DISPLAY (reserved by Umbriel)"));
  CHECK(!containsDiagnostic(store, "unknown key environment.DXVK_HDR"));
  CHECK(!containsDiagnostic(store, "unknown key environment._PRIVATE"));
}

UMBRIEL_TEST(drmConfigurationLoadsAndNormalizesSelectors) {
  const TempConfig file;
  file.write(R"(
[drm]
ignored_devices = [
  "/dev/dri/by-path/pci-0000:01:00.0-card",
  "/dev/dri/by-path/pci-0000:01:00.0-card",
]
ignored_pci_addresses = ["0000:01:00.0", "0000:01:00.0"]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().drm.configured());
  CHECK_EQ(store.config().drm.ignoredDevices.size(), size_t{1});
  CHECK_EQ(store.config().drm.ignoredPciAddresses, std::vector<std::string>{"0000:01:00.0"});
  CHECK(containsDiagnostic(store, "ignoring duplicate drm.ignored_devices"));
  CHECK(containsDiagnostic(store, "ignoring duplicate drm.ignored_pci_addresses"));

  file.write(R"(
[drm]
ignored_pci_addresses = ["0000:AB:0C.7"]
)");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().drm.ignoredPciAddresses, std::vector<std::string>{"0000:ab:0c.7"});
}

UMBRIEL_TEST(emptyDrmTableKeepsTheCompatibilityPath) {
  const TempConfig file;
  file.write("[drm]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  CHECK(store.reload().success);
  CHECK(!store.config().drm.configured());
}

UMBRIEL_TEST(drmPathsPreserveSymlinkSensitiveComponents) {
  const TempConfig file;
  file.write(R"(
[drm]
ignored_devices = ["/dev/dri/excluded/../card0"]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  CHECK(store.reload().success);
  CHECK_EQ(store.config().drm.ignoredDevices, std::vector<std::string>{"/dev/dri/excluded/../card0"});
}

UMBRIEL_TEST(drmConfigurationRejectsUnsafeSelectors) {
  const TempConfig file;
  file.write(R"(
[drm]
ignored_pci_addresses = ["0000:01:00.0"]
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  const umbriel::Config previous = store.config();

  file.write(R"(
[drm]
ignored_devices = [1, "relative-card"]
ignored_pci_addresses = ["01:00.0", "0000:01:00.8", "0000:01:20.0"]
render_device = "/dev/dri/renderD128"
surprise = true
)");

  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(!result.success);
  CHECK(store.config() == previous);
  CHECK(containsDiagnostic(store, "drm.ignored_devices must be a string"));
  CHECK(containsDiagnostic(store, "drm.ignored_devices must be an absolute path"));
  CHECK(containsDiagnostic(store, "invalid drm.ignored_pci_addresses entry"));
  CHECK(containsDiagnostic(store, "unknown key drm.render_device"));
  CHECK(containsDiagnostic(store, "unknown key drm.surprise"));
}

UMBRIEL_TEST(initialConfigErrorsDoNotCommitDefaults) {
  const TempConfig file;
  file.write("[drm]\nignored_pci_addresses = [\"invalid\"]\n");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();
  const umbriel::Config previous = store.config();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(store.config() == previous);
  CHECK(containsDiagnostic(store, "invalid drm.ignored_pci_addresses entry"));
}

UMBRIEL_TEST(initialNonDrmErrorsKeepCompatibilityDefaults) {
  const TempConfig file;
  file.write("workspace = \"invalid\"\n");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation + 1);
  CHECK(!store.config().drm.configured());
  CHECK(containsDiagnostic(store, "workspace must be a [[workspace]] array of tables"));
}

UMBRIEL_TEST(initialSyntaxErrorsFailClosedEvenWithoutRecognizableDrmPolicy) {
  const TempConfig file;
  file.write("workspace =\n");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
}

UMBRIEL_TEST(initialErrorsCannotDiscardRequestedDrmPolicy) {
  const TempConfig file;
  file.write(R"(
workspace = "invalid"

[drm]
ignored_pci_addresses = ["0000:01:00.0"]
)");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "workspace must be a [[workspace]] array of tables"));
}

UMBRIEL_TEST(initialSyntaxErrorsCannotDiscardRequestedDrmPolicy) {
  const TempConfig file;
  file.write(R"(
[drm]
ignored_devices = ["/dev/dri/card0"
)");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "Error while parsing array"));
}

UMBRIEL_TEST(initialIncludedSyntaxErrorsCannotDiscardRequestedDrmPolicy) {
  const TempConfig file;
  file.write("[include]\nfiles = [\"" + file.includeName() + "\"]\n");
  file.writeInclude(R"(
[drm]
ignored_pci_addresses = ["0000:01:00.0"
)");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "Error while parsing array"));
}

UMBRIEL_TEST(invalidIncludeDirectivesCannotDiscardDrmPolicy) {
  const TempConfigTree tree;
  const std::filesystem::path root = tree.path("config.toml");
  tree.write("hardware.toml", "[drm]\nignored_pci_addresses = [\"0000:01:00.0\"]\n");
  tree.write("config.toml", "[include]\nfiles = [\"hardware.toml\"]\n");

  ConfigStore& store = umbriel::configStore();
  CHECK(store.load(root.c_str()));
  const umbriel::Config previous = store.config();
  const uint64_t generation = store.generation();

  constexpr std::array invalidIncludes{
      "include = [\"hardware.toml\"]\n",
      "[include]\nfiles = \"hardware.toml\"\n",
      "[include]\nfiles = [\"hardware.toml\", 42]\n",
      "[include]\nfile = [\"hardware.toml\"]\n",
      "[include]\nfiles = [\"hardware.toml\\u0000missing\"]\n",
  };
  for (const char* include : invalidIncludes) {
    tree.write("config.toml", include);

    CHECK(!store.load(root.c_str()));
    CHECK_EQ(store.generation(), generation);
    CHECK(store.config() == previous);
    CHECK(std::ranges::any_of(store.diagnostics(), [](const ConfigDiagnostic& diagnostic) {
      return diagnostic.severity == ConfigDiagnostic::Severity::Error;
    }));
    CHECK(!store.reload().success);
    CHECK_EQ(store.generation(), generation);
    CHECK(store.config() == previous);
  }
}

UMBRIEL_TEST(initialUnreadableConfigCannotSilentlyDiscardDrmPolicy) {
  const TempConfig file;
  file.write("[drm]\nignored_devices = [\"/dev/dri/card0\"]\n");
  std::filesystem::permissions(file.path(), std::filesystem::perms::none);
  CHECK(access(file.path().c_str(), R_OK) != 0);

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "File could not be opened for reading"));
}

UMBRIEL_TEST(initialInaccessibleConfigCannotSilentlyDiscardDrmPolicy) {
  const TempConfigTree tree;
  tree.write("restricted/config.toml", "[drm]\nignored_devices = [\"/dev/dri/card0\"]\n");
  const std::filesystem::path restricted = tree.path("restricted");
  const std::filesystem::path configPath = restricted / "config.toml";
  std::filesystem::permissions(restricted, std::filesystem::perms::none);

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();
  const bool loaded = store.load(configPath.c_str());
  std::filesystem::permissions(restricted, std::filesystem::perms::owner_all);

  CHECK(!loaded);
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "cannot inspect config file"));
}

UMBRIEL_TEST(defaultConfigLookupDoesNotSkipInaccessibleUserPolicy) {
  const TempConfigTree tree;
  const std::filesystem::path userHome = tree.path("user");
  const std::filesystem::path userConfig = userHome / "umbriel/config.toml";
  const std::filesystem::path systemDir = tree.path("system");
  tree.write("user/umbriel/config.toml", "[drm]\nignored_devices = [\"/dev/dri/card0\"]\n");
  tree.write("system/umbriel/config.toml", "[layout]\ngap = 17\n");
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", userHome.string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", systemDir.string());
  std::filesystem::permissions(userConfig.parent_path(), std::filesystem::perms::none);

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();
  const bool loaded = store.load(nullptr);
  std::filesystem::permissions(userConfig.parent_path(), std::filesystem::perms::owner_all);

  CHECK(!loaded);
  CHECK_EQ(store.rootPath(), userConfig);
  CHECK_EQ(store.generation(), generation);
  CHECK(containsDiagnostic(store, "cannot inspect config file"));
}

UMBRIEL_TEST(missingPolicyIncludeRequiresRootDrmIntentMarker) {
  const TempConfig file;
  file.write("[include]\nfiles = [\"" + file.includeName() + "\"]\n\n[drm]\n");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(store.missingIncludes());
  CHECK(containsDiagnostic(store, "cannot safely load DRM policy while an include is missing"));
}

UMBRIEL_TEST(missingOptionalPolicyIncludeRequiresRootDrmIntentMarker) {
  const TempConfig file;
  file.write("[include.optional]\nfiles = [\"" + file.includeName() + "\"]\n\n[drm]\n");

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(!store.missingIncludes());
  CHECK(containsDiagnostic(store, "cannot safely load DRM policy while an include is missing"));
}

UMBRIEL_TEST(inaccessibleIncludeCannotBeTreatedAsMissing) {
  const TempConfigTree tree;
  tree.write("config.toml", "[include]\nfiles = [\"restricted/hardware.toml\"]\n");
  tree.write("restricted/hardware.toml", "[drm]\nignored_devices = [\"/dev/dri/card0\"]\n");
  const std::filesystem::path restricted = tree.path("restricted");
  std::filesystem::permissions(restricted, std::filesystem::perms::none);

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();
  const bool loaded = store.load(tree.path("config.toml").c_str());
  std::filesystem::permissions(restricted, std::filesystem::perms::owner_all);

  CHECK(!loaded);
  CHECK_EQ(store.generation(), generation);
  CHECK(!store.missingIncludes());
  CHECK(containsDiagnostic(store, "cannot inspect included config file"));
}

UMBRIEL_TEST(initialMissingExplicitConfigFailsWithoutCommittingDefaults) {
  const TempConfig file;
  std::filesystem::remove(file.path());

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();
  const umbriel::Config previous = store.config();

  CHECK(!store.load(file.path().c_str()));
  CHECK_EQ(store.generation(), generation);
  CHECK(store.config() == previous);
  CHECK(containsDiagnostic(store, "config file not found"));
}

UMBRIEL_TEST(eventsLoadCanonicalLidCommands) {
  const TempConfig file;
  file.write(R"(
[events]
lid_close = "systemctl suspend"
lid_open = "notify-send awake"
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK_EQ(store.config().events.lidClose, std::string{"systemctl suspend"});
  CHECK_EQ(store.config().events.lidOpen, std::string{"notify-send awake"});
  CHECK(store.diagnostics().empty());
}

UMBRIEL_TEST(packagedAnimationDefaultsMatchCompiledDefaults) {
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(UMBRIEL_EXAMPLE_CONFIG, true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().animation == umbriel::Config{}.animation);
}

UMBRIEL_TEST(effectPresetsClaimOnlyTheirKindsKeys) {
  const TempConfigTree tree;
  tree.write("pulse.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("glow.glsl", "vec4 cursor(vec2 uv) { return umbriel_sample(uv); }");
  tree.write(
      "config.toml",
      "[effects]\nmax_fps = 60\nin_capture = true\n"
      "[effects.preset.pulse]\nkind = \"border\"\nshader = \"pulse.glsl\"\npadding = 12\nspeed = 2.5\nanimated = "
      "false\n"
      "palette = true\nradius = 5\n"
      "[effects.preset.pulse.light]\nspread = 40\nintensity = 2\nthreshold = 0.25\n"
      "[effects.preset.glow]\nkind = \"cursor\"\nshader = \"glow.glsl\"\nradius = 96\npadding = 3\n"
  );
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& effects = store.config().effects;
  CHECK_EQ(effects.maxFps, 60);
  CHECK(effects.inCapture);
  CHECK_EQ(effects.presets.size(), size_t{2});
  const umbriel::EffectPreset* pulse = umbriel::findEffectPreset(effects, "pulse");
  CHECK(pulse != nullptr);
  if (pulse != nullptr) {
    CHECK(pulse->kind == umbriel::EffectKind::Border);
    CHECK_EQ(pulse->padding, 12);
    CHECK(pulse->speed == 2.5F);
    CHECK(!pulse->animated);
    CHECK(pulse->palette);
    CHECK(pulse->light.has_value());
    if (pulse->light) {
      CHECK_EQ(pulse->light->spread, 40);
      CHECK(pulse->light->intensity == 2.0F);
      CHECK(pulse->light->threshold == 0.25F);
    }
    CHECK(pulse->shader.file == tree.path("pulse.glsl"));
    CHECK(!pulse->inert());
  }
  const umbriel::EffectPreset* glow = umbriel::findEffectPreset(effects, "glow");
  CHECK(glow != nullptr);
  if (glow != nullptr) {
    CHECK_EQ(glow->radius, 96);
  }
  CHECK(containsDiagnostic(store, "unknown key effects.preset.pulse.radius"));
  CHECK(containsDiagnostic(store, "unknown key effects.preset.glow.padding"));
  CHECK_EQ(std::ranges::count(store.watchPaths(), tree.path("pulse.glsl")), 1);
}

UMBRIEL_TEST(effectPresetsNeedAKindAndKeepTheirNameWithoutAShader) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write(
      "[effects.preset.nokind]\nshader = \"x.glsl\"\n[effects.preset.missing]\nkind = \"screen\"\nshader = "
      "\"absent.glsl\"\n[effects.preset.off]\nkind = \"screen\"\n"
  );
  CHECK(store.reload().success);
  CHECK(umbriel::findEffectPreset(store.config().effects, "nokind") == nullptr);
  CHECK(
      containsDiagnostic(store, "ignoring effects.preset.nokind (kind must be animation|border|window|screen|cursor)")
  );
  const umbriel::EffectPreset* missing = umbriel::findEffectPreset(store.config().effects, "missing");
  CHECK(missing != nullptr && missing->inert());
  CHECK(containsDiagnostic(store, "cannot read shader file"));
  CHECK(umbriel::findEffectPreset(store.config().effects, "off") == nullptr);
  CHECK(containsDiagnostic(store, "ignoring effects.preset.off ('off' is reserved)"));
}

UMBRIEL_TEST(effectSelectorsLoadAtEveryLevel) {
  const TempConfigTree tree;
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("w.glsl", "vec4 window(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("s.glsl", "vec4 screen(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("c.glsl", "vec4 cursor(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("o.glsl", "vec4 animation(vec2 uv) { return umbriel_sample(uv); }");
  tree.write(
      "config.toml",
      "[effects]\nborder = \"ring\"\nwindow = \"lines\"\nscreen = \"vig\"\ncursor = \"glow\"\n"
      "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\noverlay = \"lines\"\n"
      "[effects.preset.lines]\nkind = \"window\"\nshader = \"w.glsl\"\n"
      "[effects.preset.vig]\nkind = \"screen\"\nshader = \"s.glsl\"\n"
      "[effects.preset.glow]\nkind = \"cursor\"\nshader = \"c.glsl\"\n"
      "[effects.preset.open]\nkind = \"animation\"\nshader = \"o.glsl\"\n"
      "[[window_rule]]\nmatch.app_id = \"^foot$\"\nborder_effect = \"off\"\nwindow_effect = \"lines\"\n"
      "[output.\"HEADLESS-1\"]\nscreen_effect = \"off\"\n"
      "[animation.windows_in]\neffect = \"open\"\n"
      "[animation.windows_drag]\nphysics = true\n"
  );
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& config = store.config();
  CHECK_EQ(config.effects.border, std::string("ring"));
  CHECK_EQ(config.effects.window, std::string("lines"));
  CHECK_EQ(config.effects.screen, std::string("vig"));
  CHECK_EQ(config.effects.cursor, std::string("glow"));
  CHECK_EQ(config.windowRules.size(), size_t{1});
  CHECK(config.windowRules[0].borderEffect == "off");
  CHECK(config.windowRules[0].windowEffect == "lines");
  CHECK_EQ(config.outputs.size(), size_t{1});
  CHECK(config.outputs[0].screenEffect == "off");
  CHECK_EQ(config.animation.windowsIn.effect, std::string("open"));
  CHECK(config.animation.windowsDrag.physics);
  CHECK(!containsDiagnostic(store, "unknown key"));
  const umbriel::EffectPreset* ring = umbriel::findEffectPreset(config.effects, "ring");
  CHECK(ring != nullptr && ring->overlay == "lines");
}

UMBRIEL_TEST(effectReferencesAreValidatedAfterEverySectionIsRead) {
  const TempConfigTree tree;
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write(
      "config.toml",
      // Forward reference: the selector precedes the preset in the file and the preset comes from an include.
      "[effects]\nborder = \"ring\"\nwindow = \"ring\"\nscreen = \"nope\"\n"
      "[include]\nfiles = [\"presets.toml\"]\n"
      "[[window_rule]]\nmatch.app_id = \"^foot$\"\nborder_effect = \"nope\"\nwindow_effect = \"\"\n"
      "[output.\"HEADLESS-1\"]\nscreen_effect = \"ring\"\n"
      "[animation.windows_out]\neffect = \"ring\"\n"
  );
  tree.write(
      "presets.toml",
      "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\noverlay = \"nope\"\n"
      "[effects.preset.ring2]\nkind = \"border\"\nshader = \"a.glsl\"\noverlay = \"ring\"\n"
  );
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& config = store.config();
  CHECK_EQ(config.effects.border, std::string("ring"));
  CHECK(config.effects.window.empty());
  CHECK(containsDiagnostic(store, "ignoring effects.window (effect 'ring' is a border preset, not a window preset)"));
  CHECK(config.effects.screen.empty());
  CHECK(containsDiagnostic(store, "ignoring effects.screen (unknown effect 'nope')"));
  CHECK_EQ(config.windowRules.size(), size_t{1});
  CHECK_EQ(config.outputs.size(), size_t{1});
  if (config.windowRules.size() != 1 || config.outputs.size() != 1) {
    return;
  }
  CHECK(!config.windowRules[0].borderEffect);
  CHECK(config.windowRules[0].windowEffect == "");
  CHECK(!config.outputs[0].screenEffect);
  CHECK(containsDiagnostic(
      store, "ignoring output.HEADLESS-1.screen_effect (effect 'ring' is a border preset, not a screen preset)"
  ));
  CHECK(config.animation.windowsOut.effect.empty());
  CHECK(containsDiagnostic(
      store, "ignoring animation.windows_out.effect (effect 'ring' is a border preset, not an animation preset)"
  ));
  const umbriel::EffectPreset* ring = umbriel::findEffectPreset(config.effects, "ring");
  CHECK(ring != nullptr && ring->overlay.empty());
  CHECK(containsDiagnostic(store, "ignoring effects.preset.ring.overlay (unknown effect 'nope')"));
  // An overlay must name a window preset: a border preset is the wrong kind.
  const umbriel::EffectPreset* ring2 = umbriel::findEffectPreset(config.effects, "ring2");
  CHECK(ring2 != nullptr && ring2->overlay.empty());
  CHECK(containsDiagnostic(
      store, "ignoring effects.preset.ring2.overlay (effect 'ring' is a border preset, not a window preset)"
  ));
}

UMBRIEL_TEST(effectPaletteFollowsTheColorsSectionOrder) {
  umbriel::Config config;
  config.colors.accentPrimary = {1, 0, 0, 1};
  config.colors.accentSecondary = {0, 1, 0, 1};
  config.colors.warning = {0, 0, 1, 1};
  config.colors.error = {1, 1, 0, 1};
  const auto palette = umbriel::effectPalette(config.colors);
  CHECK(palette[0] == config.colors.accentPrimary);
  CHECK(palette[1] == config.colors.accentSecondary);
  CHECK(palette[2] == config.colors.warning);
  CHECK(palette[3] == config.colors.error);
}

UMBRIEL_TEST(effectReloadsFlagOnlyEffectDependentState) {
  const TempConfigTree tree;
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write(
      "config.toml",
      "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\n[effects]\nborder = \"ring\"\n"
      "[output.\"HEADLESS-1\"]\nenabled = true\n"
  );
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto same = store.reload();
  CHECK(same.success);
  CHECK(!same.effects.effects);
  CHECK(!same.effects.any());
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv) * 0.5; }");
  const auto edited = store.reload();
  CHECK(edited.success);
  CHECK(edited.effects.effects);
  CHECK(edited.change.effects);
  CHECK(!edited.effects.animation);
  tree.write(
      "config.toml",
      "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\n[effects]\nborder = \"ring\"\n"
      "[output.\"HEADLESS-1\"]\nenabled = true\nscreen_effect = \"off\"\n"
  );
  const auto output = store.reload();
  CHECK(output.success);
  CHECK(output.effects.effects);
  CHECK(!output.effects.outputState);
  CHECK_EQ(output.effects.summary(), std::string("effects"));
}

UMBRIEL_TEST(effectSelectorNonStringValueWarnsAndLeavesSettingUnset) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[[window_rule]]\nmatch.app_id = \"^foot$\"\nborder_effect = 3\n");
  CHECK(store.reload().success);
  const auto& config = store.config();
  CHECK_EQ(config.windowRules.size(), size_t{1});
  if (config.windowRules.size() != 1) {
    return;
  }
  CHECK(!config.windowRules[0].borderEffect);
}

UMBRIEL_TEST(effectSelectorOnADroppedWindowRuleRecordsNoReference) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  file.write("[[window_rule]]\nmatch.app_id = \"[\"\nborder_effect = \"off\"\n");
  CHECK(store.reload().success);
  const auto& config = store.config();
  CHECK(config.windowRules.empty());
  CHECK(!containsDiagnostic(store, "border_effect"));
}

UMBRIEL_TEST(duplicateOutputSectionDoesNotCorruptASurvivingScreenEffectReference) {
  const TempConfigTree tree;
  tree.write("vig.glsl", "vec4 screen(vec2 uv) { return umbriel_sample(uv); }");
  tree.write(
      "config.toml",
      "[effects.preset.vig]\nkind = \"screen\"\nshader = \"vig.glsl\"\n"
      "[output.\"DP-1\"]\nscreen_effect = \"nope\"\n"
      "[output.\"HDMI-A-1\"]\nscreen_effect = \"vig\"\n"
      "[output.\"dp-1\"]\nenabled = true\n"
  );
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& config = store.config();
  CHECK_EQ(config.outputs.size(), size_t{2});
  const auto hdmi =
      std::ranges::find_if(config.outputs, [](const umbriel::OutputRule& rule) { return rule.name == "HDMI-A-1"; });
  CHECK(hdmi != config.outputs.end());
  if (hdmi != config.outputs.end()) {
    CHECK(hdmi->screenEffect == "vig");
  }
  CHECK(containsDiagnostic(store, "duplicate output section 'dp-1'"));
  // The discarded DP-1 section's own screen_effect setting is superseded along with the rest of the section: it is
  // never validated, so it produces no warning of its own.
  CHECK(!containsDiagnostic(store, "screen_effect (unknown effect 'nope')"));
}

UMBRIEL_TEST(duplicateEffectPresetsAcrossIncludesAreRejected) {
  const TempConfigTree tree;
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("theme.toml", "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\n");
  tree.write("config.toml", "[include]\nfiles = [\"theme.toml\"]\n[effects]\nborder = \"ring\"\n");
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const uint64_t generation = store.generation();
  const umbriel::Config previous = store.config();

  tree.write(
      "config.toml",
      "[include]\nfiles = [\"theme.toml\"]\n[effects]\nborder = \"ring\"\n"
      "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\npadding = 4\n"
  );
  const auto duplicate = store.reload();
  CHECK(!duplicate.success);
  CHECK(containsDiagnostic(store, "effects.preset.ring is also defined in"));
  CHECK(containsDiagnostic(store, "theme.toml"));
  CHECK(store.config() == previous);
  CHECK_EQ(store.generation(), generation);
}

UMBRIEL_TEST(initialDuplicateEffectPresetsKeepCompatibilityDefaults) {
  const TempConfigTree tree;
  const std::string preset = "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\n";
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("theme.toml", preset);
  tree.write("config.toml", "[include]\nfiles = [\"theme.toml\"]\n" + preset);

  ConfigStore& store = umbriel::configStore();
  const uint64_t generation = store.generation();

  CHECK(store.load(tree.path("config.toml").c_str()));
  CHECK_EQ(store.generation(), generation + 1);
  CHECK(store.config().effects.presets.empty());
  CHECK(std::ranges::any_of(store.diagnostics(), [](const ConfigDiagnostic& diagnostic) {
    return diagnostic.severity == ConfigDiagnostic::Severity::Error
        && diagnostic.message.contains("effects.preset.ring is also defined in");
  }));
}

UMBRIEL_TEST(duplicateEffectPresetsInSiblingIncludesNameTheFirstSibling) {
  const TempConfigTree tree;
  const std::string preset = "[effects.preset.ring]\nkind = \"border\"\nshader = \"a.glsl\"\n";
  tree.write("a.glsl", "vec4 border(vec2 uv) { return umbriel_sample(uv); }");
  tree.write("first.toml", preset);
  tree.write("second.toml", preset);
  tree.write("config.toml", "[include]\nfiles = [\"first.toml\", \"second.toml\"]\n");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(!store.reload().success);
  const auto isDuplicate = [](const ConfigDiagnostic& diagnostic) {
    return diagnostic.message.contains("is also defined in");
  };
  CHECK_EQ(std::ranges::count_if(store.diagnostics(), isDuplicate), std::ptrdiff_t{1});
  const auto duplicate = std::ranges::find_if(store.diagnostics(), isDuplicate);
  CHECK(duplicate != store.diagnostics().end());
  if (duplicate == store.diagnostics().end()) {
    return;
  }
  CHECK(duplicate->severity == ConfigDiagnostic::Severity::Error);
  CHECK(duplicate->message.contains(tree.path("first.toml").string() + ":1:"));
  CHECK(duplicate->file.ends_with("second.toml"));
}

UMBRIEL_TEST(bundledEffectPresetsDefineWithoutSelecting) {
  const TempConfigTree tree;
  std::string includes = "[include]\nfiles = [\n";
  for (const char* effect :
       {"animation/reveal", "animation/squash", "border/pulse", "window/scanlines", "screen/vignette", "cursor/glow"}) {
    includes += std::format("  \"{}/examples/effects/{}/effect.toml\",\n", UMBRIEL_SOURCE_ROOT, effect);
  }
  includes += "]\n";
  tree.write("config.toml", includes);
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  CHECK(!containsDiagnostic(store, "unknown key"));
  CHECK(!containsDiagnostic(store, "cannot read shader"));
  const auto& effects = store.config().effects;
  CHECK_EQ(effects.presets.size(), size_t{6});
  CHECK(effects.border.empty() && effects.window.empty() && effects.screen.empty() && effects.cursor.empty());
  const umbriel::EffectPreset* pulse = umbriel::findEffectPreset(effects, "pulse");
  CHECK(pulse != nullptr && pulse->kind == umbriel::EffectKind::Border && pulse->light.has_value());
  const umbriel::EffectPreset* glow = umbriel::findEffectPreset(effects, "glow");
  CHECK(glow != nullptr && glow->kind == umbriel::EffectKind::Cursor && glow->radius > 0);
}

UMBRIEL_TEST(effectPoolsPreserveDeclarationsAcrossNestedIncludesAndValidateMembersSafely) {
  const TempConfigTree tree;
  tree.write("nested.toml", R"(
[effects.preset.zulu]
kind = 'window'
[effects.preset.alpha]
kind = 'window'
[effects.preset.'bad/name']
kind = 'window'
[effects.pool.zpool]
kind = 'window'
choose = ['zulu', 'missing', 'alpha', 'zulu', 2, '', 'off', 'apool', 'border', 'root']
selection = 'round_robin'
[effects.pool.apool]
kind = 'window'
choose = []
)");
  tree.write("middle.toml", R"(
[include]
files = ['nested.toml']
[effects.preset.middle]
kind = 'window'
[effects.pool.middle]
kind = 'window'
choose = ['zulu']
[effects.pool.mpool]
kind = 'window'
choose = ['root']
selection = 'random'
)");
  tree.write("config.toml", R"(
[include]
files = ['middle.toml']
[effects]
window = 'zpool'
[effects.preset.root]
kind = 'window'
[effects.preset.border]
kind = 'border'
overlay = 'zpool'
[effects.pool.rpool]
kind = 'window'
choose = ['alpha']
[[window_rule]]
match.app_id = '^test$'
window_effect = 'mpool'
[animation.windows_in]
effect = 'zpool'
)");
  auto& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& effects = store.config().effects;
  std::vector<std::string> presets;
  presets.reserve(effects.presets.size());
  for (const auto& preset : effects.presets) {
    presets.push_back(preset.name);
  }
  CHECK(presets == std::vector<std::string>({"zulu", "alpha", "middle", "root", "border"}));
  std::vector<std::string> pools;
  pools.reserve(effects.pools.size());
  for (const auto& pool : effects.pools) {
    pools.push_back(pool.name);
  }
  CHECK(pools == std::vector<std::string>({"zpool", "apool", "mpool", "rpool"}));
  const auto* zpool = umbriel::findEffectPool(effects, "zpool");
  CHECK(zpool != nullptr);
  if (zpool) {
    CHECK(zpool->members == std::vector<std::string>({"zulu", "alpha", "root"}));
    CHECK(zpool->selection == umbriel::EffectSelectionPolicy::RoundRobin);
  }
  CHECK_EQ(effects.window, std::string("zpool"));
  CHECK(store.config().windowRules[0].windowEffect == "mpool");
  CHECK(store.config().animation.windowsIn.effect.empty());
  CHECK(umbriel::findEffectPreset(effects, "border")->overlay.empty());
  CHECK(containsDiagnostic(store, "duplicate member"));
  CHECK(containsDiagnostic(store, "a preset is required"));
  CHECK(containsDiagnostic(store, "action delimiter; rename"));
  CHECK(containsDiagnostic(store, "pool is inert"));
  CHECK(std::ranges::any_of(store.diagnostics(), [&](const auto& diagnostic) {
    return diagnostic.message.contains("ignoring effects.pool.middle")
        && diagnostic.message.contains(tree.path("middle.toml").string())
        && diagnostic.file == tree.path("middle.toml").string()
        && diagnostic.line > 0;
  }));
}

UMBRIEL_TEST(effectPoolRequiredFieldsAndNamesRejectMalformedDeclarations) {
  const TempConfig file;
  file.write(R"(
[effects.pool.no_kind]
choose = []
[effects.pool.animation]
kind = 'animation'
choose = []
[effects.pool.wrong_kind_type]
kind = 2
choose = []
[effects.pool.no_choose]
kind = 'border'
[effects.pool.wrong_choose]
kind = 'border'
choose = 'ring'
[effects.pool.wrong_choose_table]
kind = 'border'
choose = { member = 'ring' }
[effects.pool.wrong_policy]
kind = 'border'
choose = []
selection = 'round-robin'
[effects.pool.wrong_policy_type]
kind = 'border'
choose = []
selection = 42
[effects.pool.off]
kind = 'border'
choose = []
[effects.pool.'']
kind = 'border'
choose = []
[effects.pool.'bad/name']
kind = 'border'
choose = []
[effects.preset.'']
kind = 'border'
[effects.preset.'bad/name']
kind = 'border'
[effects.pool.empty]
kind = 'border'
choose = []
[effects.pool.invalid_members]
kind = 'border'
choose = ['missing', 'off', '', false]
[effects]
border = 'invalid_members'
)");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  const auto& effects = store.config().effects;
  CHECK_EQ(effects.pools.size(), size_t{2});
  CHECK(effects.presets.empty());
  CHECK_EQ(effects.border, std::string("invalid_members"));
  for (const auto& pool : effects.pools) {
    CHECK(pool.members.empty());
    CHECK(pool.selection == umbriel::EffectSelectionPolicy::UnusedFirst);
  }
  CHECK(containsDiagnostic(store, "choose is required and must be an array"));
  CHECK(containsDiagnostic(store, "selection must be unused_first|round_robin|random"));
  CHECK(containsDiagnostic(store, "effect names must not be empty"));
}

UMBRIEL_TEST(effectPoolSelectorsWorkForScreenCursorAndOutputRules) {
  const TempConfig file;
  file.write(R"(
[effects]
screen = 'screens'
cursor = 'cursors'
[effects.preset.screen]
kind = 'screen'
[effects.preset.cursor]
kind = 'cursor'
[effects.pool.screens]
kind = 'screen'
choose = ['screen']
[effects.pool.cursors]
kind = 'cursor'
choose = ['cursor']
[output.HEADLESS-1]
screen_effect = 'screens'
)");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK_EQ(store.config().effects.screen, std::string("screens"));
  CHECK_EQ(store.config().effects.cursor, std::string("cursors"));
  CHECK(store.config().outputs[0].screenEffect == "screens");
}

UMBRIEL_TEST(duplicateEffectPoolsRejectReloadAndNameBothLocations) {
  const TempConfigTree tree;
  tree.write("theme.toml", "[effects.pool.shared]\nkind = 'border'\nchoose = []\n");
  tree.write("config.toml", "[include]\nfiles = ['theme.toml']\n");
  auto& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto previous = store.config();
  tree.write("config.toml", "[include]\nfiles = ['theme.toml']\n[effects.pool.shared]\nkind = 'border'\nchoose = []\n");
  CHECK(!store.reload().success);
  CHECK(store.config() == previous);
  CHECK(std::ranges::any_of(store.diagnostics(), [&](const auto& diagnostic) {
    return diagnostic.severity == ConfigDiagnostic::Severity::Error
        && diagnostic.file == tree.path("config.toml").string()
        && diagnostic.message.contains("effects.pool.shared is also defined in")
        && diagnostic.message.contains(tree.path("theme.toml").string());
  }));
}

UMBRIEL_TEST(effectPoolCollisionAcrossIncludesKeepsPresetRegardlessOfDeclarationOrder) {
  const TempConfigTree tree;
  for (bool poolFirst : {false, true}) {
    const std::string preset = "[effects.preset.shared]\nkind = 'window'\n";
    const std::string pool = "[effects.pool.shared]\nkind = 'window'\nchoose = []\n";
    tree.write("theme.toml", poolFirst ? pool : preset);
    tree.write(
        "config.toml", "[include]\nfiles = ['theme.toml']\n[effects]\nwindow = 'shared'\n" + (poolFirst ? preset : pool)
    );
    auto& store = umbriel::configStore();
    store.setRootPath(tree.path("config.toml"), true);
    CHECK(store.reload().success);
    CHECK(umbriel::findEffectPreset(store.config().effects, "shared") != nullptr);
    CHECK(umbriel::findEffectPool(store.config().effects, "shared") == nullptr);
    CHECK_EQ(store.config().effects.window, std::string("shared"));
    CHECK(containsDiagnostic(store, "preset with this name declared at"));
  }
}

UMBRIEL_TEST(effectActionReferencesValidateKindsPoolsAndDisabledCorners) {
  TempConfig file;
  file.write(R"(
[effects.preset.window]
kind = 'window'
[effects.preset.border]
kind = 'border'
[effects.pool.windows]
kind = 'window'
choose = ['window']
[keybinds]
'Mod+F1' = 'effect-window-set:window'
'Mod+F2' = 'effect-window-set:windows'
'Mod+F3' = 'effect-window-cycle:windows'
'Mod+F4' = 'effect-window-set:missing'
'Mod+F5' = 'effect-window-set:border'
'Mod+F6' = 'effect-window-cycle:window'
'Mod+F7' = 'effect-window-set:off'
'Mod+F8' = 'effect-window-cycle'
'submap[effects],F9' = 'effect-window-set:missing'
'Mod+F10' = 'effect-window-set:window'
'Mod+f10' = 'effect-window-set:missing'
[hot_corners.top_left]
enabled = false
action = 'effect-screen-set:window'
[hot_corners.top_right]
enabled = true
action = 'effect-window-cycle:windows'
)");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+F4'"));
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+F5'"));
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+F6'"));
  CHECK(containsDiagnostic(store, "ignoring keybind 'submap[effects],F9'"));
  CHECK(containsDiagnostic(store, "ignoring hot_corners.top_left.action"));
  CHECK(!store.config().hotCorners.corners[0].action);
  CHECK(store.config().hotCorners.corners[1].action.has_value());
  size_t valid = 0;
  for (const auto& bind : store.config().keybinds) {
    if (bind.action == umbriel::KeybindAction::EffectWindowSet
        || bind.action == umbriel::KeybindAction::EffectWindowCycle) {
      ++valid;
      const auto reference = umbriel::effectActionReference(bind);
      CHECK(!reference || reference->name == "window" || reference->name == "windows");
    }
  }
  // Six, not five: a rejected spelling of a chord leaves its valid spelling bound, as every other rejected bind does.
  CHECK_EQ(valid, size_t{6});
  CHECK(containsDiagnostic(store, "ignoring keybind 'Mod+f10'"));
  CHECK(std::ranges::any_of(store.diagnostics(), [&](const auto& diagnostic) {
    return diagnostic.file == file.path().string() && diagnostic.message.contains("Mod+F4");
  }));
}

UMBRIEL_TEST(replacedEffectActionsLeaveNoReferencesOrDiagnostics) {
  TempConfig file;
  file.writeInclude(R"(
[keybinds]
'Mod+F1' = 'effect-window-set:missing'
[hot_corners.top_left]
action = 'effect-cursor-set:missing'
)");
  file.write("[include]\nfiles = ['" + file.includeName() + "']\n" + R"(
[effects.preset.window]
kind = 'window'
[keybinds]
'Mod+F1' = 'effect-window-set:window'
[hot_corners.top_left]
enabled = false
action = 'effect-window-set:window'
)");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK(!containsDiagnostic(store, "missing"));
  CHECK(store.config().hotCorners.corners[0].action.has_value());
  CHECK(umbriel::effectActionReference(*store.config().hotCorners.corners[0].action)->name == "window");
}

UMBRIEL_TEST(audioSourcesRequireExplicitModeAndExclusiveTargetSelection) {
  const TempConfig file;
  file.write(R"(
[effects.audio.sources.desktop]
provider = 'pipewire'
mode = 'playback'
follow_default = true
[effects.audio.sources.voice]
provider = 'pipewire'
mode = 'microphone'
target = 'exact microphone node'
follow_default = false
)");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  const auto& sources = store.config().effects.audioSources;
  CHECK_EQ(sources.size(), size_t{2});
  if (sources.size() != 2) {
    return;
  }
  CHECK(sources[0].provider == umbriel::AudioProvider::Pipewire);
  CHECK(sources[0].mode == umbriel::AudioMode::Playback);
  CHECK(sources[0].target.empty());
  CHECK(sources[0].followDefault);
  CHECK(sources[1].mode == umbriel::AudioMode::Microphone);
  CHECK_EQ(sources[1].target, std::string("exact microphone node"));
  CHECK(!sources[1].followDefault);
  CHECK(store.config().effects.presets.empty());
  CHECK(store.diagnostics().empty());
}

UMBRIEL_TEST(audioExternalExecutableResolvesBesideItsDeclaringIncludeAndPreservesArgv) {
  const TempConfigTree tree;
  tree.write("config.toml", "[include]\nfiles = ['theme/audio.toml']\n");
  tree.write("theme/audio.toml", R"(
[effects.audio.sources.external]
provider = 'external'
mode = 'playback'
target = 'literal target'
executable = '../helpers/audio helper'
args = ['', 'a b', '$(touch should-not-exist)', '; echo text', '$HOME', '*.wav']
)");
  auto& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& sources = store.config().effects.audioSources;
  CHECK_EQ(sources.size(), size_t{1});
  if (sources.size() != 1) {
    return;
  }
  const auto& source = sources[0];
  CHECK(source.provider == umbriel::AudioProvider::External);
  CHECK_EQ(source.executable, tree.path("helpers/audio helper").string());
  CHECK(
      source.args == std::vector<std::string>({"", "a b", "$(touch should-not-exist)", "; echo text", "$HOME", "*.wav"})
  );
  CHECK(store.diagnostics().empty());
  // Loading metadata does not require opening or executing the helper.
  CHECK(!std::filesystem::exists(source.executable));
}

UMBRIEL_TEST(audioInvalidSourceDeclarationsNeverFallBackToAnImplicitDevice) {
  const TempConfig file;
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const std::array<std::string_view, 22> invalid = {
      "mode = 'playback'\nfollow_default = true\n",
      "provider = 'pipewire'\nfollow_default = true\n",
      "provider = 'shell'\nmode = 'playback'\nfollow_default = true\n",
      "provider = 'pipewire'\nmode = 'capture'\nfollow_default = true\n",
      "provider = 'pipewire'\nmode = 'playback'\n",
      "provider = 'pipewire'\nmode = 'playback'\nfollow_default = false\n",
      "provider = 'pipewire'\nmode = 'playback'\ntarget = ''\n",
      "provider = 'pipewire'\nmode = 'playback'\ntarget = ''\nfollow_default = true\n",
      "provider = 'pipewire'\nmode = 'playback'\ntarget = 'fixed'\nfollow_default = true\n",
      "provider = 'pipewire'\nmode = 'playback'\ntarget = 1\n",
      "provider = 'pipewire'\nmode = 'playback'\nfollow_default = 'true'\n",
      "provider = 'pipewire'\nmode = 'playback'\nfollow_default = true\ntypo = 1\n",
      "provider = 'pipewire'\nmode = 'playback'\nfollow_default = true\nexecutable = '/bin/false'\n",
      "provider = 'pipewire'\nmode = 'playback'\nfollow_default = true\nargs = []\n",
      "provider = 'external'\nmode = 'microphone'\ntarget = 'fixed'\n",
      "provider = 'external'\nmode = 'microphone'\ntarget = 'fixed'\nexecutable = ''\n",
      "provider = 'external'\nmode = 'microphone'\ntarget = 'fixed'\nexecutable = 1\n",
      "provider = 'external'\nmode = 'microphone'\ntarget = 'fixed'\nexecutable = 'helper'\nargs = 'shell command'\n",
      "provider = 'external'\nmode = 'microphone'\ntarget = 'fixed'\nexecutable = 'helper'\nargs = [1]\n",
      "provider = 1\nmode = 'playback'\nfollow_default = true\n",
      "provider = 'pipewire'\nmode = true\nfollow_default = true\n",
      "provider = 'external'\nmode = 'playback'\nfollow_default = true\nexecutable = 'helper'\nshell = true\n",
  };
  for (const auto declaration : invalid) {
    file.write("[effects.audio.sources.invalid]\n" + std::string(declaration));
    CHECK(store.reload().success);
    CHECK(store.config().effects.audioSources.empty());
    CHECK(containsDiagnostic(store, "ignoring effects.audio.sources.invalid"));
  }
}

UMBRIEL_TEST(audioReferencesResolveAcrossForwardIncludesForEveryExistingPresetKind) {
  const TempConfigTree tree;
  tree.write("sources.toml", R"(
[effects.audio.sources.desktop]
provider = 'pipewire'
mode = 'playback'
follow_default = true
)");
  std::string document = "[include]\nfiles = ['sources.toml']\n";
  for (const auto kind : {"animation", "border", "window", "screen", "cursor"}) {
    document += "[effects.preset." + std::string(kind) + "]\nkind = '" + kind + "'\naudio = 'desktop'\n";
  }
  document += "[effects.preset.unbound]\nkind = 'window'\n";
  document += "[effects.preset.unknown]\nkind = 'window'\naudio = 'missing'\n";
  document += "[effects.preset.wrong_type]\nkind = 'window'\naudio = true\n";
  tree.write("config.toml", document);
  auto& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  for (const auto kind : {"animation", "border", "window", "screen", "cursor"}) {
    const auto* preset = umbriel::findEffectPreset(store.config().effects, kind);
    CHECK(preset != nullptr && preset->audio == "desktop");
  }
  for (const auto name : {"unbound", "unknown", "wrong_type"}) {
    const auto* preset = umbriel::findEffectPreset(store.config().effects, name);
    CHECK(preset != nullptr && preset->audio.empty());
  }
  CHECK(containsDiagnostic(store, "ignoring effects.preset.unknown.audio (unknown audio source 'missing')"));
  CHECK(containsDiagnostic(store, "effects.preset.wrong_type.audio (expected string)"));
  CHECK(!containsDiagnostic(store, "unknown key"));
  CHECK(std::ranges::any_of(store.diagnostics(), [&](const auto& diagnostic) {
    return diagnostic.file == tree.path("config.toml").string()
        && diagnostic.message.contains("unknown audio source 'missing'");
  }));
  // Definitions and bindings leave global consumers disabled; runtime demand is tested separately.
  CHECK(store.config().effects.border.empty());
  CHECK(store.config().effects.window.empty());
  CHECK(store.config().effects.screen.empty());
  CHECK(store.config().effects.cursor.empty());
}

UMBRIEL_TEST(audioDefinitionAndBindingChangesInvalidateEffectsAndResetOnReload) {
  const TempConfig file;
  const std::string source = "[effects.audio.sources.desktop]\nprovider = 'pipewire'\nmode = 'playback'\n";
  file.write(source + "target = 'first'\n[effects.preset.a]\nkind = 'window'\naudio = 'desktop'\n");
  auto& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  CHECK(store.reload().success);
  CHECK(!store.reload().effects.any());
  file.write(source + "target = 'second'\n[effects.preset.a]\nkind = 'window'\naudio = 'desktop'\n");
  const auto changed = store.reload();
  CHECK(changed.success);
  CHECK(changed.effects.effects);
  file.write(source + "target = 'second'\n[effects.preset.a]\nkind = 'window'\n");
  const auto unbound = store.reload();
  CHECK(unbound.success);
  CHECK(unbound.effects.effects);
  file.write("");
  CHECK(store.reload().success);
  CHECK(store.config().effects.audioSources.empty());
}

int main() { return RUN_TESTS(); }
