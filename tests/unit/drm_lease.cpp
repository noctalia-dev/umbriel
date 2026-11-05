#include "server/drm_lease.h"

#include "check.h"
#include "wlr.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include <wlr/backend/drm.h>
#include <wlr/backend/interface.h>
#include <wlr/types/wlr_drm_lease_v1.h>
}

// Run the real lease policy and multi-backend against public wlroots objects
// and Wayland signals. DRM lease provisioning is substituted so no seat or
// graphics device is opened.
namespace {
  struct Device {
    wlr_drm_lease_device_v1 base{};
    wlr_drm_lease_v1_manager manager{};
    wl_listener displayDestroy{};
    wl_listener backendDestroy{};
    bool alive = true;
  };

  struct Lease {
    wlr_drm_lease_v1 base{};
    wlr_drm_lease drm{};
    bool active = true;
  };

  struct Backend {
    wlr_backend base{};
    bool available = true;
  };

  struct Output {
    wlr_output base{};
    wlr_drm_lease_connector_v1 connector{};
    char name[15] = "test-connector";

    Output(wlr_backend* backend, bool nonDesktop) {
      base.backend = backend;
      base.non_desktop = nonDesktop;
      base.name = name;
      wl_signal_init(&base.events.destroy);
      connector.output = &base;
    }
  };

  struct State {
    std::vector<Backend*> backends;
    std::vector<std::unique_ptr<Device>> devices;
    std::vector<std::unique_ptr<Lease>> leases;
    std::vector<wlr_output*> offered;
    std::function<void()> onRevoke;
    bool failNextGrant = false;
    int attempted = 0;
    int granted = 0;
    int rejected = 0;
    int revoked = 0;

    Device* device(wlr_backend* backend) {
      for (auto& candidate : devices) {
        if (candidate->alive && candidate->base.backend == backend) {
          return candidate.get();
        }
      }
      return nullptr;
    }
  };

  State* state = nullptr;

  void destroyDevice(Device& device) {
    if (!device.alive) {
      return;
    }
    wl_signal_emit_mutable(&device.manager.events.destroy, nullptr);
    CHECK(wl_list_empty(&device.manager.events.request.listener_list));
    CHECK(wl_list_empty(&device.manager.events.destroy.listener_list));
    wl_list_remove(&device.base.link);
    wl_list_remove(&device.displayDestroy.link);
    wl_list_remove(&device.backendDestroy.link);
    device.alive = false;
  }

  void destroyDisplayDevice(wl_listener* listener, void*) {
    Device* device;
    device = wl_container_of(listener, device, displayDestroy);
    destroyDevice(*device);
  }

  void destroyBackendDevice(wl_listener* listener, void*) {
    Device* device;
    device = wl_container_of(listener, device, backendDestroy);
    while (!wl_list_empty(&device->base.leases)) {
      wlr_drm_lease_v1* lease;
      lease = wl_container_of(device->base.leases.next, lease, link);
      wlr_drm_lease_v1_revoke(lease);
    }
    // The manager survives its device until display destruction.
    wl_list_remove(&device->base.link);
    wl_list_init(&device->base.link);
    wl_list_remove(&device->backendDestroy.link);
    wl_list_init(&device->backendDestroy.link);
    device->base.backend = nullptr;
  }

  void destroyBackend(wlr_backend* base) {
    Backend* backend;
    backend = wl_container_of(base, backend, base);
    wlr_backend_finish(base);
    std::erase(state->backends, backend);
    delete backend;
  }

  constexpr wlr_backend_impl kBackendImpl{
      .start = nullptr,
      .destroy = destroyBackend,
      .get_drm_fd = nullptr,
      .test = nullptr,
      .commit = nullptr,
  };

  struct Fixture {
    State inventory;
    wl_display* display = nullptr;
    wlr_backend* multi = nullptr;
    std::unique_ptr<umbriel::DrmLeaseManager> manager;

    Fixture() {
      state = &inventory;
      display = wl_display_create();
      multi = wlr_multi_backend_create(wl_display_get_event_loop(display));
    }
    ~Fixture() {
      inventory.onRevoke = {};
      manager.reset();
      destroyDisplay();
      state = nullptr;
    }

    void destroyDisplay() {
      if (multi != nullptr) {
        wlr_backend_destroy(multi);
        multi = nullptr;
      }
      if (display != nullptr) {
        wl_display_destroy(display);
        display = nullptr;
      }
    }

    wlr_backend* addBackend(bool available = true) {
      auto* backend = new Backend{};
      wlr_backend_init(&backend->base, &kBackendImpl);
      backend->available = available;
      inventory.backends.push_back(backend);
      CHECK(wlr_multi_backend_add(multi, &backend->base));
      return &backend->base;
    }

    void initialize() { manager = std::make_unique<umbriel::DrmLeaseManager>(display, multi); }

