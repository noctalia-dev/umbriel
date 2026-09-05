#include "server/backend_manager.h"

#include "check.h"
#include "wlr.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vector>

extern "C" {
#include <libudev.h>
#include <wlr/backend/interface.h>
#include <xf86drmMode.h>
}

// Run the real BackendManager and wlroots multi-backend against a simulated
// udev inventory and session. Only device I/O is substituted: no seat, DRM FD,
// or graphics driver is opened. Every privileged open attempt is observable.
struct udev_list_entry {
  std::string name;
  udev_list_entry* next = nullptr;
};
struct udev_device {
  std::string subsystem;
  std::string sysname;
  std::string syspath;
  std::string node;
  dev_t device = 0;
  udev_device* parent = nullptr;
  std::vector<std::string> links;
  bool present = true;
};
struct udev {};
struct udev_enumerate {
  std::vector<udev_list_entry> entries;
};

namespace {
  udev_list_entry* linkEntries(std::vector<udev_list_entry>& entries) {
    for (size_t i = 0; i < entries.size(); ++i) {
      entries[i].next = i + 1 < entries.size() ? &entries[i + 1] : nullptr;
    }
    return entries.empty() ? nullptr : &entries.front();
  }

  struct TestDevices {
    udev context;
    std::list<udev_device> inventory;
    std::vector<std::string> openedPci;
    std::vector<std::string> startedPci;
    int automaticCalls = 0;
    int nextFd = 1000;
    std::string auditPath;
    dev_t auditDevice = 0;
    int auditError = 0;

    udev_device& addGpu(unsigned int card, const std::string& pci) {
      auto& parent = inventory.emplace_back();
      parent.subsystem = "pci";
      parent.sysname = pci;
      parent.syspath = "/sys/devices/" + pci;
      auto& device = inventory.emplace_back();
      device.subsystem = "drm";
      device.parent = &parent;
      renumber(device, card);
      device.links.push_back("/dev/dri/by-path/pci-" + pci + "-card");
      return device;
    }

    udev_device& addRenderNode(udev_device& card, unsigned int minor) {
      auto& render = inventory.emplace_back();
      render.subsystem = "drm";
      render.parent = card.parent;
      render.sysname = "renderD" + std::to_string(minor);
      render.node = "/dev/dri/" + render.sysname;
      render.syspath = render.parent->syspath + "/drm/" + render.sysname;
      render.device = makedev(226, minor);
      render.links.push_back("/dev/dri/by-path/pci-" + render.parent->sysname + "-render");
      return render;
    }

    void renumber(udev_device& device, unsigned int card) {
      device.sysname = "card" + std::to_string(card);
      device.node = "/dev/dri/" + device.sysname;
      device.syspath = device.parent->syspath + "/drm/" + device.sysname;
      device.device = makedev(226, card);
      device.present = true;
    }

    udev_device* findNode(std::string_view path) {
      for (auto& device : inventory) {
        if (device.present && (device.node == path || std::ranges::contains(device.links, path))) {
          return &device;
        }
      }
      return nullptr;
    }
  };

  TestDevices* devices = nullptr;

  struct DrmBackend {
    wlr_backend base{};
    wlr_session* session = nullptr;
    wlr_device* device = nullptr;
    std::string pci;
    wl_listener remove{};
  };

  bool startDrm(wlr_backend* base) {
    DrmBackend* drm;
    drm = wl_container_of(base, drm, base);
    devices->startedPci.push_back(drm->pci);
    return true;
  }

  void destroyDrm(wlr_backend* base) {
    DrmBackend* drm;
    drm = wl_container_of(base, drm, base);
    wl_list_remove(&drm->remove.link);
    wlr_backend_finish(base);
    wlr_session_close_file(drm->session, drm->device);
    delete drm;
  }

  void removeDrm(wl_listener* listener, void*) {
    DrmBackend* drm;
    drm = wl_container_of(listener, drm, remove);
    wlr_backend_destroy(&drm->base);
  }

  const wlr_backend_impl kDrmImpl{
      .start = startDrm,
      .destroy = destroyDrm,
      .get_drm_fd = nullptr,
      .test = nullptr,
      .commit = nullptr,
  };

  class Fixture {
  public:
    Fixture() {
      if (const char* value = std::getenv("WLR_BACKENDS")) {
        m_backends = value;
      }
      setenv("WLR_BACKENDS", "drm", 1);
      devices = &inventory;
      display = wl_display_create();
    }
    ~Fixture() {
      manager.reset();
      wl_display_destroy(display);
      devices = nullptr;
      if (m_backends) {
        setenv("WLR_BACKENDS", m_backends->c_str(), 1);
      } else {
        unsetenv("WLR_BACKENDS");
      }
    }

