#include "config/overrides.h"

#include "config/config.h"
#include "config/config_registry.h"

#include <cctype>
#include <cstdint>
#include <format>
#include <sstream>
#include <type_traits>
#include <utility>

namespace umbriel::configoverrides {

  std::optional<std::vector<std::string>> parseKey(std::string_view key, std::string& error) {
    const auto invalid = [&] {
      error = std::format("invalid config key: {}", key);
      return std::nullopt;
    };
    const auto skipBlank = [&key](size_t at) {
      while (at < key.size() && (key[at] == ' ' || key[at] == '\t')) {
        ++at;
      }
      return at;
    };
    std::vector<std::string> segments;
    size_t at = 0;
    while (true) {
      at = skipBlank(at);
      if (at >= key.size()) {
        return invalid();
      }
      const char first = key[at];
      if (first == '"' || first == '\'') {
        size_t end = at + 1;
        while (end < key.size() && key[end] != first) {
          if (first == '"' && key[end] == '\\') {
            ++end;
          }
          ++end;
        }
        if (end >= key.size()) {
          return invalid();
        }
        const std::string_view quoted = key.substr(at, end - at + 1);
        if (first == '\'') {
          if (quoted.find_first_of("\r\n") != std::string_view::npos) {
            return invalid();
          }
          segments.emplace_back(quoted.substr(1, quoted.size() - 2));
        } else {
          // The quoted span ends at its closing quote, so the parse below sees exactly one basic string.
          try {
            const toml::table decoded = toml::parse(std::format("k = {}", quoted));
            segments.push_back(decoded["k"].value_or(std::string()));
          } catch (const toml::parse_error&) {
            return invalid();
          }
        }
        at = end + 1;
      } else {
        const size_t start = at;
        while (at < key.size()
               && (std::isalnum(static_cast<unsigned char>(key[at])) != 0 || key[at] == '_' || key[at] == '-')) {
          ++at;
        }
        if (at == start) {
          return invalid();
        }
        segments.emplace_back(key.substr(start, at - start));
      }
      at = skipBlank(at);
      if (at == key.size()) {
        return segments;
      }
      if (key[at] != '.') {
        return invalid();
      }
      ++at;
    }
  }

  std::optional<toml::table> assignment(std::span<const std::string> key, std::string_view value, std::string& error) {
    toml::table parsed;
    try {
      parsed = toml::parse(std::format("value = {}", value));
    } catch (const toml::parse_error& e) {
      error = std::format("invalid TOML value: {}: {}", value, e.description());
      return std::nullopt;
    }
    toml::node* node = parsed.get("value");
    if (parsed.size() != 1 || node == nullptr) {
      error = std::format("expected one TOML value, got: {}", value);
      return std::nullopt;
    }
    toml::table root;
    toml::table* parent = &root;
    for (const std::string& segment : key.first(key.size() - 1)) {
      parent = parent->insert_or_assign(segment, toml::table{}).first->second.as_table();
    }
    node->visit([&](auto&& parsedValue) {
      using T = std::decay_t<decltype(parsedValue)>;
      parent->insert_or_assign(key.back(), T(std::move(parsedValue)));
    });
    return root;
  }

  void overlay(toml::table& base, const toml::table& overlay) {
    for (const auto& [key, value] : overlay) {
      if (const auto* table = value.as_table()) {
        if (auto* baseTable = base.get_as<toml::table>(key.str())) {
          configoverrides::overlay(*baseTable, *table);
          continue;
        }
      }
      value.visit([&](const auto& overlayValue) { base.insert_or_assign(std::string(key.str()), overlayValue); });
    }
  }

  bool remove(toml::table& overrides, std::span<const std::string> key) {
    std::vector<toml::table*> chain{&overrides};
    for (const std::string& segment : key.first(key.size() - 1)) {
      toml::table* child = chain.back()->get_as<toml::table>(segment);
      if (child == nullptr) {
        return false;
      }
      chain.push_back(child);
    }
    if (chain.back()->erase(key.back()) == 0) {
      return false;
    }
    for (size_t depth = chain.size() - 1; depth > 0 && chain[depth]->empty(); --depth) {
      chain[depth - 1]->erase(key[depth - 1]);
    }
    return true;
  }

  bool contains(const toml::table& overrides, std::span<const std::string> key) {
    const toml::table* table = &overrides;
    for (const std::string& segment : key.first(key.size() - 1)) {
      table = table->get_as<toml::table>(segment);
      if (table == nullptr) {
        return false;
      }
    }
    return table->contains(key.back());
  }

  nlohmann::json toJson(const toml::node& node) {
    if (const auto* table = node.as_table()) {
      nlohmann::json object = nlohmann::json::object();
      for (const auto& [key, value] : *table) {
        object[std::string(key.str())] = toJson(value);
      }
      return object;
    }
    if (const auto* array = node.as_array()) {
      nlohmann::json values = nlohmann::json::array();
      for (const toml::node& value : *array) {
        values.push_back(toJson(value));
      }
      return values;
    }
    if (const auto text = node.value_exact<std::string>()) {
      return *text;
    }
    if (const auto integer = node.value_exact<int64_t>()) {
      return *integer;
    }
    if (const auto real = node.value_exact<double>()) {
      return *real;
    }
    if (const auto flag = node.value_exact<bool>()) {
      return *flag;
    }
    // Dates and times keep their TOML spelling.
    std::ostringstream text;
    node.visit([&text](const auto& value) { text << value; });
    return text.str();
  }

  nlohmann::json activeDocument(const toml::table& effective, const Config& config) {
    nlohmann::json document = toJson(effective);
    for (const registry::KeyDescription& entry : registry::describeConfig(config)) {
      if (entry.type == "table" || entry.type == "map" || entry.type == "array_of_tables") {
        continue;
      }
      // Placeholders describe every entry of a collection, not one value; `include` is read before the config.
      if (entry.path.contains("<name>")
          || entry.path.contains("[]")
          || entry.path == "include"
          || entry.path.starts_with("include.")) {
        continue;
      }
      nlohmann::json* parent = &document;
      std::string_view rest = entry.path;
      bool reachable = true;
      for (size_t dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
        nlohmann::json& child = (*parent)[std::string(rest.substr(0, dot))];
        if (child.is_null()) {
          child = nlohmann::json::object();
        }
        if (!child.is_object()) {
          reachable = false;
          break;
        }
        parent = &child;
        rest.remove_prefix(dot + 1);
      }
      if (!reachable) {
        continue;
      }
      // A table the file wrote for a number-or-table key says more than its summary; a key without a reported value
      // keeps what the file holds.
      nlohmann::json& leaf = (*parent)[std::string(rest)];
      if (!leaf.is_object() && !entry.defaultValue.is_null()) {
        leaf = nlohmann::json(entry.defaultValue);
      }
    }
    return document;
  }

  const nlohmann::json* find(const nlohmann::json& document, std::span<const std::string> key) {
    const nlohmann::json* node = &document;
    for (const std::string& segment : key) {
      if (!node->is_object()) {
        return nullptr;
      }
      const auto found = node->find(segment);
      if (found == node->end()) {
        return nullptr;
      }
      node = &*found;
    }
    return node;
  }

} // namespace umbriel::configoverrides
