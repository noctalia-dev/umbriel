#pragma once

#include "config/config_diag.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  class Section;

  enum class EffectKind : std::uint8_t { Animation, Border, Window, Screen, Cursor };

  [[nodiscard]] std::optional<EffectKind> parseEffectKind(std::string_view text);
  [[nodiscard]] std::string_view effectKindName(EffectKind kind);

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

  // The reserved selector value that disables a default per window or output.
  inline constexpr std::string_view kEffectOff = "off";

  struct BorderLight {
    int spread = 80;        // 1-256 logical px
    float intensity = 1.0F; // 0-4
    float threshold = 0.5F; // 0-1
    bool operator==(const BorderLight&) const = default;
  };

  struct EffectPreset {
    std::string name;
    EffectKind kind = EffectKind::Animation;
    // Empty code means the file was missing or unreadable: the preset exists so
    // references resolve, but it renders plainly.
    ShaderSource shader = {};
    bool palette = false;
    int padding = 0;          // border: 0-1024 logical px
    float speed = 1.0F;       // border: 0-10
    bool animated = true;     // border
    std::string overlay = {}; // NOLINT(readability-redundant-member-init) border: names a window preset
    std::optional<BorderLight> light = std::nullopt; // border
    int radius = 0;                                  // cursor: 0-4096, 0 = whole output
    [[nodiscard]] bool inert() const { return shader.code.empty(); }
    bool operator==(const EffectPreset&) const = default;
  };

  struct Effects {
    std::vector<EffectPreset> presets;
    std::string border; // "" = off
    std::string window;
    std::string screen;
    std::string cursor;
    int maxFps = 0; // 0-240, 0 follows the refresh rate
    bool inCapture = false;
    bool operator==(const Effects&) const = default;
  };

  [[nodiscard]] const EffectPreset* findEffectPreset(const Effects& effects, std::string_view name);
  // "" and, when allowOff, "off" are valid; otherwise the preset must exist with
  // `kind`. Returns the diagnostic text on failure.
  [[nodiscard]] std::optional<std::string>
  effectReferenceError(const Effects& effects, std::string_view name, EffectKind kind, bool allowOff);

} // namespace umbriel
