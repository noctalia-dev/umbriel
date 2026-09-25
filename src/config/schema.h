#pragma once

#include "config/schema_recorder.h"

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <vector>

namespace umbriel {

  // How the schema run enters a key it found to be a container.
  struct SchemaShape {
    SchemaType type = SchemaType::Table; // Table, ArrayOfTables, or Map
    std::string sampleKey;               // Map only: the entry name the run fills in
  };

  // Every key the config readers claim, gathered by loading a document that grows until it reaches all of them.
  struct ConfigSchema {
    std::map<std::string, KeySpec> entries;
    std::map<std::string, SchemaShape> shapes;
    // The document the last round loaded: one empty entry for every table, array of tables, and map.
    toml::table skeleton;
    // Overlay paths no reader claimed: a key was renamed or removed and the overlay still describes it.
    std::vector<std::string> staleOverlay;
  };

  // A reader that claims a new nested table every round would never settle. Real nesting is a handful of levels deep.
  inline constexpr int kSchemaMaxRounds = 10;

  // Fails, rather than returning a partial schema, when the document has not settled within kSchemaMaxRounds. The
  // readers log their complaints about the skeleton's incomplete entries; callers silence the console if they care.
  [[nodiscard]] std::expected<ConfigSchema, std::string> buildConfigSchema();
  // Options per top-level section, for `umbriel config schema`. Tables, maps, and arrays of tables only group options
  // and are not counted.
  [[nodiscard]] std::string configSchemaSummary(const ConfigSchema& schema);
  // `{"version": ..., "revision": ..., "options": [...]}`, sorted by path. Build info lives in the executable, so the
  // caller passes it. `version` stays the same across many commits; `revision` (git describe, null when the build had
  // none) is what tells a consumer that the binary, and so possibly the schema, changed.
  [[nodiscard]] std::string
  configSchemaJson(const ConfigSchema& schema, std::string_view version, std::optional<std::string_view> revision);

  // Load `document` through the real loader (loadConfig) from a private temporary file. Returns false when the file
  // could not be written.
  bool loadConfigDocument(const toml::table& document);
  // Write `value` at schema `path` into `document`, resolving `name[]` to the array's first entry and `<name>` to its
  // map's sample key. Returns false when the document does not reach it.
  bool setSchemaValue(
      toml::table& document, const std::map<std::string, SchemaShape>& shapes, std::string_view path,
      const SchemaValue& value
  );

} // namespace umbriel
