#pragma once

#include <memory>

struct wl_global;
struct wlr_image_description_v1_data;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_surface;

namespace umbriel {

  class Server;

  class ColorManager {
  public:
    explicit ColorManager(Server& server);
    ~ColorManager();

    ColorManager(const ColorManager&) = delete;
    ColorManager& operator=(const ColorManager&) = delete;

    bool valid() const;
    wl_global* global() const;
    const wlr_image_description_v1_data* surfaceDescription(wlr_surface* surface) const;
    bool surfaceRequiresHdrOutput(wlr_surface* surface) const;
    // Register before adding surfaces. Scene destruction removes the registration.
    [[nodiscard]] bool registerScene(wlr_scene* scene);
    // Describes `buffer` with the surface's colour state. A null surface resets it to the sRGB default, for a node
    // that shows a compositor-drawn colour rather than surface content.
    void applySurfaceDescriptionToBuffer(wlr_surface* surface, wlr_scene_buffer* buffer) const;
    void updatePreferredDescriptions();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
