#include "server/background_effect.h"

#include "ext-background-effect-v1-server-protocol.h"
#include "wlr.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace umbriel {

  namespace {

    // One surface's double-buffered blur request. `requested` separates "no
    // request" from a request whose region is empty; the protocol's NULL region
    // clears the request entirely.
    struct RegionState {
      pixman_region32_t region;
      bool requested = false;
    };

    void initState(void* raw) {
      auto* state = static_cast<RegionState*>(raw);
      pixman_region32_init(&state->region);
      state->requested = false;
    }

    void finishState(void* raw) {
      auto* state = static_cast<RegionState*>(raw);
      pixman_region32_fini(&state->region);
    }

    // States move between the surface's cached and current slots. The default
    // memcpy would alias the two pixman allocations and double-free them, so
    // copy deeply and leave both sides owning their storage.
    void moveState(void* dstRaw, void* srcRaw) {
      auto* dst = static_cast<RegionState*>(dstRaw);
      const auto* src = static_cast<const RegionState*>(srcRaw);
      pixman_region32_fini(&dst->region);
      pixman_region32_init(&dst->region);
      pixman_region32_copy(&dst->region, &src->region);
      dst->requested = src->requested;
    }

    const wlr_surface_synced_impl kSyncedImpl{
        .state_size = sizeof(RegionState),
        .init_state = initState,
        .finish_state = finishState,
        .move_state = moveState,
        .commit = nullptr,
    };

  } // namespace

  struct BackgroundEffectManager::Impl {
    struct Surface {
      Impl* manager = nullptr;
      wlr_surface* surface = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface_synced synced{};
      RegionState pending{};
      RegionState current{};
      // The object was destroyed but the surface lives: the effect region stays until the surface's next commit, as the
      // protocol requires, so the entry stays here until the surface dies or a new object reuses it.
      bool destroyPending = false;
      wl_listener surfaceDestroy{};

      // The live state slots belong to the surface's own state objects, so resolve them through the API rather than
      // through the two members above, which only seed the initial states.
      RegionState* slot(bool pendingState) {
        return static_cast<RegionState*>(
            wlr_surface_synced_get_state(&synced, pendingState ? &surface->pending : &surface->current)
        );
      }
    };

    wl_display* display = nullptr;
    wl_global* global = nullptr;
    std::unordered_map<wlr_surface*, std::unique_ptr<Surface>> surfaces;

    static void destroySurface(Surface* surface) {
      if (surface == nullptr) {
        return;
      }
      if (surface->resource != nullptr) {
        wl_resource_set_user_data(surface->resource, nullptr);
        surface->resource = nullptr;
      }
      wl_list_remove(&surface->surfaceDestroy.link);
      // Take the owner out of the map first: erasing it would free the Surface before the synced state is finished.
      // The extracted owner frees the Surface when it leaves scope, after finish.
      auto owned = surface->manager->surfaces.extract(surface->surface);
      wlr_surface_synced_finish(&surface->synced);
    }

    static void handleSurfaceWlrDestroy(wl_listener* listener, void*) {
      Surface* state;
      state = wl_container_of(listener, state, surfaceDestroy);
      destroySurface(state);
    }

    static void handleSurfaceResourceDestroy(wl_resource* resource) {
      auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
      if (surface == nullptr) {
        return;
      }
      // The object is gone, the surface is not: "The effect regions will be removed on the next commit". Drop the
      // pending request so the next commit publishes "no region", and keep the current one until then.
      surface->resource = nullptr;
      surface->destroyPending = true;
      RegionState* pending = surface->slot(true);
      pixman_region32_clear(&pending->region);
      pending->requested = false;
    }

    static void handleSurfaceSetBlurRegion(wl_client*, wl_resource* resource, wl_resource* regionResource) {
      auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
      if (surface == nullptr) {
        wl_resource_post_error(
            resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED, "surface was destroyed"
        );
        return;
      }
      RegionState* pending = surface->slot(true);
      pixman_region32_clear(&pending->region);
      pending->requested = regionResource != nullptr;
      if (regionResource != nullptr) {
        const pixman_region32_t* region = wlr_region_from_resource(regionResource);
        if (region != nullptr) {
          pixman_region32_copy(&pending->region, region);
        }
      }
    }

    static void handleSurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static const struct ext_background_effect_surface_v1_interface& surfaceImplementation() {
      static const struct ext_background_effect_surface_v1_interface implementation{
          .destroy = handleSurfaceDestroy,
          .set_blur_region = handleSurfaceSetBlurRegion,
      };
      return implementation;
    }

    static void handleGetBackgroundEffect(
        wl_client* client, wl_resource* managerResource, uint32_t id, wl_resource* surfaceResource
    ) {
      auto* impl = static_cast<Impl*>(wl_resource_get_user_data(managerResource));
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      if (surface == nullptr) {
        return;
      }
      if (const auto found = impl->surfaces.find(surface); found != impl->surfaces.end()) {
        if (!found->second->destroyPending) {
          wl_resource_post_error(
              managerResource, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
              "surface already has a background effect object"
          );
          return;
        }
        // The previous object was destroyed before its commit; the surface may bind a new one, which reuses the slot.
        Surface* reused = found->second.get();
        reused->destroyPending = false;
        RegionState* pending = reused->slot(true);
        pixman_region32_clear(&pending->region);
        pending->requested = false;
        wl_resource* reusedResource = wl_resource_create(
            client, &ext_background_effect_surface_v1_interface, wl_resource_get_version(managerResource), id
        );
        if (reusedResource == nullptr) {
          reused->destroyPending = true;
          wl_client_post_no_memory(client);
          return;
        }
        reused->resource = reusedResource;
        wl_resource_set_implementation(reusedResource, &surfaceImplementation(), reused, handleSurfaceResourceDestroy);
        return;
      }

      auto owned = std::make_unique<Surface>(impl, surface);
      if (!wlr_surface_synced_init(&owned->synced, surface, &kSyncedImpl, &owned->pending, &owned->current)) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource* resource = wl_resource_create(
          client, &ext_background_effect_surface_v1_interface, wl_resource_get_version(managerResource), id
      );
      if (resource == nullptr) {
        wlr_surface_synced_finish(&owned->synced);
        wl_client_post_no_memory(client);
        return;
      }
      owned->resource = resource;
      wl_resource_set_implementation(resource, &surfaceImplementation(), owned.get(), handleSurfaceResourceDestroy);

      Surface* surfaceState = owned.get();
      surfaceState->surfaceDestroy.notify = handleSurfaceWlrDestroy;
      wl_signal_add(&surface->events.destroy, &surfaceState->surfaceDestroy);
      impl->surfaces.emplace(surface, std::move(owned));
    }

    static void handleManagerDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static const struct ext_background_effect_manager_v1_interface& managerImplementation() {
      static const struct ext_background_effect_manager_v1_interface implementation{
          .destroy = handleManagerDestroy,
          .get_background_effect = handleGetBackgroundEffect,
      };
      return implementation;
    }

    static void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
      auto* impl = static_cast<Impl*>(data);
      wl_resource* resource =
          wl_resource_create(client, &ext_background_effect_manager_v1_interface, std::min<uint32_t>(version, 1), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &managerImplementation(), impl, nullptr);
      ext_background_effect_manager_v1_send_capabilities(resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
    }
  };

  BackgroundEffectManager::BackgroundEffectManager(wl_display* display) : m_impl(std::make_unique<Impl>()) {
    m_impl->display = display;
    m_impl->global =
        wl_global_create(display, &ext_background_effect_manager_v1_interface, 1, m_impl.get(), Impl::bindManager);
  }

  BackgroundEffectManager::~BackgroundEffectManager() {
    if (m_impl->global != nullptr) {
      wl_global_destroy(m_impl->global);
    }
    for (auto& [surface, state] : m_impl->surfaces) {
      (void)surface;
      // A resource destroyed later must not call back into a Surface this manager is about to free.
      if (state->resource != nullptr) {
        wl_resource_set_user_data(state->resource, nullptr);
      }
      wl_list_remove(&state->surfaceDestroy.link);
      wlr_surface_synced_finish(&state->synced);
    }
  }

  wl_global* BackgroundEffectManager::global() const { return m_impl->global; }

  const pixman_region32_t* BackgroundEffectManager::region(wlr_surface* surface) {
    const auto found = m_impl->surfaces.find(surface);
    if (found == m_impl->surfaces.end()) {
      return nullptr;
    }
    const auto* state =
        static_cast<const RegionState*>(wlr_surface_synced_get_state(&found->second->synced, &surface->current));
    if (state == nullptr || !state->requested) {
      return nullptr;
    }
    return &state->region;
  }

} // namespace umbriel
