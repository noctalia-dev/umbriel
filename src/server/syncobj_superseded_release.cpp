#include "server/syncobj_superseded_release.h"

#include "wlr.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xf86drm.h>

namespace umbriel {

  namespace {

    // A cached state is waited on at most this many times: once while its fence materializes, and once more in case
    // the wait fired before wlroots dropped the matching lock.
    constexpr int kMaxAcquireWaits = 2;

    struct SurfaceWatch;

    struct AcquireWait {
      wlr_drm_syncobj_timeline_waiter waiter{};
      SurfaceWatch* watch = nullptr;
      uint32_t seq = 0;
    };

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      wl_event_loop* loop = nullptr;
      wl_event_source* idle = nullptr;
      // Cached states whose release point this watch has signalled, by wlr_surface_state::seq. When wlroots applies
      // such a state later it signals the point again, which the timeline ignores.
      std::vector<uint32_t> signalled;
      // Release points this surface committed that may still be pending. Signalling a point also completes every lower
      // point on its timeline, so a buffer is only released early while none of these sit below its point.
      std::unordered_map<wlr_drm_syncobj_timeline*, std::vector<uint64_t>> committedPoints;
      // wlroots locks a cached state until its acquire fence materializes and reports nothing when it unlocks, so a
      // state that blocks a release is waited on here, by seq, to inspect the queue again.
      std::vector<AcquireWait*> waits;
      std::vector<std::pair<uint32_t, int>> waitCounts;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    void scheduleInspection(SurfaceWatch& watch);

    bool cachedStateExists(wlr_surface* surface, uint32_t seq) {
      wlr_surface_state* state;
      wl_list_for_each(state, &surface->cached, cached_state_link) {
        if (state->seq == seq) {
          return true;
        }
      }
      return false;
    }

    void pruneCommittedPoints(SurfaceWatch& watch) {
      for (auto it = watch.committedPoints.begin(); it != watch.committedPoints.end();) {
        wlr_drm_syncobj_timeline* timeline = it->first;
        std::erase_if(it->second, [timeline](uint64_t point) {
          bool signalled = false;
          return wlr_drm_syncobj_timeline_check(timeline, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &signalled)
              && signalled;
        });
        if (it->second.empty()) {
          wlr_drm_syncobj_timeline_unref(timeline);
          it = watch.committedPoints.erase(it);
        } else {
          ++it;
        }
      }
    }

