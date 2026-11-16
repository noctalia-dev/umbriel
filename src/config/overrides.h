#pragma once

// Runtime overrides: config values set over IPC that every load lays over the config file and its includes.

#include "core/toml.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {
  struct Config;
} // namespace umbriel

namespace umbriel::configoverrides {

  // The segments of a TOML dotted key: `input.touchpad.enabled`, or `output."HDMI-A-1".scale` with a quoted segment.
  std::optional<std::vector<std::string>> parseKey(std::string_view key, std::string& error);
  // A table holding the one TOML value `value` at `key`, as `key = value` would in a file.
  std::optional<toml::table> assignment(std::span<const std::string> key, std::string_view value, std::string& error);
  // Lays `overlay` over `base`: tables merge key by key; any other value, arrays included, replaces what `base` holds.
  void overlay(toml::table& base, const toml::table& overlay);
  // Removes `key` and every table that removal leaves empty. False when `overrides` holds nothing at `key`.
  bool remove(toml::table& overrides, std::span<const std::string> key);
  bool contains(const toml::table& overrides, std::span<const std::string> key);
  nlohmann::json toJson(const toml::node& node);
  // The active configuration as one document: `effective` (file, includes, and overrides as last loaded) with every
  // declared key's active value from `config` laid over it, so a key the file leaves out reports its built-in value.
  nlohmann::json activeDocument(const toml::table& effective, const Config& config);
  // The node at `key` in `document`, or null when there is none.
  const nlohmann::json* find(const nlohmann::json& document, std::span<const std::string> key);

} // namespace umbriel::configoverrides
