#include "server/drm_lease.h"

#include "core/log.h"
#include "wlr.h"

#include <memory>
#include <vector>

#if UMBRIEL_HAS_DRM_BACKEND
extern "C" {
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
}
#endif

namespace umbriel {

#if UMBRIEL_HAS_DRM_BACKEND
  namespace {
    constexpr Logger kLog("drm-lease");
  } // namespace
#endif

  class DrmLeaseManager::Impl {
  public:
    Impl(wl_display* display, wlr_backend* backend) {
#if UMBRIEL_HAS_DRM_BACKEND
      m_display = display;
      if (wlr_backend_is_multi(backend)) {
        wlr_multi_for_each_backend(
            backend, [](wlr_backend* child, void* data) { (void)static_cast<Impl*>(data)->managerFor(child); }, this
        );
      } else {
        (void)managerFor(backend);
      }
#else
      (void)display;
      (void)backend;
#endif
    }

    ~Impl() { setActive(false); }

    [[nodiscard]] bool offerOutput(wlr_output* output) {
      if (!output->non_desktop) {
        return false;
      }
#if UMBRIEL_HAS_DRM_BACKEND
      if (!wlr_output_is_drm(output)) {
        return false;
      }
      // A GPU can arrive after startup. wlroots only enumerates the backends
      // present when its manager is created, so enroll each DRM backend once.
      wlr_drm_lease_v1_manager* manager = managerFor(output->backend);
      if (manager != nullptr && wlr_drm_lease_v1_manager_offer_output(manager, output)) {
        kLog.info("offered non-desktop output {} for leasing", output->name);
        return true;
      }
#endif
      return false;
    }

    void setActive(bool active) {
#if UMBRIEL_HAS_DRM_BACKEND
      // Revoking a lease can synchronously announce its outputs again. Close
      // the request gate first, and let the normal output path reoffer them.
      m_active = active;
      if (active) {
        return;
      }
      for (const auto& entry : m_managers) {
        if (entry->manager == nullptr) {
          continue;
        }
        wlr_drm_lease_device_v1* device = nullptr;
        wl_list_for_each(device, &entry->manager->devices, link) {
          while (!wl_list_empty(&device->leases)) {
            wlr_drm_lease_v1* lease = nullptr;
            lease = wl_container_of(device->leases.next, lease, link);
            wlr_drm_lease_v1_revoke(lease);
          }
        }
      }
#else
      (void)active;
#endif
    }

  private:
#if UMBRIEL_HAS_DRM_BACKEND
    struct Manager {
      Impl* owner = nullptr;
      wlr_drm_lease_v1_manager* manager = nullptr;
      wl_listener request{};
      wl_listener destroy{};

      ~Manager() { disconnect(); }

      void disconnect() {
        if (manager != nullptr) {
          wl_list_remove(&request.link);
          wl_list_remove(&destroy.link);
          manager = nullptr;
        }
      }
    };

    wlr_drm_lease_v1_manager* managerFor(wlr_backend* backend) {
      if (!wlr_backend_is_drm(backend)) {
        return nullptr;
      }
      for (const auto& entry : m_managers) {
        if (entry->manager == nullptr) {
          continue;
        }
        // Devices disappear with their backend, while managers live until
        // display destruction. Do not retain a removed backend's address.
        wlr_drm_lease_device_v1* device = nullptr;
        wl_list_for_each(device, &entry->manager->devices, link) {
          if (device->backend == backend) {
            return entry->manager;
          }
        }
      }
      wlr_drm_lease_v1_manager* manager = wlr_drm_lease_v1_manager_create(m_display, backend);
      if (manager == nullptr) {
        kLog.warn("could not create a DRM lease device");
        return nullptr;
      }
      auto entry = std::make_unique<Manager>();
      entry->owner = this;
      entry->manager = manager;
      entry->request.notify = onRequest;
      wl_signal_add(&manager->events.request, &entry->request);
      entry->destroy.notify = onDestroy;
      wl_signal_add(&manager->events.destroy, &entry->destroy);
      m_managers.push_back(std::move(entry));
      return manager;
    }

    static void onRequest(wl_listener* listener, void* data) {
      Manager* entry = nullptr;
      entry = wl_container_of(listener, entry, request);
      auto* request = static_cast<wlr_drm_lease_request_v1*>(data);
      bool allowed = entry->owner->m_active && request->n_connectors > 0;
      for (size_t i = 0; allowed && i < request->n_connectors; ++i) {
        const wlr_drm_lease_connector_v1* connector = request->connectors[i];
        allowed = connector->device == request->device
            && connector->output->non_desktop
            && connector->output->backend == request->device->backend;
      }
      if (!allowed) {
        wlr_drm_lease_request_v1_reject(request);
        return;
      }
      // Granting destroys the offered outputs and their connector objects.
      // wlroots reannounces outputs when the lease ends.
      if (wlr_drm_lease_request_v1_grant(request) == nullptr) {
        kLog.warn("failed to grant DRM lease request");
      }
    }

    static void onDestroy(wl_listener* listener, void* /*data*/) {
      Manager* entry = nullptr;
      entry = wl_container_of(listener, entry, destroy);
      entry->disconnect();
    }

    wl_display* m_display = nullptr;
    bool m_active = true;
    std::vector<std::unique_ptr<Manager>> m_managers;
#endif
  };

  DrmLeaseManager::DrmLeaseManager(wl_display* display, wlr_backend* backend)
      : m_impl(std::make_unique<Impl>(display, backend)) {}

  DrmLeaseManager::~DrmLeaseManager() = default;

  bool DrmLeaseManager::offerOutput(wlr_output* output) { return m_impl->offerOutput(output); }

  void DrmLeaseManager::setActive(bool active) { m_impl->setActive(active); }

} // namespace umbriel
