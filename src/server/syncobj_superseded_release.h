#pragma once

#include <wayland-server-core.h>

struct wlr_compositor;

namespace umbriel {

  // wlroots queues each commit to a synchronized subsurface as its own cached state and creates its linux-drm-syncobj
  // release only once that state is applied, so buffers committed while the parent has not committed again keep their
  // release points pending. A client that renders a new surface tree with a few swapchain buffers before its main
  // thread commits the parent (Firefox opening a prompt panel) blocks on those points and never commits the parent.
  // A cached buffer that a later cached commit replaces is never shown once nothing between them holds a lock, because
  // wlroots then applies both in one pass. This signals such a buffer's release point while it waits.
  class SyncobjSupersededRelease {
  public:
    explicit SyncobjSupersededRelease(wlr_compositor* compositor);
    ~SyncobjSupersededRelease();

    SyncobjSupersededRelease(const SyncobjSupersededRelease&) = delete;
    SyncobjSupersededRelease& operator=(const SyncobjSupersededRelease&) = delete;

  private:
    static void onNewSurface(wl_listener* listener, void* data);

    wl_listener m_newSurface{};
  };

} // namespace umbriel
