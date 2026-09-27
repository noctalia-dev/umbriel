#pragma once

#include "config/config_diag.h"
#include "core/toml.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace umbriel::configmerge {

  struct MergeResult {
    toml::table merged;
    std::vector<std::filesystem::path> loadedFiles;
    std::vector<ConfigDiagnostic> diagnostics;
    bool hadError = false;
    bool missingIncludes = false;
    bool missingOptionalIncludes = false;
    // Defining file of every effects.preset.<name> table, for duplicate detection across files.
    std::map<std::string, std::string> presetFiles;
  };

  [[nodiscard]] MergeResult mergeWithIncludes(const std::filesystem::path& rootFile);
  void deepMerge(toml::table& base, toml::table&& overlay);

} // namespace umbriel::configmerge