    void request(std::initializer_list<Output*> outputs) {
      CHECK(outputs.size() != 0);
      Device* device = inventory.device((*outputs.begin())->base.backend);
      CHECK(device != nullptr);
      if (device == nullptr) {
        return;
      }
      std::vector<wlr_drm_lease_connector_v1*> connectors;
      for (Output* output : outputs) {
        output->connector.device = &device->base;
        connectors.push_back(&output->connector);
      }
      wlr_drm_lease_request_v1 request{};
      request.device = &device->base;
      request.connectors = connectors.data();
      request.n_connectors = connectors.size();
      wl_signal_emit_mutable(&device->manager.events.request, &request);
    }
  };
} // namespace

extern "C" {
bool wlr_backend_is_drm(wlr_backend* backend) {
  return state != nullptr && std::ranges::any_of(state->backends, [backend](const Backend* candidate) {
           return &candidate->base == backend;
         });
}

bool wlr_output_is_drm(wlr_output* output) { return wlr_backend_is_drm(output->backend); }

wlr_drm_lease_v1_manager* wlr_drm_lease_v1_manager_create(wl_display* display, wlr_backend* backend) {
  CHECK(wlr_backend_is_drm(backend));
  if (!wlr_backend_is_drm(backend)) {
    return nullptr;
  }
  Backend* fake;
  fake = wl_container_of(backend, fake, base);
  if (!fake->available) {
    return nullptr;
  }
  auto device = std::make_unique<Device>();
  device->manager.display = display;
  wl_list_init(&device->manager.devices);
  wl_signal_init(&device->manager.events.request);
  wl_signal_init(&device->manager.events.destroy);
  device->base.manager = &device->manager;
  device->base.backend = backend;
  wl_list_init(&device->base.leases);
  wl_list_init(&device->base.connectors);
  wl_list_insert(&device->manager.devices, &device->base.link);
  device->displayDestroy.notify = destroyDisplayDevice;
  wl_display_add_destroy_listener(display, &device->displayDestroy);
  device->backendDestroy.notify = destroyBackendDevice;
  wl_signal_add(&backend->events.destroy, &device->backendDestroy);
  auto* manager = &device->manager;
  state->devices.push_back(std::move(device));
  return manager;
}

bool wlr_drm_lease_v1_manager_offer_output(wlr_drm_lease_v1_manager* manager, wlr_output* output) {
  CHECK(output->non_desktop);
  Device* device = state->device(output->backend);
  CHECK(device != nullptr && &device->manager == manager);
  state->offered.push_back(output);
  return true;
}

wlr_drm_lease_v1* wlr_drm_lease_request_v1_grant(wlr_drm_lease_request_v1* request) {
  ++state->attempted;
  if (std::exchange(state->failNextGrant, false)) {
    return nullptr;
  }
  ++state->granted;
  auto lease = std::make_unique<Lease>();
  lease->base.device = request->device;
  lease->base.drm_lease = &lease->drm;
  wl_signal_init(&lease->drm.events.destroy);
  wl_list_insert(&request->device->leases, &lease->base.link);
  auto* result = &lease->base;
  state->leases.push_back(std::move(lease));
  return result;
}

void wlr_drm_lease_request_v1_reject(wlr_drm_lease_request_v1* request) {
  ++state->rejected;
  request->invalid = true;
}

void wlr_drm_lease_v1_revoke(wlr_drm_lease_v1* lease) {
  Lease* record;
  record = wl_container_of(lease, record, base);
  CHECK(record->active);
  if (!record->active) {
    return;
  }
  ++state->revoked;
  record->active = false;
  wl_signal_emit_mutable(&record->drm.events.destroy, &record->drm);
  wl_list_remove(&record->base.link);
  if (auto callback = std::exchange(state->onRevoke, {})) {
    callback();
  }
}
} // extern "C"

UMBRIEL_TEST(desktopOutputsRemainOutsideLeaseOffers) {
  Fixture fixture;
  wlr_backend* backend = fixture.addBackend();
  fixture.initialize();
  Output desktop(backend, false);
  Output headset(backend, true);
  CHECK(!fixture.manager->offerOutput(&desktop.base));
  CHECK(fixture.inventory.offered.empty());
  CHECK(fixture.manager->offerOutput(&headset.base));
  CHECK_EQ(fixture.inventory.offered, (std::vector<wlr_output*>{&headset.base}));
}

UMBRIEL_TEST(unavailableAndNonDrmBackendsCannotOfferOutputs) {
  Fixture fixture;
  wlr_backend* unavailable = fixture.addBackend(false);
  fixture.initialize();
  Output headset(unavailable, true);
  Output virtualOutput(fixture.multi, true);
  CHECK(!fixture.manager->offerOutput(&headset.base));
  CHECK(!fixture.manager->offerOutput(&virtualOutput.base));
  CHECK(fixture.inventory.offered.empty());
}

