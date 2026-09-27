#include "config/effects.h"

#include "config/section.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {

  ShaderReadResult
  readShaderSource(Section& section, std::string_view key, std::vector<ConfigDiagnostic>& diagnostics) {
    ShaderReadResult result;
    const toml::node* node = section.take(key);
    const auto warn = [&](const toml::node& node, std::string message) {
      diagnostics.push_back(makeDiagnostic(ConfigDiagnostic::Severity::Warning, node.source(), std::move(message)));
    };
    if (node == nullptr) {
      return result;
    }
    const auto value = node->value<std::string>();
    if (!node->is_string() || !value) {
      warn(*node, "shader path must be a string");
      return result;
    }
    if (value->empty() || value->contains('\0')) {
      warn(*node, "shader path must not be empty or contain NUL bytes");
      return result;
    }

    ShaderSource source;
    source.file = *value;
    if (source.file.is_relative()) {
      if (node->source().path == nullptr || node->source().path->empty()) {
        warn(*node, "relative shader requires a config source path");
        return result;
      }
      source.file = std::filesystem::path(*node->source().path).parent_path() / source.file;
    }
    source.file = source.file.lexically_normal();
    result.watchPaths.push_back(source.file);

    // O_NONBLOCK avoids hanging on a FIFO before fstat can reject it. Follow
    // symlinks normally, but only read regular files and enforce a size cap
    // while reading, since the file can grow after the metadata check.
    const int fd = open(source.file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      warn(*node, std::format("cannot read shader file '{}': {}", source.file.string(), std::strerror(errno)));
      return result;
    }
    struct stat metadata{};
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
      close(fd);
      warn(*node, std::format("shader file '{}' must be a readable regular file", source.file.string()));
      return result;
    }
    if (metadata.st_size > static_cast<off_t>(kShaderSourceLimit)) {
      close(fd);
      warn(*node, "shader source exceeds 256 KiB");
      return result;
    }

    std::array<char, 4096> chunk{};
    bool failed = false;
    while (source.code.size() <= kShaderSourceLimit) {
      const ssize_t count = read(fd, chunk.data(), chunk.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        warn(*node, std::format("cannot read shader file '{}': {}", source.file.string(), std::strerror(errno)));
        failed = true;
        break;
      }
      if (count == 0) {
        break;
      }
      source.code.append(chunk.data(), static_cast<std::size_t>(count));
    }
    close(fd);
    if (failed) {
      return result;
    }

    if (source.code.size() > kShaderSourceLimit) {
      warn(*node, "shader source exceeds 256 KiB");
      return result;
    }
    if (source.code.contains('\0') || source.code.find_first_not_of(" \t\r\n") == std::string::npos) {
      warn(*node, "shader source must not be blank or contain NUL bytes");
      return result;
    }
    result.source = std::move(source);
    return result;
  }

  std::optional<EffectKind> parseEffectKind(std::string_view text) {
    if (text == "animation") {
      return EffectKind::Animation;
    }
    if (text == "border") {
      return EffectKind::Border;
    }
    if (text == "window") {
      return EffectKind::Window;
    }
    if (text == "screen") {
      return EffectKind::Screen;
    }
    if (text == "cursor") {
      return EffectKind::Cursor;
    }
    return std::nullopt;
  }

  std::string_view effectKindName(EffectKind kind) {
    switch (kind) {
    case EffectKind::Animation:
      return "animation";
    case EffectKind::Border:
      return "border";
    case EffectKind::Window:
      return "window";
    case EffectKind::Screen:
      return "screen";
    case EffectKind::Cursor:
      return "cursor";
    }
    return "effect";
  }

  namespace {
    // "a border preset", "an animation preset".
    std::string presetPhrase(EffectKind kind) {
      const std::string_view name = effectKindName(kind);
      return std::format("{} {} preset", std::string_view("aeiou").contains(name.front()) ? "an" : "a", name);
    }
  } // namespace

  const EffectPreset* findEffectPreset(const Effects& effects, std::string_view name) {
    const auto preset = std::ranges::find(effects.presets, name, &EffectPreset::name);
    return preset != effects.presets.end() ? &*preset : nullptr;
  }

  std::optional<std::string>
  effectReferenceError(const Effects& effects, std::string_view name, EffectKind kind, bool allowOff) {
    if (name.empty() || (allowOff && name == kEffectOff)) {
      return std::nullopt;
    }
    const EffectPreset* preset = findEffectPreset(effects, name);
    if (preset == nullptr) {
      return std::format("unknown effect '{}'", name);
    }
    if (preset->kind != kind) {
      return std::format("effect '{}' is {}, not {}", name, presetPhrase(preset->kind), presetPhrase(kind));
    }
    return std::nullopt;
  }

} // namespace umbriel
