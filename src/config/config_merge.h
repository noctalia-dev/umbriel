#pragma once

#include "config/config_diag.h"
#include "core/toml.h"

#include <filesystem>
#include <string>
#include <vector>

namespace umbriel::configmerge {

  struct EffectDeclaration {
    std::string name;
    toml::source_region source;
  };

  struct MergeResult {
    toml::table merged;
    std::vector<std::filesystem::path> loadedFiles;
    std::vector<ConfigDiagnostic> diagnostics;
    bool hadError = false;
    bool missingIncludes = false;
    bool missingOptionalIncludes = false;
    // Source order within a file, include expansion order across files. Captured before merging tables.
    std::vector<EffectDeclaration> presets;
    std::vector<EffectDeclaration> pools;
  };

  [[nodiscard]] MergeResult mergeWithIncludes(const std::filesystem::path& rootFile);
  void deepMerge(toml::table& base, toml::table&& overlay);

} // namespace umbriel::configmerge