    void start(const umbriel::Config::Drm& config) {
      manager = umbriel::BackendManager::create(display, config);
      CHECK(manager != nullptr);
      if (manager) {
        CHECK(wlr_backend_start(manager->backend()));
        manager->markStarted();
      }
    }

    void add(udev_device& device) {
      device.present = true;
      wlr_session_add_event event{.path = device.node.c_str()};
      wl_signal_emit_mutable(&manager->session()->events.add_drm_card, &event);
    }

    void remove(udev_device& node) {
      node.present = false;
      wlr_device* device = nullptr;
      wl_list_for_each(device, &manager->session()->devices, link) {
        if (device->dev == node.device) {
          wl_signal_emit_mutable(&device->events.remove, nullptr);
          break;
        }
      }
    }

    bool verifyOpenDevice(dev_t device) {
      std::FILE* file = std::tmpfile();
      CHECK(file != nullptr);
      if (file == nullptr) {
        return false;
      }
      inventory.auditPath = "/proc/self/fd/" + std::to_string(fileno(file));
      inventory.auditDevice = device;
      const bool accepted = manager->verifyOpenDevices("during descriptor audit");
      inventory.auditPath.clear();
      std::fclose(file);
      return accepted;
    }

    TestDevices inventory;
    wl_display* display = nullptr;
    std::unique_ptr<umbriel::BackendManager> manager;

  private:
    std::optional<std::string> m_backends;
  };
} // namespace

extern "C" {
// --wrap=stat limits this substitution to calls in this executable. Other paths
// retain libc behavior, including invalid selectors such as /dev/null.
#if defined(__GLIBC__) && _FILE_OFFSET_BITS == 64
#define __real_stat __real_stat64
#define __wrap_stat __wrap_stat64
#endif
int __real_stat(const char* path, struct stat* result);
int __wrap_stat(const char* path, struct stat* result) {
  // Substitute one real descriptor so the audit can outlive its udev entry.
  if (devices->auditPath == path) {
    if (devices->auditError != 0) {
      errno = devices->auditError;
      return -1;
    }
    *result = {};
    result->st_mode = S_IFCHR | 0600;
    result->st_rdev = devices->auditDevice;
    return 0;
  }
  if (!std::string_view(path).starts_with("/dev/dri/")) {
    return __real_stat(path, result);
  }
  const udev_device* device = devices->findNode(path);
  if (device == nullptr) {
    errno = ENOENT;
    return -1;
  }
  *result = {};
  result->st_mode = S_IFCHR | 0600;
  result->st_rdev = device->device;
  return 0;
}

udev_enumerate* udev_enumerate_new(udev*) { return new udev_enumerate; }
udev_enumerate* udev_enumerate_unref(udev_enumerate* enumerate) {
  delete enumerate;
  return nullptr;
}
int udev_enumerate_add_match_subsystem(udev_enumerate*, const char*) { return 0; }
int udev_enumerate_add_match_sysname(udev_enumerate*, const char*) { return 0; }
int udev_enumerate_scan_devices(udev_enumerate* enumerate) {
  for (const auto& device : devices->inventory) {
    if (device.present && device.subsystem == "drm" && device.sysname.starts_with("card")) {
      enumerate->entries.push_back({.name = device.syspath});
    }
  }
  return 0;
}
udev_list_entry* udev_enumerate_get_list_entry(udev_enumerate* enumerate) { return linkEntries(enumerate->entries); }
udev_list_entry* udev_list_entry_get_next(udev_list_entry* entry) { return entry->next; }
const char* udev_list_entry_get_name(udev_list_entry* entry) { return entry->name.c_str(); }
udev_device* udev_device_new_from_syspath(udev*, const char* path) {
  const auto found = std::ranges::find(devices->inventory, path, &udev_device::syspath);
  return found == devices->inventory.end() || !found->present ? nullptr : &*found;
}
udev_device* udev_device_new_from_devnum(udev*, char, dev_t number) {
  const auto found = std::ranges::find_if(devices->inventory, [&](const auto& device) {
    return device.present && device.device == number && device.subsystem == "drm";
  });
  return found == devices->inventory.end() ? nullptr : &*found;
}
udev_device* udev_device_unref(udev_device*) { return nullptr; }
udev_device* udev_device_get_parent(udev_device* device) { return device->parent; }
udev_device* udev_device_get_parent_with_subsystem_devtype(udev_device* device, const char* subsystem, const char*) {
  for (auto* parent = device->parent; parent != nullptr; parent = parent->parent) {
    if (parent->subsystem == subsystem) {
      return parent;
    }
  }
  return nullptr;
}
const char* udev_device_get_subsystem(udev_device* device) { return device->subsystem.c_str(); }
const char* udev_device_get_sysname(udev_device* device) { return device->sysname.c_str(); }
const char* udev_device_get_syspath(udev_device* device) { return device->syspath.c_str(); }
const char* udev_device_get_devnode(udev_device* device) { return device->node.c_str(); }
dev_t udev_device_get_devnum(udev_device* device) { return device->device; }
const char* udev_device_get_property_value(udev_device*, const char*) { return nullptr; }
const char* udev_device_get_sysattr_value(udev_device*, const char*) { return nullptr; }

wlr_session* wlr_session_create(wl_event_loop* loop) {
  auto* session = new wlr_session{};
  session->active = true;
  std::strcpy(session->seat, "seat0");
  session->udev = &devices->context;
  session->event_loop = loop;
  wl_list_init(&session->devices);
  wl_signal_init(&session->events.active);
  wl_signal_init(&session->events.add_drm_card);
  wl_signal_init(&session->events.destroy);
  return session;
}
void wlr_session_destroy(wlr_session* session) {
  wl_signal_emit_mutable(&session->events.destroy, session);
  CHECK(wl_list_empty(&session->devices));
  CHECK(wl_list_empty(&session->events.add_drm_card.listener_list));
  delete session;
}
wlr_device* wlr_session_open_file(wlr_session* session, const char* path) {
  const auto* node = devices->findNode(path);
  CHECK(node != nullptr);
  if (node == nullptr) {
    return nullptr;
  }
  devices->openedPci.push_back(node->parent->sysname);
  auto* device = new wlr_device{};
  device->fd = devices->nextFd++;
  device->dev = node->device;
  wl_list_insert(&session->devices, &device->link);
  wl_signal_init(&device->events.change);
  wl_signal_init(&device->events.remove);
  return device;
}
void wlr_session_close_file(wlr_session*, wlr_device* device) {
  wl_list_remove(&device->link);
  delete device;
}
int drmIsKMS(int fd) {
  CHECK(fd >= 1000);
  return 1;
}
wlr_backend* wlr_drm_backend_create(wlr_session* session, wlr_device* device, wlr_backend* parent) {
  CHECK(parent == nullptr);
  auto* drm = new DrmBackend{};
  wlr_backend_init(&drm->base, &kDrmImpl);
  drm->session = session;
  drm->device = device;
  drm->pci = devices->openedPci.back();
  drm->remove.notify = removeDrm;
  wl_signal_add(&device->events.remove, &drm->remove);
  return &drm->base;
}
wlr_backend* wlr_backend_autocreate(wl_event_loop* loop, wlr_session** session) {
  ++devices->automaticCalls;
  *session = nullptr;
  return wlr_multi_backend_create(loop);
}
} // extern "C"

