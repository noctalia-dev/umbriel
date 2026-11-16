#include "core/private_state_file.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {
  namespace {
    bool validName(std::string_view name) {
      return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0');
    }

    bool writeAll(int fd, std::string_view bytes) {
      while (!bytes.empty()) {
        const ssize_t count = write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        bytes.remove_prefix(static_cast<size_t>(count));
      }
      return true;
    }
  } // namespace

  PrivateStateFileLock::PrivateStateFileLock(const std::filesystem::path& path, std::string_view lockName) {
    if (path.empty() || !path.is_absolute() || !validName(lockName)) {
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return;
    }
    const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (directory < 0) {
      return;
    }
    if (fstat(directory, &info) == 0 && info.st_uid == geteuid() && fchmod(directory, 0700) == 0) {
      const std::string name(lockName);
      m_fd = openat(directory, name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    close(directory);
    if (m_fd >= 0
        && (fstat(m_fd, &info) != 0
            || !S_ISREG(info.st_mode)
            || info.st_uid != geteuid()
            || (info.st_mode & 077) != 0
            || flock(m_fd, LOCK_EX | LOCK_NB) != 0)) {
      close(m_fd);
      m_fd = -1;
    }
  }

  PrivateStateFileLock::~PrivateStateFileLock() {
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  std::optional<std::string> randomHex(size_t bytes) {
    std::array<unsigned char, 32> data{};
    if (bytes > data.size()) {
      return std::nullopt;
    }
    size_t offset = 0;
    while (offset < bytes) {
      const ssize_t count = getrandom(data.data() + offset, bytes - offset, 0);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return std::nullopt;
      }
      offset += static_cast<size_t>(count);
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (size_t index = 0; index < bytes; ++index) {
      result += digits[data[index] >> 4];
      result += digits[data[index] & 15];
    }
    return result;
  }

  std::optional<PrivateStateFileContents> readPrivateStateFile(const std::filesystem::path& path, size_t maxBytes) {
    if (path.empty() || !path.is_absolute()) {
      return std::nullopt;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      if (errno == ENOENT) {
        return PrivateStateFileContents{.missing = true, .bytes = {}};
      }
      return std::nullopt;
    }
    struct stat info{};
    const bool safe = fstat(fd, &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == geteuid()
        && (info.st_mode & 077) == 0
        && info.st_size >= 0
        && static_cast<uint64_t>(info.st_size) <= maxBytes;
    if (!safe) {
      close(fd);
      return std::nullopt;
    }
    PrivateStateFileContents contents{.missing = false, .bytes = std::string(static_cast<size_t>(info.st_size), '\0')};
    size_t offset = 0;
    while (offset < contents.bytes.size()) {
      const ssize_t count = read(fd, contents.bytes.data() + offset, contents.bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        close(fd);
        return std::nullopt;
      }
      offset += static_cast<size_t>(count);
    }
    close(fd);
    return contents;
  }

  bool
  writePrivateStateFile(const std::filesystem::path& path, std::string_view bytes, std::string_view temporaryPrefix) {
    if (path.empty() || !path.is_absolute() || !validName(temporaryPrefix)) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return false;
    }
    const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (directory < 0) {
      return false;
    }
    if (fstat(directory, &info) != 0 || info.st_uid != geteuid() || fchmod(directory, 0700) != 0) {
      close(directory);
      return false;
    }
    const auto suffix = randomHex(16);
    if (!suffix) {
      close(directory);
      return false;
    }
    std::string temporary(temporaryPrefix);
    temporary += *suffix;
    const int fd = openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      close(directory);
      return false;
    }
    const bool written = writeAll(fd, bytes) && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    const bool replaced =
        written && closed && renameat(directory, temporary.c_str(), directory, path.filename().c_str()) == 0;
    if (!replaced) {
      unlinkat(directory, temporary.c_str(), 0);
    } else {
      // The rename committed the new credentials. Reporting failure after it
      // could keep an old in-memory token valid.
      fsync(directory);
    }
    close(directory);
    return replaced;
  }

} // namespace umbriel
