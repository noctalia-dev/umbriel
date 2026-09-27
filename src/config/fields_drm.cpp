// [drm]: GPUs the session must leave alone. Read by hand: unknown keys here are errors, not warnings, so a
// misspelled exclusion cannot silently claim a GPU.

#include "config/fields.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    std::optional<std::string> normalizePciAddress(std::string_view value) {
      constexpr std::array<size_t, 3> separators{4, 7, 10};
      if (value.size() != 12
          || value[separators[0]] != ':'
          || value[separators[1]] != ':'
          || value[separators[2]] != '.'
          || (value[8] != '0' && value[8] != '1')
          || value.back() < '0'
          || value.back() > '7') {
        return std::nullopt;
      }
      for (size_t index = 0; index < value.size(); ++index) {
        if (std::ranges::find(separators, index) != separators.end()) {
          continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
          return std::nullopt;
        }
      }
      return lowercase(value);
    }

    std::optional<std::string> readDrmPath(const toml::node& node, std::string_view context) {
      const auto value = node.value<std::string>();
      if (!value) {
        errorAt(node.source(), "{} must be a string", context);
        return std::nullopt;
      }
      if (value->empty()) {
        errorAt(node.source(), "{} cannot be empty", context);
        return std::nullopt;
      }
      if (value->contains('\0')) {
        errorAt(node.source(), "{} cannot contain NUL", context);
        return std::nullopt;
      }
      const std::filesystem::path path(*value);
      if (!path.is_absolute()) {
        errorAt(node.source(), R"({} must be an absolute path (got "{}"))", context, *value);
        return std::nullopt;
      }
      // Keep the exact spelling: lexical normalization changes the meaning of
      // `..` when an earlier path component is a symlink.
      return value;
    }

    std::optional<std::string> readDrmPciAddress(const toml::node& node, std::string_view context) {
      const auto value = node.value<std::string>();
      if (!value) {
        errorAt(node.source(), "{} entries must be strings", context);
        return std::nullopt;
      }
      const auto address = normalizePciAddress(*value);
      if (!address) {
        errorAt(
            node.source(), R"(invalid {} entry "{}" (expected domain:bus:slot.function, for example 0000:01:00.0))",
            context, *value
        );
      }
      return address;
    }

    template <typename Parse>
    void readDrmSelectorList(
        const toml::node& node, std::string_view context, std::vector<std::string>& target, Parse parse
    ) {
      const toml::array* values = node.as_array();
      if (values == nullptr) {
        errorAt(node.source(), "{} must be an array of strings", context);
        return;
      }
      for (const toml::node& entry : *values) {
        auto value = parse(entry, context);
        if (!value) {
          continue;
        }
        if (std::ranges::find(target, *value) != target.end()) {
          warnAt(entry.source(), R"(ignoring duplicate {} entry "{}")", context, *value);
          continue;
        }
        target.push_back(std::move(*value));
      }
    }

    void readDrm(Section& root, Config& loaded) {
      const toml::node* node = root.take("drm");
      if (node == nullptr) {
        return;
      }
      const toml::table* table = node->as_table();
      if (table == nullptr) {
        errorAt(node->source(), "drm must be a table");
        return;
      }

      for (const auto& [key, value] : *table) {
        if (key == "ignored_devices") {
          readDrmSelectorList(value, "drm.ignored_devices", loaded.drm.ignoredDevices, readDrmPath);
        } else if (key == "ignored_pci_addresses") {
          readDrmSelectorList(value, "drm.ignored_pci_addresses", loaded.drm.ignoredPciAddresses, readDrmPciAddress);
        } else {
          errorAt(value.source(), "unknown key drm.{}", key.str());
        }
      }
    }

  } // namespace

  registry::Field<Config> drmTable() {
    using registry::KeyDescription;
    return registry::handRead<Config>(
        "drm", KeyDescription("table"),
        [] {
          registry::Descriptions keys{
              KeyDescription("string_array").withFormat("path"),
              KeyDescription("string_array").withFormat("pci_address"),
          };
          keys[0].path = ".ignored_devices";
          keys[1].path = ".ignored_pci_addresses";
          return keys;
        }(),
        [](Section& s, Config& c, registry::ReadContext&) { readDrm(s, c); }
    );
  }

  bool hasRequestedDrmPolicy(const toml::table& root) {
    const toml::node* node = root.get("drm");
    if (node == nullptr) {
      return false;
    }
    const toml::table* table = node->as_table();
    return table == nullptr || !table->empty();
  }

} // namespace umbriel
