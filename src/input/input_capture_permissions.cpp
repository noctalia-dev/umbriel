#include "input/input_capture_permissions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {

  namespace {
    constexpr size_t kMaxGrants = 128;
    constexpr size_t kMaxStoreBytes = 256 * 1024;

    class StoreLock {
    public:
      explicit StoreLock(const std::filesystem::path& path) {
        if (path.empty() || !path.is_absolute()) {
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
          m_fd = openat(directory, ".input-capture-permissions.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
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
      ~StoreLock() {
        if (m_fd >= 0) {
          close(m_fd);
        }
      }
      StoreLock(const StoreLock&) = delete;
      StoreLock& operator=(const StoreLock&) = delete;
      explicit operator bool() const { return m_fd >= 0; }

    private:
      int m_fd = -1;
    };

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

    bool hexString(std::string_view value, size_t length) {
      return value.size() == length
          && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    bool validRequest(std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer) {
      if (application.size() > 256 || capabilities == 0 || (capabilities & ~3U) != 0 || persistMode > 2 || peer == 0) {
        return false;
      }
      try {
        nlohmann::json(application).dump();
      } catch (const nlohmann::json::exception&) {
        return false;
      }
      return true;
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

  InputCapturePermissions::InputCapturePermissions(std::filesystem::path path) : m_path(std::move(path)) {
    m_loaded = load();
  }

  std::filesystem::path InputCapturePermissions::defaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
      return std::filesystem::path(state) / "umbriel/input-capture-permissions.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".local/state/umbriel/input-capture-permissions.json";
    }
    return {};
  }

  bool InputCapturePermissions::load() {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    const int fd = open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      if (errno != ENOENT) {
        return false;
      }
      std::erase_if(m_records, [](const auto& item) { return item.permission.persistMode == 2; });
      return true;
    }
    struct stat info{};
    const bool safe = fstat(fd, &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == geteuid()
        && (info.st_mode & 077) == 0
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
        || !data.contains("version")
        || !data["version"].is_number_unsigned()
        || data["version"] != 1
        || !data.contains("grants")
        || !data["grants"].is_array()
        || data["grants"].size() > kMaxGrants) {
      return false;
    }
    std::vector<Record> records;
    for (const auto& item : data["grants"]) {
      if (!item.is_object()
          || item.size() != 4
          || !item.contains("id")
          || !item["id"].is_string()
          || !item.contains("token")
          || !item["token"].is_string()
          || !item.contains("application")
          || !item["application"].is_string()
          || !item.contains("capabilities")
          || !item["capabilities"].is_number_unsigned()) {
        return false;
      }
      const auto id = item["id"].get<std::string>();
      const auto token = item["token"].get<std::string>();
      const auto application = item["application"].get<std::string>();
      const auto capabilities = item["capabilities"].get<uint64_t>();
      if (!hexString(id, 32)
          || !hexString(token, 64)
          || application.size() > 256
          || capabilities == 0
          || capabilities > 3
          || std::ranges::any_of(records, [&](const auto& record) {
               return record.permission.id == id || record.token == token;
             })) {
        return false;
      }
      records.push_back({{id, application, static_cast<uint32_t>(capabilities), 2}, token, 0});
    }
    for (const auto& record : m_records) {
      if (record.permission.persistMode == 1) {
        records.push_back(record);
      }
    }
    m_records = std::move(records);
    return true;
  }

  bool InputCapturePermissions::save(const std::vector<Record>& records) const {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    auto data = nlohmann::json{{"version", 1}, {"grants", nlohmann::json::array()}};
    for (const auto& record : records) {
      if (record.permission.persistMode == 2) {
        data["grants"].push_back({
            {"id", record.permission.id},
            {"application", record.permission.application},
            {"capabilities", record.permission.capabilities},
            {"token", record.token},
        });
      }
    }
    std::string bytes;
    try {
      bytes = data.dump() + '\n';
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    if (bytes.size() > kMaxStoreBytes) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (error) {
      return false;
    }
    const int directory = open(m_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
    const std::string temporary = ".input-capture-" + *suffix;
    const int fd = openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
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
      // The rename already committed the new credentials. Do not report a
      // recoverable failure that could keep an old in-memory token valid.
      fsync(directory);
    }
    close(directory);
    return replaced;
  }

  std::optional<InputCaptureGrant> InputCapturePermissions::issue(
      std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer
  ) {
    if (!validRequest(application, capabilities, persistMode, peer) || persistMode == 0) {
      return std::nullopt;
    }
    // Serialize durable credentials across compositor instances. Refresh while
    // holding the lock so another instance cannot restore a consumed token.
    std::optional<StoreLock> lock;
    if (persistMode == 2) {
      lock.emplace(m_path);
      if (!*lock || !(m_loaded = load())) {
        return std::nullopt;
      }
    }
    if (m_records.size() >= kMaxGrants) {
      return std::nullopt;
    }
    const auto id = randomHex(16);
    const auto token = randomHex(32);
    if (!id || !token) {
      return std::nullopt;
    }
    auto records = m_records;
    records.push_back(
        {{*id, std::string(application), capabilities, persistMode}, *token, persistMode == 1 ? peer : 0}
    );
    if (persistMode == 2 && !save(records)) {
      return std::nullopt;
    }
    m_records = std::move(records);
    return InputCaptureGrant{*id, *token, persistMode};
  }

  std::optional<InputCaptureGrant> InputCapturePermissions::restore(
      std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer
  ) {
    if (!validRequest(application, capabilities, persistMode, peer) || !hexString(token, 64)) {
      return std::nullopt;
    }
    std::optional<StoreLock> lock;
    const bool ephemeral = std::ranges::any_of(m_records, [token](const auto& record) {
      return record.permission.persistMode == 1 && record.token == token;
    });
    if (!ephemeral) {
      lock.emplace(m_path);
      if (!*lock || !(m_loaded = load())) {
        return std::nullopt;
      }
    }
    const auto found = std::ranges::find(m_records, token, &Record::token);
    if (found == m_records.end()
        || found->permission.application != application
        || (capabilities & ~found->permission.capabilities) != 0
        || persistMode > found->permission.persistMode
        || (found->permission.persistMode == 1 && found->peer != peer)) {
      return std::nullopt;
    }
    const std::string id = found->permission.id;
    const auto nextToken = persistMode != 0 ? randomHex(32) : std::optional<std::string>{std::string{}};
    if (!nextToken) {
      return std::nullopt;
    }
    auto records = m_records;
    const size_t index = static_cast<size_t>(found - m_records.begin());
    const bool durable = found->permission.persistMode == 2;
    if (persistMode == 0) {
      records.erase(records.begin() + static_cast<ptrdiff_t>(index));
    } else {
      records[index].token = *nextToken;
      records[index].permission.persistMode = persistMode;
      records[index].peer = persistMode == 1 ? peer : 0;
    }
    if (durable && !save(records)) {
      return std::nullopt;
    }
    m_records = std::move(records);
    return InputCaptureGrant{id, *nextToken, persistMode};
  }

  std::vector<InputCapturePermission> InputCapturePermissions::list() {
    StoreLock lock(m_path);
    if (lock) {
      m_loaded = load();
      if (!m_loaded) {
        std::erase_if(m_records, [](const auto& item) { return item.permission.persistMode == 2; });
      }
    }
    std::vector<InputCapturePermission> result;
    result.reserve(m_records.size());
    for (const auto& record : m_records) {
      result.push_back(record.permission);
    }
    return result;
  }

  bool InputCapturePermissions::revoke(std::string_view id) {
    const bool all = id == "all";
    if (!all && !hexString(id, 32)) {
      return false;
    }
    std::optional<StoreLock> lock;
    const bool ephemeral = !all && std::ranges::any_of(m_records, [id](const auto& record) {
      return record.permission.persistMode == 1 && record.permission.id == id;
    });
    if (!ephemeral) {
      lock.emplace(m_path);
      if (!*lock) {
        return false;
      }
      m_loaded = load();
      if (!m_loaded && !all) {
        return false;
      }
    }
    if (!all && std::ranges::none_of(m_records, [id](const auto& item) { return item.permission.id == id; })) {
      // A restored-once session still carries this ID after consuming the
      // stored permission. Revoking it again must remain successful.
      return true;
    }
    auto records = m_records;
    std::erase_if(records, [id, all](const auto& item) { return all || item.permission.id == id; });
    const bool durable = all || std::ranges::any_of(m_records, [id](const auto& item) {
                           return item.permission.id == id && item.permission.persistMode == 2;
                         });
    if (durable && !save(records)) {
      return false;
    }
    m_records = std::move(records);
    if (all) {
      m_loaded = true;
    }
    return true;
  }

  void InputCapturePermissions::expirePeer(uint64_t peer) {
    std::erase_if(m_records, [peer](const auto& item) {
      return item.permission.persistMode == 1 && item.peer == peer;
    });
  }

} // namespace umbriel
