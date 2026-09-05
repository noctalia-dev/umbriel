#pragma once

#include "config/config_diag.h"
#include "core/toml.h"

#include <filesystem>
#include <string>
#include <vector>

namespace umbriel::configmerge {

  struct MergeResult {
    toml::table merged;
    std::vector<std::filesystem::path> loadedFiles;
    std::vector<ConfigDiagnostic> diagnostics;
    bool hadError = false;
    bool missingIncludes = false;
  };

  [[nodiscard]] MergeResult mergeWithIncludes(const std::filesystem::path& rootFile);
  void deepMerge(toml::table& base, const toml::table& overlay);
  void deepMerge(toml::table& base, toml::table&& overlay);

} // namespace umbriel::configmerge
