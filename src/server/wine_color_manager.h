#pragma once

#include <memory>

struct wl_client;
struct wl_global;
struct wlr_image_description_v1_data;
struct wlr_scene_buffer;
struct wlr_surface;

namespace umbriel {

  class Server;

  class WineColorManager {
  public:
    explicit WineColorManager(Server& server);
    ~WineColorManager();

    WineColorManager(const WineColorManager&) = delete;
    WineColorManager& operator=(const WineColorManager&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] wl_global* global() const;
    [[nodiscard]] const wlr_image_description_v1_data* surfaceDescription(wlr_surface* surface) const;
    [[nodiscard]] bool surfaceRequiresHdrOutput(wlr_surface* surface) const;
    void applySurfaceDescriptionToBuffer(wlr_surface* surface, wlr_scene_buffer* buffer) const;
    void applySurfaceDescriptions();
    void updatePreferredDescriptions();

    [[nodiscard]] static bool clientNeedsCompatibility(const wl_client* client);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