UMBRIEL_TEST(excludedGpuRemainsUnopenedAfterNodeReuseAndReattachment) {
  Fixture fixture;
  fixture.inventory.addGpu(0, "0000:00:02.0");
  auto& ignored = fixture.inventory.addGpu(1, "0000:01:00.0");
  fixture.start({.ignoredDevices = {ignored.node}, .ignoredPciAddresses = {}});
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0"}));

  fixture.remove(ignored);
  auto& replacement = fixture.inventory.addGpu(1, "0000:03:00.0");
  fixture.add(replacement);
  fixture.inventory.renumber(ignored, 7);
  fixture.add(ignored);
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0", "0000:03:00.0"}));
  CHECK_EQ(fixture.inventory.startedPci, fixture.inventory.openedPci);

  fixture.remove(replacement);
  fixture.inventory.renumber(replacement, 4);
  fixture.add(replacement);
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0", "0000:03:00.0", "0000:03:00.0"}));
}

UMBRIEL_TEST(pciExclusionCoversGpuThatStartsBoundToVfio) {
  Fixture fixture;
  fixture.inventory.addGpu(0, "0000:00:02.0");
  fixture.start({.ignoredDevices = {}, .ignoredPciAddresses = {"0000:01:00.0"}});
  auto& ignored = fixture.inventory.addGpu(5, "0000:01:00.0");
  fixture.add(ignored);
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0"}));
}

