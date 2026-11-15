#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  // X11 application names remembered as allowed to emulate desktop input. Every
  // call rereads the file, so another instance's changes take effect.
  class X11InputPermissions {
  public:
    explicit X11InputPermissions(std::filesystem::path path);
    static std::filesystem::path defaultPath();

    bool contains(std::string_view application) const;
    std::vector<std::string> list() const;
    [[nodiscard]] bool remember(std::string_view application);
    // `all` forgets every application. Forgetting an unknown name succeeds.
    [[nodiscard]] bool revoke(std::string_view application);

  private:
    bool load(std::vector<std::string>& applications) const;
    bool save(const std::vector<std::string>& applications) const;

    std::filesystem::path m_path;
  };

} // namespace umbriel
