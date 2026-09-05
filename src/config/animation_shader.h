#pragma once

#include "config/config_diag.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace umbriel {

  class Section;

  // Keep source text in the resolved configuration. File edits then participate
  // in config equality, and render paths never perform filesystem I/O.
  struct AnimationShaderSource {
    std::string code;
    std::filesystem::path file;
    bool operator==(const AnimationShaderSource&) const = default;
  };

  struct AnimationShaderReadResult {
    std::optional<AnimationShaderSource> source;
    // Includes missing files so creating one can trigger another config load.
    std::vector<std::filesystem::path> watchPaths;
  };

  inline constexpr std::size_t kAnimationShaderSourceLimit = 256 * 1024;

  // Reads the shader file path. Relative file paths
  // belong to the TOML value's source file, including when tables were merged.
  [[nodiscard]] AnimationShaderReadResult
  readAnimationShader(Section& section, std::vector<ConfigDiagnostic>& diagnostics);

} // namespace umbriel
