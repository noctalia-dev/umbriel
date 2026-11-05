#pragma once

#include "core/toml.h"

#include <cstdint>
#include <format>
#include <string>

namespace umbriel {

  struct ConfigDiagnostic {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Warning;
    std::string message; // text WITHOUT location prefix
    std::string file;    // empty when unknown
    uint32_t line = 0;   // 1-based; 0 = unknown
    uint32_t column = 0;

    // "path:12:5" or "" when file empty.
    std::string location() const {
      if (file.empty()) {
        return {};
      }
      return std::format("{}:{}:{}", file, line, column);
    }
  };

  // Copies file/line/column out of a toml source region (path may be null).
  inline ConfigDiagnostic
  makeDiagnostic(ConfigDiagnostic::Severity severity, const toml::source_region& source, std::string message) {
    ConfigDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.message = std::move(message);
    diagnostic.line = source.begin.line;
    diagnostic.column = source.begin.column;
    if (source.path != nullptr) {
      diagnostic.file = *source.path;
    }
    return diagnostic;
  }

} // namespace umbriel
