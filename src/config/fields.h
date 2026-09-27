#pragma once

// Internal to src/config: what the per-topic readers (fields_*.cpp), the loader (config.cpp), and the store share.
// Nothing outside src/config includes it.

#include "config/config.h"
#include "config/config_diag.h"
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
    toml::source_region source;
    std::function<void()> clear;
  };

  // What one load shares across tables.
  struct registry::ReadContext {
    // The config being loaded. Tables read earlier are already in it, such as the scratchpads an action may name.
    Config& loaded;
    std::vector<EffectReference>& effectReferences;
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

  [[nodiscard]] std::string lowercase(std::string_view text);

  // Vocabularies outputs and window rules share.
  [[nodiscard]] const registry::Choices<VrrMode>& vrrModes();
  [[nodiscard]] const registry::Choices<HdrMode>& hdrModes();
  [[nodiscard]] const registry::Choices<ContentType>& contentTypes();

  // Why an action or rule may not name a scratchpad, or nullopt when it may. Scratchpads are read before every table
  // that names one.
  [[nodiscard]] std::optional<std::string> scratchpadSelectorError(const Config& loaded, const Keybind& binding);
  [[nodiscard]] std::optional<std::string> scratchpadTargetError(const Config& loaded, std::string_view name);

  void addEffectReference(
      std::vector<EffectReference>& references, std::string context,
      const std::pair<std::string, toml::source_region>& selector, EffectKind kind, bool allowOff,
      std::function<void()> clear
  );

  // Record the effect selector a rule holds under `key`, once the rule is kept.
  void recordRuleEffect(
      registry::ReadContext& context, Section& keys, std::string_view key, EffectKind kind, std::function<void()> clear
  );

  // An effect preset selector, checked against the presets once every table is read.
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
          addEffectReference(context.effectReferences, path, {*value, node.source()}, kind, false, [&selected] {
            selected.clear();
          });
        },
        [member](const T& defaults) { return nlohmann::ordered_json(defaults.*member); }
    );
  }

  // An effect preset selector on a rule. It is recorded by the rule's accept step, once the rule's place in its array
  // is known.
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
  [[nodiscard]] registry::Field<Config> colorsTable();
  [[nodiscard]] registry::Field<Config> appearanceTable();
  [[nodiscard]] registry::Field<Config> overviewTable();
  [[nodiscard]] registry::Field<Config> hotCornersTable();
  [[nodiscard]] registry::Field<Config> generalTable();
  [[nodiscard]] registry::Field<Config> environmentTable();
  [[nodiscard]] registry::Field<Config> eventsTable();
  [[nodiscard]] registry::Field<Config> workspaceSettingsTable(); // [workspaces]
  [[nodiscard]] registry::Field<Config> screencastTable();
  [[nodiscard]] registry::Field<Config> effectsTable();
  [[nodiscard]] registry::Field<Config> animationTable();
  [[nodiscard]] registry::Field<Config> layoutTable();
  [[nodiscard]] registry::Field<Config> workspaceRulesTable(); // [[workspace]]
  [[nodiscard]] registry::Field<Config> scratchpadTable();
  [[nodiscard]] registry::Field<Config> windowRuleTable();
  [[nodiscard]] registry::Field<Config> layerRuleTable();
  [[nodiscard]] registry::Field<Config> securityContextRuleTable();
  [[nodiscard]] registry::Field<Config> drmTable();
  [[nodiscard]] registry::Field<Config> inputTable();
  [[nodiscard]] registry::Field<Config> outputTable();
  [[nodiscard]] registry::Field<Config> keybindsTable();

  // Checks that need every table read.
  void validateEffectReferences(Config& loaded, std::vector<EffectReference>& references);
  void warnScrollButtonBinds(const Config& loaded);

  // Whether the config asks for a DRM policy. A load that fails then refuses to fall back to defaults.
  [[nodiscard]] bool hasRequestedDrmPolicy(const toml::table& root);

  enum class ConfigParseOutcome : uint8_t {
    Loaded,
    Missing,
    DefaultsAllowed,
    Fatal,
  };

  // Merge `rootPath` with its includes and read every table into `out`, which is left alone unless the result is
  // Loaded. The caller has already found the file and reset the store for this load.
  [[nodiscard]] ConfigParseOutcome parseConfig(Config& out, const std::filesystem::path& rootPath);

} // namespace umbriel
