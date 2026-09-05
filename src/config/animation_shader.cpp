#include "config/animation_shader.h"

#include "config/section.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {

  AnimationShaderReadResult readAnimationShader(Section& section, std::vector<ConfigDiagnostic>& diagnostics) {
    AnimationShaderReadResult result;
    const toml::node* node = section.take("shader");
    const auto warn = [&](const toml::node& node, std::string message) {
      diagnostics.push_back(makeDiagnostic(ConfigDiagnostic::Severity::Warning, node.source(), std::move(message)));
    };
    if (node == nullptr) {
      return result;
    }
    const auto value = node->value<std::string>();
    if (!node->is_string() || !value) {
      warn(*node, "animation shader path must be a string");
      return result;
    }
    if (value->empty() || value->find('\0') != std::string::npos) {
      warn(*node, "animation shader path must not be empty or contain NUL bytes");
      return result;
    }

    AnimationShaderSource source;
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
    if (metadata.st_size > static_cast<off_t>(kAnimationShaderSourceLimit)) {
      close(fd);
      warn(*node, "animation shader source exceeds 256 KiB");
      return result;
    }

    std::array<char, 4096> chunk{};
    bool failed = false;
    while (source.code.size() <= kAnimationShaderSourceLimit) {
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

    if (source.code.size() > kAnimationShaderSourceLimit) {
      warn(*node, "animation shader source exceeds 256 KiB");
      return result;
    }
    if (source.code.find('\0') != std::string::npos || source.code.find_first_not_of(" \t\r\n") == std::string::npos) {
      warn(*node, "animation shader source must not be blank or contain NUL bytes");
      return result;
    }
    result.source = std::move(source);
    return result;
  }

} // namespace umbriel
