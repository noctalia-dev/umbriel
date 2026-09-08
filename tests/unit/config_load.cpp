#include "check.h"
#include "config/store.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <linux/input-event-codes.h>
#include <optional>
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
surprise = true

[output.DP-1]
workspaces = ["dev"]

[[workspace]]
name = "dev"

[workspace.layout.master]
position = "left"
default_width_fraction = 0.7
new_on_top = true
)");

  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);
  const umbriel::ConfigReloadResult result = store.reload();

  CHECK(result.success);
  CHECK(store.config().layout.mode == LayoutMode::Master);
  CHECK(store.config().layout.master.position == umbriel::MasterPosition::Right);
  CHECK_EQ(store.config().layout.master.defaultWidthFraction, 0.1);
  CHECK(!store.config().layout.master.newOnTop);
  CHECK_EQ(store.config().workspaceRules.size(), size_t{1});
  CHECK(store.config().workspaceRules[0].layout.master.position == umbriel::MasterPosition::Left);
  CHECK(store.config().workspaceRules[0].layout.master.defaultWidthFraction.has_value());
  CHECK_EQ(*store.config().workspaceRules[0].layout.master.defaultWidthFraction, 0.7);
  CHECK(store.config().workspaceRules[0].layout.master.newOnTop == true);
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

UMBRIEL_TEST(outputScrollingDefaultWidthUsesNarrowLayoutScope) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(R"(
[output.DP-1.layout]
gap = 12

[output.DP-1.layout.scrolling]
default_width_fraction = 0.05
center_focused = true
)");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().outputs.size(), size_t{1});
  CHECK(store.config().outputs[0].layout.scrolling.defaultWidthFraction.has_value());
  if (store.config().outputs[0].layout.scrolling.defaultWidthFraction) {
    CHECK_EQ(*store.config().outputs[0].layout.scrolling.defaultWidthFraction, 0.1);
  }
  CHECK(containsDiagnostic(
      store, "output.DP-1.layout.scrolling.default_width_fraction = 0.05 out of range, clamped to 0.1"
  ));
  CHECK(containsDiagnostic(store, "unknown key output.DP-1.layout.gap"));
  CHECK(containsDiagnostic(store, "unknown key output.DP-1.layout.scrolling.center_focused"));

  file.write("[output.DP-1]\nenabled = true\n");
  CHECK(store.reload().success);
  CHECK(!store.config().outputs[0].layout.scrolling.defaultWidthFraction.has_value());
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
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.workspace_axis (expected vertical|horizontal)"));

  file.write("[output.DP-1]\nworkspace_axis = true\n");
  CHECK(store.reload().success);
  CHECK(store.config().outputs[0].workspaceAxis == umbriel::WorkspaceAxis::Vertical);
  CHECK(containsDiagnostic(store, "ignoring output.DP-1.workspace_axis (expected vertical|horizontal)"));

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
      "[colors.border]\nfocused = \"#01020304\"\nunfocused = \"#05060708\"\n"
      "scratchpad_focused = \"#090A0B0C\"\nscratchpad_unfocused = \"#0D0E0F10\"\nouter = \"#11121314\"\n"
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
  CHECK_EQ(colors.border.scratchpadFocused[1], 10.0F / 255.0F);
  CHECK_EQ(colors.border.scratchpadUnfocused[2], 15.0F / 255.0F);
  CHECK_EQ(colors.border.outer[0], 17.0F / 255.0F);
  CHECK_EQ(colors.overview.backgroundTint[1], 22.0F / 255.0F);
  CHECK_EQ(colors.overview.workspaceBackground[2], 27.0F / 255.0F);
  CHECK_EQ(colors.overview.badge[0], 18.0F / 255.0F);
  CHECK_EQ(colors.overview.badge[3], 120.0F / 255.0F);
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

