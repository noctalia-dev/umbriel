#pragma once

#include "config/config_diag.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {
  class Section;

  // Keep source text in the resolved configuration. File edits then participate
  // in config equality, and render paths never perform filesystem I/O.
  struct ShaderSource {
    std::string code;
    std::filesystem::path file = {};
    bool operator==(const ShaderSource&) const = default;
  };

  struct ShaderReadResult {
    std::optional<ShaderSource> source;
    // Includes missing files so creating one can trigger another config load.
    std::vector<std::filesystem::path> watchPaths;
  };

  inline constexpr std::size_t kShaderSourceLimit = 256 * 1024;

  // Reads the shader file path under `key`. Relative file paths belong to the
  // TOML value's source file, including when tables were merged.
  [[nodiscard]] ShaderReadResult
  readShaderSource(Section& section, std::string_view key, std::vector<ConfigDiagnostic>& diagnostics);

} // namespace umbriel
