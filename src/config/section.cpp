#include "config/section.h"

#include "config/value_parse.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace umbriel {

  namespace {

    // Clamp and report. Shared by the int and double readers, which were
    // otherwise identical four times over (plain and optional, each type).
    template <typename T> struct Clamped {
      T value;
      bool changed;
    };

    template <typename T> Clamped<T> clampTo(T value, T minimum, T maximum) {
      const T used = std::clamp(value, minimum, maximum);
      return {.value = used, .changed = used != value};
    }

  } // namespace

  Section::Section(const toml::table& table, std::string name, std::vector<ConfigDiagnostic>& diagnostics)
      : m_table(table), m_name(std::move(name)), m_diagnostics(diagnostics) {}

  Section::~Section() {
    if (m_freeform) {
      return;
    }
    for (const auto& [key, value] : m_table) {
      if (std::ranges::find(m_seen, key.str()) == m_seen.end()) {
        warn(value, std::format("unknown key {}", qualified(key.str())));
      }
    }
  }

  std::string Section::qualified(std::string_view key) const {
    return m_name.empty() ? std::string(key) : std::format("{}.{}", m_name, key);
  }

  const toml::node* Section::claim(std::string_view key) {
    m_seen.emplace_back(key);
    return m_table.get(key);
  }

  const toml::table* Section::nestedTable(std::string_view key) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return nullptr;
    }
    const toml::table* nested = node->as_table();
    if (nested == nullptr) {
      warn(*node, std::format("ignoring {} (expected table)", qualified(key)));
    }
    return nested;
  }

  void Section::warn(const toml::node& node, std::string message) {
    ConfigDiagnostic diag;
    diag.severity = ConfigDiagnostic::Severity::Warning;
    diag.message = std::move(message);
    const auto& src = node.source();
    diag.line = src.begin.line;
    diag.column = src.begin.column;
    if (src.path != nullptr) {
      diag.file = *src.path;
    }
    m_diagnostics.push_back(std::move(diag));
  }

  Section& Section::custom(std::string_view key) {
    m_seen.emplace_back(key);
    return *this;
  }

  Section& Section::freeform() {
    m_freeform = true;
    return *this;
  }

  bool Section::allKeysKnown() const {
    return std::ranges::all_of(m_table, [this](const auto& item) {
      return std::ranges::find(m_seen, item.first.str()) != m_seen.end();
    });
  }

  Section& Section::integer(std::string_view key, int minimum, int maximum, std::optional<int>& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    const auto value = node->value<std::int64_t>();
    if (!value) {
      warn(*node, std::format("ignoring {} (expected integer)", qualified(key)));
      return *this;
    }
    const auto used = clampTo(*value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum));
    if (used.changed) {
      warn(*node, std::format("{} = {} out of range, clamped to {}", qualified(key), *value, used.value));
    }
    target = static_cast<int>(used.value);
    return *this;
  }

  Section& Section::integer(std::string_view key, int minimum, int maximum, int& target) {
    std::optional<int> parsed;
    integer(key, minimum, maximum, parsed);
    if (parsed) {
      target = *parsed;
    }
    return *this;
  }

  Section& Section::real(std::string_view key, double minimum, double maximum, std::optional<double>& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    const auto value = node->value<double>();
    if (!value || std::isnan(*value)) {
      warn(*node, std::format("ignoring {} (expected number)", qualified(key)));
      return *this;
    }
    const auto used = clampTo(*value, minimum, maximum);
    if (used.changed) {
      warn(*node, std::format("{} = {} out of range, clamped to {}", qualified(key), *value, used.value));
    }
    target = used.value;
    return *this;
  }

  Section& Section::real(std::string_view key, double minimum, double maximum, double& target) {
    std::optional<double> parsed;
    real(key, minimum, maximum, parsed);
    if (parsed) {
      target = *parsed;
    }
    return *this;
  }

  Section& Section::text(std::string_view key, std::string& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    const auto value = node->value<std::string>();
    if (!value) {
      warn(*node, std::format("ignoring {} (expected string)", qualified(key)));
      return *this;
    }
    target = *value;
    return *this;
  }

  Section& Section::boolean(std::string_view key, std::optional<bool>& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    if (!node->is_boolean()) {
      warn(*node, std::format("ignoring {} (expected boolean)", qualified(key)));
      return *this;
    }
    target = node->value<bool>();
    return *this;
  }

  Section& Section::boolean(std::string_view key, bool& target) {
    std::optional<bool> parsed;
    boolean(key, parsed);
    if (parsed) {
      target = *parsed;
    }
    return *this;
  }

  Section& Section::color(std::string_view key, std::array<float, 4>& target) {
    std::optional<std::array<float, 4>> parsed;
    color(key, parsed);
    if (parsed) {
      target = *parsed;
    }
    return *this;
  }

  Section& Section::color(std::string_view key, std::optional<std::array<float, 4>>& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    const auto value = node->value<std::string>();
    if (!value) {
      warn(*node, std::format("ignoring {} (expected color string)", qualified(key)));
      return *this;
    }
    std::array<float, 4> parsed{};
    if (!parseColor(*value, parsed)) {
      warn(*node, std::format("ignoring {} (invalid color '{}')", qualified(key), *value));
      return *this;
    }
    target = parsed;
    return *this;
  }

  Section& Section::strings(std::string_view key, std::vector<std::string>& target) {
    const toml::node* node = claim(key);
    if (node == nullptr) {
      return *this;
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
      warn(*node, std::format("ignoring {} (expected array of strings)", qualified(key)));
      return *this;
    }
    std::vector<std::string> parsed;
    parsed.reserve(array->size());
    for (const auto& entry : *array) {
      const auto value = entry.value<std::string>();
      if (!value) {
        warn(entry, std::format("ignoring {} (expected array of strings)", qualified(key)));
        return *this;
      }
      if (value->empty()) {
        warn(entry, std::format("ignoring empty {} entry", qualified(key)));
        continue;
      }
      parsed.push_back(*value);
    }
    target = std::move(parsed);
    return *this;
  }

} // namespace umbriel