UMBRIEL_TEST(renderSelectorKeepsItsGpuWhenTheLinkIsRetargeted) {
  Fixture fixture;
  fixture.inventory.addGpu(0, "0000:00:02.0");
  auto& ignored = fixture.inventory.addGpu(1, "0000:01:00.0");
  auto& render = fixture.inventory.addRenderNode(ignored, 129);
  const std::string selector = render.links.front();
  fixture.start({.ignoredDevices = {selector}, .ignoredPciAddresses = {}});
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0"}));

  fixture.remove(ignored);
  render.present = false;
  auto& replacement = fixture.inventory.addGpu(1, "0000:03:00.0");
  auto& replacementRender = fixture.inventory.addRenderNode(replacement, 129);
  replacementRender.links.push_back(selector);
  fixture.add(replacement);
  fixture.inventory.renumber(ignored, 7);
  fixture.add(ignored);
  CHECK_EQ(fixture.inventory.openedPci, (std::vector<std::string>{"0000:00:02.0", "0000:03:00.0"}));
}

UMBRIEL_TEST(unresolvedRenderSelectorFailsBeforeOpeningAnyGpu) {
  Fixture fixture;
  fixture.inventory.addGpu(0, "0000:00:02.0");
  // The card is visible before its render-node link. Accepting the unresolved
  // selector here would open the GPU that the user intended to exclude.
  fixture.inventory.addGpu(5, "0000:01:00.0");
  auto manager = umbriel::BackendManager::create(
      fixture.display, {.ignoredDevices = {"/dev/dri/by-path/pci-0000:01:00.0-render"}, .ignoredPciAddresses = {}}
  );
  CHECK(manager == nullptr);
  CHECK(fixture.inventory.openedPci.empty());
}

UMBRIEL_TEST(allExcludedAndInvalidSelectorsFailWithoutOpeningAnyGpu) {
  Fixture fixture;
  auto& gpu = fixture.inventory.addGpu(0, "0000:00:02.0");
  CHECK(
      umbriel::BackendManager::create(fixture.display, {.ignoredDevices = {gpu.node}, .ignoredPciAddresses = {}})
      == nullptr
  );
  CHECK(
      umbriel::BackendManager::create(fixture.display, {.ignoredDevices = {"/dev/null"}, .ignoredPciAddresses = {}})
      == nullptr
  );
  CHECK(fixture.inventory.openedPci.empty());
}

UMBRIEL_TEST(descriptorAuditRejectsExcludedGpuWhenUdevMetadataDisappears) {
  for (const bool usePciSelector : {false, true}) {
    Fixture fixture;
    auto& allowed = fixture.inventory.addGpu(0, "0000:00:02.0");
    auto& ignored = fixture.inventory.addGpu(1, "0000:01:00.0");
    auto& render = fixture.inventory.addRenderNode(ignored, 129);
    fixture.start(
        usePciSelector ? umbriel::Config::Drm{.ignoredDevices = {}, .ignoredPciAddresses = {"0000:01:00.0"}}
                       : umbriel::Config::Drm{.ignoredDevices = {ignored.node}, .ignoredPciAddresses = {}}
    );

    CHECK(fixture.verifyOpenDevice(allowed.device));
    CHECK(!fixture.verifyOpenDevice(ignored.device));
    CHECK(!fixture.verifyOpenDevice(render.device));
    render.present = false;
    CHECK(!fixture.verifyOpenDevice(render.device));
    ignored.present = false;
    CHECK(!fixture.verifyOpenDevice(ignored.device));

    allowed.parent->subsystem = "platform";
    CHECK(fixture.verifyOpenDevice(allowed.device));
    allowed.parent = nullptr;
    CHECK(!fixture.verifyOpenDevice(allowed.device));
    CHECK(fixture.verifyOpenDevice(makedev(1, 3)));
  }
}

UMBRIEL_TEST(automaticAndHeadlessPathsKeepWlrootsDiscovery) {
  Fixture fixture;
  fixture.start({});
  CHECK_EQ(fixture.inventory.automaticCalls, 1);
  fixture.manager.reset();
  setenv("WLR_BACKENDS", "headless", 1);
  fixture.start({.ignoredDevices = {"/dev/dri/missing-render-node"}, .ignoredPciAddresses = {"0000:01:00.0"}});
  CHECK_EQ(fixture.inventory.automaticCalls, 2);
  CHECK(fixture.inventory.openedPci.empty());
}

UMBRIEL_TEST(descriptorAuditRejectsUnreadableDescriptorsButAllowsClosedOnes) {
  Fixture fixture;
  fixture.inventory.addGpu(0, "0000:00:02.0");
  auto& ignored = fixture.inventory.addGpu(1, "0000:01:00.0");
  fixture.start({.ignoredDevices = {}, .ignoredPciAddresses = {"0000:01:00.0"}});
  fixture.inventory.auditError = EACCES;
  CHECK(!fixture.verifyOpenDevice(ignored.device));
  fixture.inventory.auditError = ENOENT;
  CHECK(fixture.verifyOpenDevice(ignored.device));
}

int main() { return RUN_TESTS(); }
