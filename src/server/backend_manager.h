#pragma once

#include "config/config.h"

#include <memory>
#include <string_view>

struct wl_display;
struct wlr_backend;
struct wlr_renderer;
struct wlr_session;

namespace umbriel {

  // Owns backend selection and the immutable DRM policy captured at startup.
  // Server only needs the resulting wlroots objects and one renderer factory;
  // filtering, hotplug, environment precedence, and cleanup stay here.
  class BackendManager {
  public:
    [[nodiscard]] static std::unique_ptr<BackendManager> create(wl_display* display, const Config::Drm& config);
    ~BackendManager();

    BackendManager(const BackendManager&) = delete;
    BackendManager& operator=(const BackendManager&) = delete;

    [[nodiscard]] wlr_backend* backend() const;
    [[nodiscard]] wlr_session* session() const;
    [[nodiscard]] wlr_renderer* createRenderer();
    [[nodiscard]] bool verifyOpenDevices(std::string_view context);
    void markStarted();

  private:
    class Impl;
    explicit BackendManager(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
