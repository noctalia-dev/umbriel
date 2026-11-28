#pragma once

#include <wayland-server-core.h>

struct wlr_compositor;

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

  private:
    static void onNewSurface(wl_listener* listener, void* data);

    wl_listener m_newSurface{};
  };

} // namespace umbriel