    void recordCommittedPoint(SurfaceWatch& watch) {
      wlr_surface* surface = watch.surface;
      if ((surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          umbrielfx_linux_drm_syncobj_state_release_point(surface, &surface->pending, &point);
      if (timeline == nullptr) {
        return;
      }
      auto it = watch.committedPoints.find(timeline);
      if (it == watch.committedPoints.end()) {
        it = watch.committedPoints.emplace(wlr_drm_syncobj_timeline_ref(timeline), std::vector<uint64_t>{}).first;
      }
      it->second.push_back(point);
    }

    void signalRelease(SurfaceWatch& watch, wlr_surface_state* state) {
      if (std::ranges::find(watch.signalled, state->seq) != watch.signalled.end()) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          umbrielfx_linux_drm_syncobj_state_release_point(watch.surface, state, &point);
      if (timeline == nullptr) {
        return;
      }
      auto it = watch.committedPoints.find(timeline);
      if (it != watch.committedPoints.end()
          && std::ranges::any_of(it->second, [point](uint64_t pending) { return pending < point; })) {
        // An earlier commit on this timeline, possibly the one on screen, has not been released yet.
        return;
      }
      if (!wlr_drm_syncobj_timeline_signal(timeline, point)) {
        return;
      }
      watch.signalled.push_back(state->seq);
      // The point is complete now, so it must not hold back a later point on its timeline in this same inspection.
      if (it != watch.committedPoints.end()) {
        std::erase_if(it->second, [point](uint64_t pending) { return pending <= point; });
      }
    }

    void onAcquireReady(wlr_drm_syncobj_timeline_waiter* waiter) {
      AcquireWait* wait;
      wait = wl_container_of(waiter, wait, waiter);
      SurfaceWatch* watch = wait->watch;
      wlr_drm_syncobj_timeline_waiter_finish(&wait->waiter);
      std::erase(watch->waits, wait);
      delete wait;
      // wlroots' own wait on this fence unlocks the state when it runs; inspect the queue once it has.
      scheduleInspection(*watch);
    }

    void waitForAcquire(SurfaceWatch& watch, wlr_surface_state* state) {
      if (std::ranges::any_of(watch.waits, [state](const AcquireWait* wait) { return wait->seq == state->seq; })) {
        return;
      }
      auto count = std::ranges::find(watch.waitCounts, state->seq, &std::pair<uint32_t, int>::first);
      if (count == watch.waitCounts.end()) {
        count = watch.waitCounts.insert(watch.waitCounts.end(), {state->seq, 0});
      }
      if (count->second >= kMaxAcquireWaits) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          umbrielfx_linux_drm_syncobj_state_acquire_point(watch.surface, state, &point);
      if (timeline == nullptr) {
        return;
      }
      auto* wait = new AcquireWait();
      wait->watch = &watch;
      wait->seq = state->seq;
      if (!wlr_drm_syncobj_timeline_waiter_init(
              &wait->waiter, timeline, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, watch.loop, onAcquireReady
          )) {
        delete wait;
        return;
      }
      ++count->second;
      watch.waits.push_back(wait);
    }

    void onIdle(void* data) {
      auto* watch = static_cast<SurfaceWatch*>(data);
      watch->idle = nullptr;
      wlr_surface* surface = watch->surface;
      std::erase_if(watch->signalled, [surface](uint32_t seq) { return !cachedStateExists(surface, seq); });
      std::erase_if(watch->waitCounts, [surface](const std::pair<uint32_t, int>& count) {
        return !cachedStateExists(surface, count.first);
      });
      pruneCommittedPoints(*watch);

      // The buffer on screen. wlroots clears current.buffer once a commit is applied and keeps the client's buffer
      // only as the source of the uploaded one.
      wlr_buffer* displayed = surface->buffer != nullptr ? surface->buffer->source : nullptr;

      // wlroots applies the cached states in order and stops at the first one that still holds a lock, so a buffer is
      // only skipped when every state after it, up to the commit that replaces it, is free to apply with it.
      wlr_surface_state* superseded = nullptr;
      wlr_surface_state* state;
      wl_list_for_each(state, &surface->cached, cached_state_link) {
        if (state->cached_state_locks > 0) {
          if (superseded != nullptr) {
            waitForAcquire(*watch, state);
          }
          superseded = nullptr;
        }
        if ((state->committed & WLR_SURFACE_STATE_BUFFER) == 0) {
          continue;
        }
        // A buffer reattached unmodified still replaces the earlier commit: the later commit holds its own release
        // point for the same buffer.
        if (superseded != nullptr) {
          signalRelease(*watch, superseded);
        }
        superseded = state->buffer != nullptr && state->buffer != displayed ? state : nullptr;
      }
    }

    void scheduleInspection(SurfaceWatch& watch) {
      if (watch.idle == nullptr) {
        watch.idle = wl_event_loop_add_idle(watch.loop, onIdle, &watch);
      }
    }

    void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      recordCommittedPoint(*watch);
      // Inspect the queue once wlroots and the other commit listeners have cached this commit and taken their locks.
      scheduleInspection(*watch);
    }

    void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (watch->idle != nullptr) {
        wl_event_source_remove(watch->idle);
      }
      for (AcquireWait* wait : watch->waits) {
        wlr_drm_syncobj_timeline_waiter_finish(&wait->waiter);
        delete wait;
      }
      for (const auto& [timeline, points] : watch->committedPoints) {
        wlr_drm_syncobj_timeline_unref(timeline);
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
