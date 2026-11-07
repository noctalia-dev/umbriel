#pragma once

#include <memory>
#include <pixman.h>

struct wl_display;
struct wl_global;
struct wlr_surface;

namespace umbriel {

  // Advertises ext_background_effect_manager_v1 and tracks the blur region each
  // surface asks for. set_blur_region is double-buffered: the request lands in
  // pending state and wl_surface.commit publishes it to current state.
  //
  // The manager owns no scene nodes. Owners of a surface's backdrop blur read
  // region() and apply it as a clip on the blur node they already manage.
  class BackgroundEffectManager {
  public:
    explicit BackgroundEffectManager(wl_display* display);
    ~BackgroundEffectManager();

    BackgroundEffectManager(const BackgroundEffectManager&) = delete;
    BackgroundEffectManager& operator=(const BackgroundEffectManager&) = delete;

    wl_global* global() const;

    // Committed blur region in surface-local coordinates, nullptr when the
    // surface made no request. An empty region is still a request: the client
    // asked for no area to be blurred. The returned region belongs to the
    // surface's current state and stays valid until that state is replaced or
    // the surface is destroyed.
    const pixman_region32_t* region(wlr_surface* surface);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
