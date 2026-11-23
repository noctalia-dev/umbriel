#pragma once

#include <memory>
#include <wayland-server-core.h>

struct wlr_compositor;
struct wlr_surface;

namespace umbriel {

  // Holds each dmabuf commit that has no linux-drm-syncobj acquire point until every plane's implicit write fence has
  // signalled, which the dmabuf fd reports by becoming readable. NVIDIA does not reliably wait on these fences when the
  // renderer samples the buffer, so an unfinished Chromium frame would otherwise reach the screen.
  class DmabufImplicitBarrier {
  public:
    explicit DmabufImplicitBarrier(wlr_compositor* compositor);
    ~DmabufImplicitBarrier();

    DmabufImplicitBarrier(const DmabufImplicitBarrier&) = delete;
    DmabufImplicitBarrier& operator=(const DmabufImplicitBarrier&) = delete;

    struct Stats;

  private:
    static void onNewSurface(wl_listener* listener, void* data);
    void handleNewSurface(wlr_surface* surface);

    std::unique_ptr<Stats> m_stats;
    wl_listener m_newSurface{};
  };

} // namespace umbriel
