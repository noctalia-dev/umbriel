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

  // Reads one table of a config file, remembering which keys it was asked for; any other key in the table produces an
  // unknown-key warning. The warning is emitted from the destructor, so a reader that returns early still reports.
  // Diagnostics go to a caller-supplied vector, which lets this be tested without a compositor.
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
    Section& text(std::string_view key, std::optional<std::string>& target);
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

    // Claim a key and hand back its raw node, for parsing that does not fit the shapes above. Fetching is what marks
    // the key known, so the claim and the read cannot come apart.
    [[nodiscard]] const toml::node* take(std::string_view key) { return claim(key); }
    // The raw node without claiming, when the key is claimed elsewhere.
    [[nodiscard]] const toml::node* node(std::string_view key) const { return m_table.get(key); }
    // The table itself, for readers that walk user-chosen names.
    [[nodiscard]] const toml::table& table() const { return m_table; }

    // Suppress the unknown-key report entirely, for tables whose keys are
    // user-chosen names rather than a fixed vocabulary.
    Section& freeform();

    // Whether every key in the table has been claimed, for readers that reject
    // an entry on a stray key instead of only warning.
    [[nodiscard]] bool allKeysKnown() const;

    // This table's path, as diagnostics name it.
    [[nodiscard]] const std::string& name() const { return m_name; }
    // `key` as diagnostics name it, qualified by this table's path.
    [[nodiscard]] std::string qualified(std::string_view key) const;
    // Report against `node` into this table's diagnostics, for readers built on top of this class.
    void warn(const toml::node& node, std::string message);
    void error(const toml::node& node, std::string message);
    [[nodiscard]] std::vector<ConfigDiagnostic>& diagnostics() const { return m_diagnostics; }

  private:
    const toml::node* claim(std::string_view key);
    [[nodiscard]] const toml::table* nestedTable(std::string_view key);
    void report(ConfigDiagnostic::Severity severity, const toml::node& node, std::string message);

    const toml::table& m_table;
    std::string m_name;
    std::vector<ConfigDiagnostic>& m_diagnostics;
    std::vector<std::string> m_seen;
    bool m_freeform = false;
  };

} // namespace umbriel
