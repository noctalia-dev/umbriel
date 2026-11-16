#pragma once

#include "input/global_shortcuts.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  struct GlobalShortcutCredential {
    GlobalShortcutGrant grant;
    std::string token;
  };

  class GlobalShortcutsStore {
  public:
    static std::filesystem::path defaultPath();
    explicit GlobalShortcutsStore(std::filesystem::path path);

    // Invoke only after physical approval; grantId extends an already authorized grant.
    [[nodiscard]] std::optional<GlobalShortcutCredential>
    approve(std::string_view application, std::span<const GlobalShortcut> shortcuts, std::string_view grantId = {});
    [[nodiscard]] std::optional<GlobalShortcutCredential> restore(std::string_view token, std::string_view application);
    // History can record user overrides, but cannot add IDs to the approved scope.
    [[nodiscard]] bool
    recordBindings(std::string_view grantId, std::string_view application, std::span<const GlobalShortcut> shortcuts);
    std::optional<std::vector<GlobalShortcutGrant>> list() const;
    [[nodiscard]] bool revokeApplication(std::string_view application);
    [[nodiscard]] bool revokeAll();

  private:
    struct Record {
      GlobalShortcutGrant grant;
      std::string token;
    };

    std::optional<std::vector<Record>> load() const;
    bool save(const std::vector<Record>& records) const;
    bool revoke(std::optional<std::string_view> application);

    std::filesystem::path m_path;
  };

} // namespace umbriel
