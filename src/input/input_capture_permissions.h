#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  struct InputCapturePermission {
    std::string id;
    std::string application;
    uint32_t capabilities = 0;
    uint32_t persistMode = 0;
  };

  struct InputCaptureGrant {
    std::string id;
    std::string token;
    uint32_t persistMode = 0;
  };

  // The token is a bearer credential. Display names and application identifiers
  // alone never authorize capture. Only durable grants are written to disk.
  class InputCapturePermissions {
  public:
    explicit InputCapturePermissions(std::filesystem::path path);
    static std::filesystem::path defaultPath();

    [[nodiscard]] std::optional<InputCaptureGrant>
    issue(std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer);
    [[nodiscard]] std::optional<InputCaptureGrant> restore(
        std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer
    );
    [[nodiscard]] std::vector<InputCapturePermission> list();
    [[nodiscard]] bool revoke(std::string_view id);
    void expirePeer(uint64_t peer);

  private:
    struct Record {
      InputCapturePermission permission;
      std::string token;
      uint64_t peer = 0;
    };

    bool load();
    bool save(const std::vector<Record>& records) const;

    std::filesystem::path m_path;
    std::vector<Record> m_records;
    bool m_loaded = false;
  };

} // namespace umbriel
