#pragma once

#include "config/config_diag.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace umbriel {

  // Reads one table of a config file, remembering which keys it was asked for. The point of remembering is the
  // unknown-key warning. Every section used to carry a hand-written list of its own key names beside the code that
  // reads them, so adding a setting meant editing two places and forgetting one meant either a valid key was warned
  // about or a typo was silently accepted. Here the list *is* the set of keys the reader asked for, so it cannot drift.
  // The warning is emitted from the destructor, so a reader that returns early still reports. Diagnostics go to a
  // caller-supplied vector rather than to a global, which is also what lets this be tested without a compositor.
  class Section {
  public:
    Section(const toml::table& table, std::string name, std::vector<ConfigDiagnostic>& diagnostics);
    ~Section();

    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    Section(Section&&) = delete;
    Section& operator=(Section&&) = delete;

    // Numbers are clamped into range, and clamping is reported: silently accepting a value the compositor will not
    // honour is how a user ends up believing a setting does nothing.
    Section& integer(std::string_view key, int minimum, int maximum, int& target);
    Section& integer(std::string_view key, int minimum, int maximum, std::optional<int>& target);
    Section& real(std::string_view key, double minimum, double maximum, double& target);
    Section& real(std::string_view key, double minimum, double maximum, std::optional<double>& target);
    Section& text(std::string_view key, std::string& target);
    Section& boolean(std::string_view key, bool& target);
    Section& boolean(std::string_view key, std::optional<bool>& target);
    Section& color(std::string_view key, std::array<float, 4>& target);
    Section& color(std::string_view key, std::optional<std::array<float, 4>>& target);
    // An array of non-empty strings. A single bad element rejects the whole array: a half-applied autostart list is
    // worse than none, because the user cannot tell which entries ran.
    Section& strings(std::string_view key, std::vector<std::string>& target);
    // Descend into a nested table, if it is there and is a table. `fn` takes a `Section&`. Taking a callback rather
    // than returning a Section keeps this type immovable, which is what makes the destructor-based warning safe.
    template <typename F> Section& sub(std::string_view key, F&& fn) {
      const toml::table* nested = nestedTable(key);
      if (nested != nullptr) {
        Section child(*nested, qualified(key), m_diagnostics);
        fn(child);
      }
      return *this;
    }

    // Claim a key and hand back its raw node, for parsing that does not fit the shapes above. Preferred over `node` +
    // `custom`: fetching is what marks the key known, so the two cannot come apart.
    [[nodiscard]] const toml::node* take(std::string_view key) { return claim(key); }
    // The raw node without claiming, when the key is claimed elsewhere.
    [[nodiscard]] const toml::node* node(std::string_view key) const { return m_table.get(key); }
    // The table itself, for bespoke readers that predate this class.
    [[nodiscard]] const toml::table& table() const { return m_table; }

    // Claim a key that bespoke code reads (arrays, keybind lists, rule tables) so
    // it is not reported as unknown.
    Section& custom(std::string_view key);

    // Suppress the unknown-key report entirely, for tables whose keys are
    // user-chosen names rather than a fixed vocabulary.
    Section& freeform();

    // Whether every key in the table has been claimed, for readers that reject
    // an entry on a stray key instead of only warning.
    [[nodiscard]] bool allKeysKnown() const;

  private:
    [[nodiscard]] std::string qualified(std::string_view key) const;
    const toml::node* claim(std::string_view key);
    [[nodiscard]] const toml::table* nestedTable(std::string_view key);
    void warn(const toml::node& node, std::string message);

    const toml::table& m_table;
    std::string m_name;
    std::vector<ConfigDiagnostic>& m_diagnostics;
    std::vector<std::string> m_seen;
    bool m_freeform = false;
  };

  // Read `name` from `table` if present. Warns and skips when the key exists but is not a table. Returns whether `fn`
  // ran. `displayName` overrides how the section is named in diagnostics, for tables nested under a per-entry context
  // (a workspace's own `layout` block reports as `workspace[2].layout`, not `layout`).
  template <typename F>
  bool readSection(
      const toml::table& table, std::string_view name, std::vector<ConfigDiagnostic>& diagnostics, F&& fn,
      std::string_view displayName = {}
  ) {
    const toml::node* node = table.get(name);
    if (node == nullptr) {
      return false;
    }
    const toml::table* section = node->as_table();
    if (section == nullptr) {
      ConfigDiagnostic diag;
      diag.severity = ConfigDiagnostic::Severity::Warning;
      diag.message =
          std::string("ignoring ") + std::string(displayName.empty() ? name : displayName) + " (expected table)";
      const auto& src = node->source();
      diag.line = src.begin.line;
      diag.column = src.begin.column;
      if (src.path != nullptr) {
        diag.file = *src.path;
      }
      diagnostics.push_back(std::move(diag));
      return false;
    }
    Section reader(*section, std::string(displayName.empty() ? name : displayName), diagnostics);
    fn(reader);
    return true;
  }

} // namespace umbriel
