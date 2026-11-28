#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace umbriel {

  class PrivateStateFileLock {
  public:
    PrivateStateFileLock(const std::filesystem::path& path, std::string_view lockName);
    ~PrivateStateFileLock();

    PrivateStateFileLock(const PrivateStateFileLock&) = delete;
    PrivateStateFileLock& operator=(const PrivateStateFileLock&) = delete;
    explicit operator bool() const { return m_fd >= 0; }

  private:
    int m_fd = -1;
  };

  struct PrivateStateFileContents {
    bool missing = false;
    std::string bytes;
  };

  std::optional<std::string> randomHex(size_t bytes);
  std::optional<PrivateStateFileContents> readPrivateStateFile(const std::filesystem::path& path, size_t maxBytes);
  // Callers serialize read/modify/write transactions with PrivateStateFileLock.
  [[nodiscard]] bool
  writePrivateStateFile(const std::filesystem::path& path, std::string_view bytes, std::string_view temporaryPrefix);

} // namespace umbriel
