#include "server/syncobj_superseded_release.h"

#include "wlr.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace umbriel {

  namespace {

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      wl_event_loop* loop = nullptr;
      wl_event_source* idle = nullptr;
      // Cached states whose release point this watch has signalled, by wlr_surface_state::seq. When wlroots applies
      // such a state later it signals the point again, which the timeline ignores.
      std::vector<uint32_t> signalled;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    bool cachedStateExists(wlr_surface* surface, uint32_t seq) {
      wlr_surface_state* state;
      wl_list_for_each(state, &surface->cached, cached_state_link) {
        if (state->seq == seq) {
          return true;
        }
      }
      return false;
    }

    void signalRelease(SurfaceWatch& watch, wlr_surface_state* state) {
      if (std::ranges::find(watch.signalled, state->seq) != watch.signalled.end()) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          umbrielfx_linux_drm_syncobj_cached_release_point(watch.surface, state, &point);
      if (timeline == nullptr) {
        return;
      }
      wlr_drm_syncobj_timeline_signal(timeline, point);
      watch.signalled.push_back(state->seq);
    }

    void onIdle(void* data) {
      auto* watch = static_cast<SurfaceWatch*>(data);
      watch->idle = nullptr;
      wlr_surface* surface = watch->surface;
      std::erase_if(watch->signalled, [surface](uint32_t seq) { return !cachedStateExists(surface, seq); });

      // wlroots applies the cached states in order and stops at the first one that still holds a lock, so a buffer is
      // only skipped when every state after it, up to the commit that replaces it, is free to apply with it.
      wlr_surface_state* superseded = nullptr;
      wlr_surface_state* state;
      wl_list_for_each(state, &surface->cached, cached_state_link) {
        if (state->cached_state_locks > 0) {
          superseded = nullptr;
        }
        if ((state->committed & WLR_SURFACE_STATE_BUFFER) == 0) {
          continue;
        }
        if (superseded != nullptr && superseded->buffer != state->buffer) {
          signalRelease(*watch, superseded);
        }
        superseded = state->buffer != nullptr && state->buffer != surface->current.buffer ? state : nullptr;
      }
    }

    void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      // Inspect the queue once wlroots and the other commit listeners have cached this commit and taken their locks.
      if (watch->idle == nullptr) {
        watch->idle = wl_event_loop_add_idle(watch->loop, onIdle, watch);
      }
    }

    void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (watch->idle != nullptr) {
        wl_event_source_remove(watch->idle);
      }
      wl_list_remove(&watch->clientCommit.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

  } // namespace

  SyncobjSupersededRelease::SyncobjSupersededRelease(wlr_compositor* compositor) {
    m_newSurface.notify = onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_newSurface);
  }

  SyncobjSupersededRelease::~SyncobjSupersededRelease() { wl_list_remove(&m_newSurface.link); }

  void SyncobjSupersededRelease::onNewSurface(wl_listener* /*listener*/, void* data) {
    auto* watch = new SurfaceWatch();
    watch->surface = static_cast<wlr_surface*>(data);
    watch->loop = wl_display_get_event_loop(wl_client_get_display(wl_resource_get_client(watch->surface->resource)));
    watch->clientCommit.notify = onClientCommit;
    wl_signal_add(&watch->surface->events.client_commit, &watch->clientCommit);
    watch->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&watch->surface->events.destroy, &watch->destroy);
  }

} // namespace umbriel
