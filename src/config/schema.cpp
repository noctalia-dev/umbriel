#include "config/schema.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <vector>

namespace umbriel {

  namespace {

    bool groupsOptions(std::string_view type) { return type == "table" || type == "map" || type == "array_of_tables"; }

    nlohmann::ordered_json bound(std::string_view type, double value) {
      const bool integral =
          type == "int" || type == "int_array" || type == "int_or_string" || type == "int_or_string_array";
      return integral ? nlohmann::ordered_json(static_cast<std::int64_t>(value)) : nlohmann::ordered_json(value);
    }

  } // namespace

  std::string configSchemaSummary(const registry::Descriptions& keys) {
    std::map<std::string, int> perSection;
    int total = 0;
    for (const registry::KeyDescription& key : keys) {
      if (groupsOptions(key.type)) {
        continue;
      }
      perSection[key.path.substr(0, key.path.find_first_of(".["))] += 1;
      ++total;
    }
    std::string out =
        std::format("{} options in {} sections\n\n{:<24}{:>7}\n", total, perSection.size(), "section", "options");
    for (const auto& [section, count] : perSection) {
      out += std::format("{:<24}{:>7}\n", section, count);
    }
    return out;
  }

  std::string configSchemaJson(
      const registry::Descriptions& keys, std::string_view version, std::optional<std::string_view> revision
  ) {
    std::vector<const registry::KeyDescription*> sorted;
    sorted.reserve(keys.size());
    for (const registry::KeyDescription& key : keys) {
      sorted.push_back(&key);
    }
    std::ranges::sort(sorted, {}, &registry::KeyDescription::path);

    nlohmann::ordered_json options = nlohmann::ordered_json::array();
    for (const registry::KeyDescription* key : sorted) {
      nlohmann::ordered_json option;
      option["path"] = key->path;
      option["type"] = key->type;
      if (!key->values.empty()) {
        option["values"] = key->values;
      }
      if (key->min) {
        option["min"] = bound(key->type, *key->min);
      }
      if (key->max) {
        option["max"] = bound(key->type, *key->max);
      }
      if (!key->format.empty()) {
        option["format"] = key->format;
      }
      if (!key->defaultValue.is_null()) {
        option["default"] = key->defaultValue;
      }
      options.push_back(std::move(option));
    }
    nlohmann::ordered_json root;
    root["version"] = version;
    root["revision"] = revision ? nlohmann::ordered_json(*revision) : nlohmann::ordered_json(nullptr);
    root["options"] = std::move(options);
    return root.dump(2) + "\n";
  }

} // namespace umbriel
