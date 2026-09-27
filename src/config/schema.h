#pragma once

#include "config/config_registry.h"

#include <optional>
#include <string>
#include <string_view>

namespace umbriel {

  // Options per top-level section, for `umbriel config schema`. Tables, maps, and arrays of tables only group options
  // and are not counted.
  [[nodiscard]] std::string configSchemaSummary(const registry::Descriptions& keys);
  // `{"version": ..., "revision": ..., "options": [...]}`, sorted by path. `version` stays the same across many
  // commits; `revision` (git describe, null when the build had none) tells a consumer the schema may have changed.
  [[nodiscard]] std::string configSchemaJson(
      const registry::Descriptions& keys, std::string_view version, std::optional<std::string_view> revision
  );

} // namespace umbriel
