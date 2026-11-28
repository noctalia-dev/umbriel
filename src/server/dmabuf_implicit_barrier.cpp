#include "server/dmabuf_implicit_barrier.h"

#include "wlr.h"

#include <algorithm>
#include <cstdint>
#include <poll.h>
#include <vector>

namespace umbriel {

  namespace {

    struct SurfaceWatch;
    struct FenceLock;

    // A distinct plane fd whose write fence is pending. wl_event_loop_add_fd keeps its own duplicate of the fd.
    struct PlaneWait {
      FenceLock* lock = nullptr;
      wl_event_source* source = nullptr;
    };

    struct FenceLock {
      SurfaceWatch* watch = nullptr;
      uint32_t seq = 0;
      std::vector<PlaneWait*> planes;
    };

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      wl_event_loop* loop = nullptr;
      std::vector<FenceLock*> locks;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    // POLLIN on a dmabuf waits for its write fences only. An fd that cannot be polled counts as ready.
    bool fenceSignalled(int fd) {
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      return poll(&pfd, 1, 0) != 0;
    }

    int onPlaneReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
      auto* plane = static_cast<PlaneWait*>(data);
      FenceLock* lock = plane->lock;
      // A signalled dmabuf stays readable, so the level-triggered source must go before the loop reports it again.
      wl_event_source_remove(plane->source);
      std::erase(lock->planes, plane);
      delete plane;
      if (!lock->planes.empty()) {
        return 0;
      }
      SurfaceWatch* watch = lock->watch;
      std::erase(watch->locks, lock);
      const uint32_t seq = lock->seq;
      delete lock;
      // May apply the cached state, and with it every later one no other lock holds back.
      wlr_surface_unlock_cached(watch->surface, seq);
      return 0;
    }

    void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      wlr_surface* surface = watch->surface;
      if ((surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0 || surface->pending.buffer == nullptr) {
        return;
      }
      // SyncobjAcquireBarrier covers commits with an acquire point.
      uint64_t point = 0;
      if (umbrielfx_linux_drm_syncobj_state_acquire_point(surface, &surface->pending, &point) != nullptr) {
        return;
      }
      wlr_dmabuf_attributes dmabuf{};
      if (!wlr_buffer_get_dmabuf(surface->pending.buffer, &dmabuf)) {
        return;
      }

      FenceLock* lock = nullptr;
      for (int i = 0; i < dmabuf.n_planes; ++i) {
        const int fd = dmabuf.fd[i];
        if (fd < 0 || std::find(dmabuf.fd, dmabuf.fd + i, fd) != dmabuf.fd + i || fenceSignalled(fd)) {
          continue;
        }
        if (lock == nullptr) {
          lock = new FenceLock();
          lock->watch = watch;
        }
        auto* plane = new PlaneWait();
        plane->lock = lock;
        plane->source = wl_event_loop_add_fd(watch->loop, fd, WL_EVENT_READABLE, onPlaneReadable, plane);
        if (plane->source == nullptr) {
          delete plane;
          continue;
        }
        lock->planes.push_back(plane);
      }
      if (lock == nullptr) {
        return;
      }
      if (lock->planes.empty()) {
        delete lock;
        return;
      }
      lock->seq = wlr_surface_lock_pending(surface);
      watch->locks.push_back(lock);
    }

    void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      for (FenceLock* lock : watch->locks) {
        for (PlaneWait* plane : lock->planes) {
          wl_event_source_remove(plane->source);
          delete plane;
        }
        wlr_surface_unlock_cached(watch->surface, lock->seq);
        delete lock;
      }
      wl_list_remove(&watch->clientCommit.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

  } // namespace

  DmabufImplicitBarrier::DmabufImplicitBarrier(wlr_compositor* compositor) {
    m_newSurface.notify = onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_newSurface);
  }

  DmabufImplicitBarrier::~DmabufImplicitBarrier() { wl_list_remove(&m_newSurface.link); }

  void DmabufImplicitBarrier::onNewSurface(wl_listener* /*listener*/, void* data) {
    auto* watch = new SurfaceWatch();
    watch->surface = static_cast<wlr_surface*>(data);
    watch->loop = wl_display_get_event_loop(wl_client_get_display(wl_resource_get_client(watch->surface->resource)));
    watch->clientCommit.notify = onClientCommit;
    wl_signal_add(&watch->surface->events.client_commit, &watch->clientCommit);
    watch->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&watch->surface->events.destroy, &watch->destroy);
  }

} // namespace umbriel