UMBRIEL_TEST(windowRuleWorkspaceTargetPreservesIntegerAndStringSelectors) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write("[[window_rule]]\ndefault_workspace = 2\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& positionTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* position = positionTarget ? std::get_if<int>(&*positionTarget) : nullptr;
  CHECK(position != nullptr);
  CHECK(position != nullptr && *position == 2);
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = 64\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& limitTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* limit = limitTarget ? std::get_if<int>(&*limitTarget) : nullptr;
  CHECK(limit != nullptr);
  CHECK(limit != nullptr && *limit == 64);

  file.write("[[window_rule]]\ndefault_workspace = \"CHAT\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& nameTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* name = nameTarget ? std::get_if<std::string>(&*nameTarget) : nullptr;
  CHECK(name != nullptr);
  CHECK(name != nullptr && *name == "CHAT");

  // A numeric-looking string remains a name. It must not silently become a
  // positional selector during parsing.
  file.write("[[window_rule]]\ndefault_workspace = \"2\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  const auto& numericNameTarget = store.config().windowRules[0].defaultWorkspace;
  const auto* numericName = numericNameTarget ? std::get_if<std::string>(&*numericNameTarget) : nullptr;
  CHECK(numericName != nullptr);
  CHECK(numericName != nullptr && *numericName == "2");

  file.write("[[window_rule]]\ndefault_workspace = \"\"\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
  CHECK(containsDiagnostic(store, "ignoring window_rule.default_workspace"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = false\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
  CHECK(containsDiagnostic(store, "ignoring window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = 0\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
  CHECK(containsDiagnostic(store, "ignoring window_rule.default_workspace"));

  file.write("[[window_rule]]\ndefault_workspace = 65\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(!store.config().windowRules[0].defaultWorkspace.has_value());
  CHECK(containsDiagnostic(store, "ignoring window_rule.default_workspace"));
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
  CHECK(store.config().securityContextRules[0].sandboxEnginePattern == "^org\\.flatpak$");
  CHECK(store.config().securityContextRules[0].appIdPattern == "^org\\.example\\.Bar$");
  CHECK_EQ(store.config().securityContextRules[0].allowGlobals.size(), size_t{1});

  file.write("[[security_context_rule]]\nmatch.app_id = '['\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "invalid regex in security_context_rule.match.app_id"));

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
  CHECK(containsDiagnostic(store, "match.app_id must be a non-empty string"));

  file.write("[[security_context_rule]]\nmatch.sandbox_engine = 5\nallow_globals = [\"zwlr_layer_shell_v1\"]\n");
  CHECK(store.reload().success);
  CHECK(store.config().securityContextRules.empty());
  CHECK(containsDiagnostic(store, "match.sandbox_engine must be a non-empty string"));

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
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.content_type (expected none|photo|video|game)"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.content_type = \"stream\"\nmatch.is_focused = true\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.content_type (expected none|photo|video|game)"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.content_type = \"Game\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.content_type (expected none|photo|video|game)"));
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
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.at_startup (expected boolean)"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));
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
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.is_floating (expected boolean)"));
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.is_pinned (expected boolean)"));
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.is_scratchpad (expected boolean)"));
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
  CHECK_EQ(store.config().windowRules[0].xdgTagPattern, std::string("^(game-launcher|game-running)$"));
  CHECK(std::regex_search("game-launcher", store.config().windowRules[0].xdgTagRegex));
  CHECK(std::regex_search("game-running", store.config().windowRules[0].xdgTagRegex));
  CHECK(!std::regex_search("game-settings", store.config().windowRules[0].xdgTagRegex));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.xdg_tag"));

  file.write("[[window_rule]]\nopacity = 0.9\n");
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].xdgTagPattern.empty());

  file.write("[[window_rule]]\nmatch.xdg_tag = 42\nmatch.is_focused = true\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(containsDiagnostic(store, "ignoring window_rule.match.xdg_tag (expected string)"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.match.is_focused"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));

  file.write("[[window_rule]]\nmatch.xdg_tag = \"[\"\nopacity = 0.5\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules.empty());
  CHECK(containsDiagnostic(store, "invalid regex in window_rule.match.xdg_tag"));
  CHECK(!containsDiagnostic(store, "unknown key window_rule.opacity"));
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

UMBRIEL_TEST(windowRuleFractionSizingLoadsAndClamps) {
  const TempConfig file;
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(file.path(), true);

  file.write(
      "[[window_rule]]\nmatch.app_id = \"^utility$\"\ndefault_floating = true\ndefault_width = 0.5\ndefault_height = "
      "0.6\n"
  );
  CHECK(store.reload().success);
  CHECK_EQ(store.config().windowRules.size(), size_t{1});
  CHECK(store.config().windowRules[0].defaultWidth && *store.config().windowRules[0].defaultWidth == 0.5);
  CHECK(store.config().windowRules[0].defaultHeight && *store.config().windowRules[0].defaultHeight == 0.6);

  // Out-of-range fractions clamp into [0.1, 1.0] with a diagnostic, like default_width.
  file.write("[[window_rule]]\ndefault_width = 3.0\ndefault_height = 0.01\n");
  CHECK(store.reload().success);
  CHECK(store.config().windowRules[0].defaultWidth && *store.config().windowRules[0].defaultWidth == 1.0);
  CHECK(store.config().windowRules[0].defaultHeight && *store.config().windowRules[0].defaultHeight == 0.1);
  CHECK(containsDiagnostic(store, "window_rule.default_width = 3 out of range, clamped to 1"));
  CHECK(containsDiagnostic(store, "window_rule.default_height = 0.01 out of range, clamped to 0.1"));

  // Non-numeric values are ignored with a diagnostic.
  file.write("[[window_rule]]\ndefault_height = \"half\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().windowRules[0].defaultHeight);
  CHECK(containsDiagnostic(store, "ignoring window_rule.default_height (expected number 0.1-1.0)"));
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
    patterns.push_back(rule.appIdPattern);
  }
  std::vector<std::string> namespaces;
  for (const umbriel::LayerRule& rule : store.config().layerRules) {
    namespaces.push_back(rule.namespacePattern);
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
disable_on_external_mouse = true
click_method = "button_areas"

[input.mouse]
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
accel_profile = "flat"
sensitivity = -0.5
disable_while_typing = false
click_method = "clickfinger"

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
  CHECK(input.touchpad.accelProfile.has_value());
  if (input.touchpad.accelProfile.has_value()) {
    CHECK(input.touchpad.accelProfile->kind == umbriel::AccelProfile::Kind::Adaptive);
  }
  CHECK(input.touchpad.sensitivity == std::optional<double>(0.1));
  CHECK(input.touchpad.scrollFactor == std::optional<double>(1.5));
  CHECK(input.touchpad.disableWhileTyping == std::optional<bool>(true));
  CHECK(input.touchpad.disableOnExternalMouse == std::optional<bool>(true));
  CHECK(input.touchpad.clickMethod == std::optional(umbriel::ClickMethod::ButtonAreas));
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
    CHECK(touchpad->clickMethod == std::optional(umbriel::ClickMethod::ClickFinger));
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
  CHECK(containsDiagnostic(store, R"(invalid input.touchpad.click_method "button-areas")"));
  CHECK(!containsDiagnostic(store, "unknown key input.touchpad.click_method"));
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
  CHECK(containsDiagnostic(store, "input.mouse.scroll_button must be a string"));
  CHECK(!containsDiagnostic(store, "unknown key input.mouse.scroll_button"));

  file.write("[input.mouse]\nscroll_button = \"button8\"\n");
  CHECK(store.reload().success);
  CHECK(!store.config().input.mouse.scrollButton.has_value());
  CHECK(containsDiagnostic(store, R"(invalid input.mouse.scroll_button "button8")"));
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
  CHECK(containsDiagnostic(store, "expected global|window"));
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

UMBRIEL_TEST(animationShadersResolveIncludedFilesAcrossAllEventsAndTrackContentChanges) {
  const TempConfigTree tree;
  const std::array sections{"windows_in", "windows_out", "windows_move",  "workspaces", "overview",
                            "scratchpad", "border",      "dim_unfocused", "layers"};
  std::string theme;
  for (const char* section : sections) {
    theme += std::format("[animation.{}]\nshader = 'effect.glsl'\n", section);
  }
  tree.write("config.toml", "[include]\nfiles = ['theme/animation.toml']\n");
  tree.write("theme/animation.toml", theme);
  tree.write("theme/effect.glsl", "first shader");
  tree.write("effect.glsl", "wrong source directory");
  ConfigStore& store = umbriel::configStore();
  store.setRootPath(tree.path("config.toml"), true);
  CHECK(store.reload().success);
  const auto& animation = store.config().animation;
  const std::array sources{&animation.windowsIn.shader,  &animation.windowsOut.shader,   &animation.windowsMove.shader,
                           &animation.workspaces.shader, &animation.overview.shader,     &animation.scratchpad.shader,
                           &animation.border.shader,     &animation.dimUnfocused.shader, &animation.layers.shader};
  for (const auto* source : sources) {
    CHECK(source->has_value());
    if (*source) {
      CHECK_EQ((*source)->code, std::string("first shader"));
      CHECK((*source)->file == tree.path("theme/effect.glsl"));
    }
  }
  CHECK(!containsDiagnostic(store, "unknown key"));
  CHECK_EQ(std::ranges::count(store.watchPaths(), tree.path("theme/effect.glsl")), 1);

  tree.write("theme/effect.glsl", "edited shader");
  const auto edited = store.reload();
  CHECK(edited.success);
  CHECK(edited.effects.animation);
  CHECK(store.config().animation.windowsIn.shader.has_value());
  if (store.config().animation.windowsIn.shader) {
    CHECK_EQ(store.config().animation.windowsIn.shader->code, std::string("edited shader"));
  }

  tree.write("theme/replacement.glsl", "replacement shader");
  tree.write("theme/animation.toml", "[animation.windows_in]\nshader = 'replacement.glsl'\n");
  CHECK(store.reload().success);
  CHECK(std::ranges::find(store.watchPaths(), tree.path("theme/effect.glsl")) == store.watchPaths().end());
  CHECK(store.config().animation.windowsIn.shader.has_value());
  if (store.config().animation.windowsIn.shader) {
    CHECK_EQ(store.config().animation.windowsIn.shader->code, std::string("replacement shader"));
  }
  CHECK(!store.config().animation.layers.shader.has_value());
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

int main() { return RUN_TESTS(); }
