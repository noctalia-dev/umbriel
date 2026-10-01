#pragma once

#include "config/shader_source.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace umbriel::scene_experiment {

  // Typed scene contracts remain internal until their runtime composition and
  // lifecycle gates pass. Parsing them does not announce a stable author ABI.
  enum class Scope : std::uint8_t { WorkspacePair };
  enum class Stage : std::uint8_t { Common, Fragment, Count };
  enum class Binding : std::uint8_t { WorkspaceSwitch, Other };
  inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);
  inline constexpr std::size_t kAggregateSourceLimit = 512 * 1024;
  inline constexpr std::size_t kParameterLimit = 32;
  inline constexpr std::size_t kParameterNameLimit = 31;

  struct Sources {
    Scope scope = Scope::WorkspacePair;
    std::array<std::optional<ShaderSource>, kStageCount> stages;
    [[nodiscard]] bool complete() const { return stages[static_cast<std::size_t>(Stage::Fragment)].has_value(); }
    bool operator==(const Sources&) const = default;
  };

  struct SourceReadResult {
    // Atomic: missing/invalid required or declared optional stages invalidate
    // the whole source set. Watches survive failure so repair can be detected.
    std::optional<Sources> sources;
    std::vector<std::filesystem::path> watchPaths;
  };

  [[nodiscard]] std::optional<Scope> parseScope(std::string_view text);
  [[nodiscard]] std::string_view scopeName(Scope scope);
  [[nodiscard]] bool acceptsBinding(Scope scope, Binding binding);
  [[nodiscard]] SourceReadResult readSources(Section& section, Scope scope, std::vector<ConfigDiagnostic>& diagnostics);

  struct Parameter {
    std::string name;
    // One scalar/vector per name, no arrays or matrices in this experiment.
    unsigned components = 1;
    std::array<float, 4> values{};
    bool operator==(const Parameter&) const = default;
  };

  struct Preset {
    Sources sources;
    std::vector<Parameter> parameters;
    bool operator==(const Preset&) const = default;
  };

  [[nodiscard]] std::optional<std::string> validateParameters(std::span<const Parameter> parameters);
  // A bad entry rejects the whole table so declarations and values stay atomic.
  [[nodiscard]] std::optional<std::vector<Parameter>> readParameters(Section& section);

} // namespace umbriel::scene_experiment