UMBRIEL_TEST(hotAddedBackendReceivesItsOwnLeaseDevice) {
  Fixture fixture;
  wlr_backend* first = fixture.addBackend();
  fixture.initialize();
  wlr_backend* second = fixture.addBackend();
  Output headset(second, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  CHECK(fixture.inventory.device(first) != nullptr);
  CHECK(fixture.inventory.device(second) != nullptr);
  CHECK(fixture.inventory.device(first) != fixture.inventory.device(second));
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.granted, 1);
}

UMBRIEL_TEST(leaseRequestsCannotClaimDesktopOrForeignBackendOutputs) {
  Fixture fixture;
  wlr_backend* first = fixture.addBackend();
  wlr_backend* second = fixture.addBackend();
  fixture.initialize();
  Output headset(first, true);
  Output desktop(first, false);
  Output foreign(second, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.request({&headset, &desktop});
  fixture.request({&headset, &foreign});
  CHECK_EQ(fixture.inventory.granted, 0);
  CHECK_EQ(fixture.inventory.rejected, 2);
}

UMBRIEL_TEST(inactiveSessionRevokesEveryLeaseAndRejectsRequestsUntilReactivated) {
  Fixture fixture;
  wlr_backend* first = fixture.addBackend();
  wlr_backend* second = fixture.addBackend();
  fixture.initialize();
  Output headset(first, true);
  Output sharedHeadset(first, true);
  Output otherHeadset(second, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  CHECK(fixture.manager->offerOutput(&sharedHeadset.base));
  CHECK(fixture.manager->offerOutput(&otherHeadset.base));
  fixture.request({&headset});
  fixture.request({&sharedHeadset});
  fixture.request({&otherHeadset});
  CHECK_EQ(fixture.inventory.granted, 3);
  fixture.manager->setActive(false);
  CHECK_EQ(fixture.inventory.revoked, 3);
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.rejected, 1);
  fixture.manager->setActive(false);
  CHECK_EQ(fixture.inventory.revoked, 3);
  fixture.manager->setActive(true);
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.granted, 4);
}

UMBRIEL_TEST(removingBackendDoesNotLeaveADeviceThatCanMatchReattachment) {
  Fixture fixture;
  wlr_backend* first = fixture.addBackend();
  fixture.initialize();
  wlr_backend_destroy(first);
  wlr_backend* replacement = fixture.addBackend();
  Output headset(replacement, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.granted, 1);
}

UMBRIEL_TEST(provisioningFailureLeavesTheDeviceAvailableForAnotherRequest) {
  Fixture fixture;
  wlr_backend* backend = fixture.addBackend();
  fixture.initialize();
  Output headset(backend, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.inventory.failNextGrant = true;
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.attempted, 1);
  CHECK_EQ(fixture.inventory.granted, 0);
  CHECK(fixture.inventory.leases.empty());
  fixture.request({&headset});
  CHECK_EQ(fixture.inventory.attempted, 2);
  CHECK_EQ(fixture.inventory.granted, 1);
}

UMBRIEL_TEST(synchronousLeaseReturnReoffersOutputWithTheRequestGateAlreadyClosed) {
  Fixture fixture;
  wlr_backend* backend = fixture.addBackend();
  fixture.initialize();
  Output headset(backend, true);
  Output returned(backend, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.request({&headset});
  fixture.inventory.onRevoke = [&] {
    CHECK(fixture.manager->offerOutput(&returned.base));
    fixture.request({&returned});
  };
  fixture.manager->setActive(false);
  CHECK_EQ(fixture.inventory.offered.size(), 2U);
  CHECK_EQ(fixture.inventory.granted, 1);
  CHECK_EQ(fixture.inventory.rejected, 1);
  CHECK_EQ(fixture.inventory.revoked, 1);
}

UMBRIEL_TEST(wrapperDestructionRevokesLeasesAndDetachesDisplayOwnedManagers) {
  Fixture fixture;
  wlr_backend* backend = fixture.addBackend();
  fixture.initialize();
  Output headset(backend, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.request({&headset});
  fixture.manager.reset();
  CHECK_EQ(fixture.inventory.revoked, 1);
  for (const auto& device : fixture.inventory.devices) {
    CHECK(wl_list_empty(&device->manager.events.request.listener_list));
    CHECK(wl_list_empty(&device->manager.events.destroy.listener_list));
  }
}

UMBRIEL_TEST(displayDestructionDisconnectsManagersBeforeWrapperTeardown) {
  Fixture fixture;
  wlr_backend* backend = fixture.addBackend();
  fixture.initialize();
  Output headset(backend, true);
  CHECK(fixture.manager->offerOutput(&headset.base));
  fixture.request({&headset});
  fixture.destroyDisplay();
  CHECK_EQ(fixture.inventory.revoked, 1);
  fixture.manager->setActive(false);
  fixture.manager.reset();
}

int main() { return RUN_TESTS(); }
