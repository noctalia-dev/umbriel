#pragma once

// Internal to src/config: what the per-topic readers (fields_*.cpp), the loader (config.cpp), and the store share.
// Nothing outside src/config includes it.

#include "config/config.h"
#include "config/config_diag.h"
#include "config/config_merge.h"
#include "config/config_registry.h"
#include "config/effects.h"
#include "config/section.h"

#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace umbriel {

  // A selector recorded while parsing and checked once every table is read, so forward and cross-include references
  // resolve.
  struct EffectReference {
    std::string context;
    std::string name;
    EffectKind kind;
    bool allowOff;
    EffectReferenceConstraint constraint;
    toml::source_region source;
    std::function<void()> clear;
  };

  // A workspace mirror an output's list declares, checked once every output is read: its origin output may come
  // later in the file or in another include.
  struct WorkspaceMirrorReference {
    std::string output;
    std::string name;
    std::string origin;
    toml::source_region source;
  };

  // What one load shares across tables.
  struct registry::ReadContext {
    // The config being loaded. Tables read earlier are already in it, such as the scratchpads an action may name.
    Config& loaded;
    std::vector<EffectReference>& effectReferences;
    std::vector<WorkspaceMirrorReference>& mirrorReferences;
    const std::vector<configmerge::EffectDeclaration>& presetDeclarations;
    const std::vector<configmerge::EffectDeclaration>& poolDeclarations;
  };

  // Record a diagnostic in the store and log it.
  void emitDiag(ConfigDiagnostic::Severity severity, const toml::source_region* src, std::string msg);

  template <typename... A> void warnAt(const toml::source_region& src, std::format_string<A...> fmt, A&&... args) {
    emitDiag(ConfigDiagnostic::Severity::Warning, &src, std::format(fmt, std::forward<A>(args)...));
  }

  template <typename... A> void warnNoSrc(std::format_string<A...> fmt, A&&... args) {
    emitDiag(ConfigDiagnostic::Severity::Warning, nullptr, std::format(fmt, std::forward<A>(args)...));
  }

  template <typename... A> void errorAt(const toml::source_region& src, std::format_string<A...> fmt, A&&... args) {
    emitDiag(ConfigDiagnostic::Severity::Error, &src, std::format(fmt, std::forward<A>(args)...));
  }

  std::string lowercase(std::string_view text);

  // Vocabularies outputs and window rules share.
  const registry::Choices<VrrMode>& vrrModes();
  const registry::Choices<HdrMode>& hdrModes();
  const registry::Choices<ContentType>& contentTypes();
  // Shared by [layout.tabs] and window rules.
  const registry::Choices<ColumnDisplay>& columnDisplays();
  // Shared by [appearance.title_bar] buttons and the title_bar_buttons window rule: an array of distinct buttons, read
  // from the title outward. Unknown or repeated entries are reported and skipped; nullopt when the value is no array.
  std::optional<std::vector<TitleBarButton>> parseTitleBarButtons(const toml::node& node, const std::string& path);
  std::vector<std::string_view> titleBarButtonNames();
  nlohmann::ordered_json titleBarButtonsJson(const std::vector<TitleBarButton>& buttons);

  // Why an action or rule may not name a scratchpad, or nullopt when it may. Scratchpads are read before every table
  // that names one.
  std::optional<std::string> scratchpadSelectorError(const Config& loaded, const Keybind& binding);
  std::optional<std::string> scratchpadTargetError(const Config& loaded, std::string_view name);
  // Why an effect action may not name its preset or pool, or nullopt when it may. Effects are read first.
  std::optional<std::string> effectActionError(const Config& loaded, const Keybind& binding);

  void addEffectReference(
      std::vector<EffectReference>& references, std::string context,
      const std::pair<std::string, toml::source_region>& selector, EffectKind kind, bool allowOff,
      std::function<void()> clear, EffectReferenceConstraint constraint = EffectReferenceConstraint::PresetOnly
  );

  // Record the effect selector a rule holds under `key`, once the rule is kept.
  void recordRuleEffect(
      registry::ReadContext& context, Section& keys, std::string_view key, EffectKind kind, std::function<void()> clear
  );

  // An effect selector, validated after all definitions are loaded.
  template <typename T> registry::Field<T> effectField(std::string_view key, std::string T::* member, EffectKind kind) {
    return registry::custom<T>(
        key, registry::KeyDescription("string").withFormat("effect"),
        [member, kind](const toml::node& node, const std::string& path, T& target, registry::ReadContext& context) {
          const auto value = node.value<std::string>();
          if (!node.is_string() || !value) {
            warnAt(node.source(), "ignoring {} (expected string)", path);
            return;
          }
          std::string& selected = target.*member;
          selected = *value;
          addEffectReference(
              context.effectReferences, path, {*value, node.source()}, kind, false, [&selected] { selected.clear(); },
              kind == EffectKind::Animation ? EffectReferenceConstraint::PresetOnly
                                            : EffectReferenceConstraint::PresetOrPool
          );
        },
        [member](const T& defaults) { return nlohmann::ordered_json(defaults.*member); }
    );
  }

  // Record a rule's effect selector after its position in the array is known.
  template <typename T>
  registry::Field<T> optionalEffectField(std::string_view key, std::optional<std::string> T::* member) {
    return registry::custom<T>(
        key, registry::KeyDescription("string").withFormat("effect"),
        [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
          if (!node.is_string()) {
            warnAt(node.source(), "ignoring {} (expected string)", path);
            return;
          }
          target.*member = node.value<std::string>();
        }
    );
  }

  // The top-level tables, each defined in the fields_*.cpp of its topic. config.cpp lists them in reading order.
  registry::Field<Config> colorsTable();
  registry::Field<Config> appearanceTable();
  registry::Field<Config> overviewTable();
  registry::Field<Config> hotCornersTable();
  registry::Field<Config> generalTable();
  registry::Field<Config> environmentTable();
  registry::Field<Config> eventsTable();
  registry::Field<Config> workspaceSettingsTable(); // [workspaces]
  registry::Field<Config> screencastTable();
  registry::Field<Config> effectsTable();
  registry::Field<Config> animationTable();
  registry::Field<Config> layoutTable();
  registry::Field<Config> workspaceRulesTable(); // [[workspace]]
  registry::Field<Config> scratchpadTable();
  registry::Field<Config> windowRuleTable();
  registry::Field<Config> layerRuleTable();
  registry::Field<Config> securityContextRuleTable();
  registry::Field<Config> drmTable();
  registry::Field<Config> inputTable();
  registry::Field<Config> outputTable();
  registry::Field<Config> keybindsTable();

  // Checks that need every table read.
  void validateEffectReferences(Config& loaded, std::vector<EffectReference>& references);
  void validateWorkspaceMirrors(const Config& loaded, const std::vector<WorkspaceMirrorReference>& references);
  void warnScrollButtonBinds(const Config& loaded);

  enum class [[nodiscard]] ConfigParseOutcome : uint8_t {
    Loaded,
    Missing,
    // Errors were reported. Startup falls back to defaults; a reload keeps the previous configuration.
    Failed,
  };

  // Merge `rootPath` with its includes and read every table into `out`, which is left alone unless the result is
  // Loaded. The caller has already found the file and reset the store for this load.
  ConfigParseOutcome parseConfig(Config& out, const std::filesystem::path& rootPath);

} // namespace umbriel
