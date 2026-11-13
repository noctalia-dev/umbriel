#include "input/x11_input_permissions.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {

  namespace {
    constexpr size_t kMaxApplications = 128;
    constexpr size_t kMaxApplicationBytes = 256;
    constexpr size_t kMaxStoreBytes = 64 * 1024;

    bool validApplication(std::string_view application) {
      return !application.empty() && application.size() <= kMaxApplicationBytes;
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

  X11InputPermissions::X11InputPermissions(std::filesystem::path path) : m_path(std::move(path)) {}

  std::filesystem::path X11InputPermissions::defaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
      return std::filesystem::path(state) / "umbriel/x11-input-permissions.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".local/state/umbriel/x11-input-permissions.json";
    }
    return {};
  }

  bool X11InputPermissions::load(std::vector<std::string>& applications) const {
    applications.clear();
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    const int fd = open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      return errno == ENOENT;
    }
    struct stat info{};
    const bool safe = fstat(fd, &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == geteuid()
        && (info.st_mode & 022) == 0
        && info.st_size >= 0
        && static_cast<uint64_t>(info.st_size) <= kMaxStoreBytes;
    if (!safe) {
      close(fd);
      return false;
    }
    std::string bytes(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        close(fd);
        return false;
      }
      offset += static_cast<size_t>(count);
    }
    close(fd);
    const auto data = nlohmann::json::parse(bytes, nullptr, false);
    if (!data.is_object()
        || data.size() != 2
        || data.value("version", nlohmann::json()) != 1
        || !data.contains("applications")
        || !data["applications"].is_array()
        || data["applications"].size() > kMaxApplications) {
      return false;
    }
    for (const auto& item : data["applications"]) {
      if (!item.is_string() || !validApplication(item.get_ref<const std::string&>())) {
        applications.clear();
        return false;
      }
      if (!std::ranges::contains(applications, item.get_ref<const std::string&>())) {
        applications.push_back(item.get<std::string>());
      }
    }
    return true;
  }

  bool X11InputPermissions::save(const std::vector<std::string>& applications) const {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    std::string bytes;
    try {
      bytes = nlohmann::json{{"version", 1}, {"applications", applications}}.dump(2) + '\n';
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (error) {
      return false;
    }
    const int directory = open(m_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
      return false;
    }
    const std::string temporary = "." + m_path.filename().string() + "." + std::to_string(getpid());
    const int fd = openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      close(directory);
      return false;
    }
    const bool written = writeAll(fd, bytes) && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    const bool replaced =
        written && closed && renameat(directory, temporary.c_str(), directory, m_path.filename().c_str()) == 0;
    if (!replaced) {
      unlinkat(directory, temporary.c_str(), 0);
    } else {
      fsync(directory);
    }
    close(directory);
    return replaced;
  }

  bool X11InputPermissions::contains(std::string_view application) const {
    std::vector<std::string> applications;
    return load(applications) && std::ranges::contains(applications, application);
  }

  std::vector<std::string> X11InputPermissions::list() const {
    std::vector<std::string> applications;
    load(applications);
    return applications;
  }

  bool X11InputPermissions::remember(std::string_view application) {
    std::vector<std::string> applications;
    if (!validApplication(application) || !load(applications)) {
      return false;
    }
    if (std::ranges::contains(applications, application)) {
      return true;
    }
    if (applications.size() >= kMaxApplications) {
      return false;
    }
    applications.emplace_back(application);
    return save(applications);
  }

  bool X11InputPermissions::revoke(std::string_view application) {
    std::vector<std::string> applications;
    if (application == "all") {
      return save(applications);
    }
    if (!validApplication(application) || !load(applications)) {
      return false;
    }
    if (std::erase(applications, application) == 0) {
      return true;
    }
    return save(applications);
  }

} // namespace umbriel
