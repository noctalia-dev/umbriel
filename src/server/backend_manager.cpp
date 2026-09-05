#include "server/backend_manager.h"

#include "core/log.h"
#include "server/drm_policy.h"
#include "wlr.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#if UMBRIEL_HAS_NATIVE_DRM_POLICY
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <libudev.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

namespace umbriel {

  namespace {

    constexpr Logger kLog("drm");

#if UMBRIEL_HAS_NATIVE_DRM_POLICY
    constexpr auto kDeviceWaitTimeout = std::chrono::seconds(10);
    // Linux reserves this character-device major for DRM, including removed nodes.
    constexpr unsigned int kDrmDeviceMajor = 226;
    // NVIDIA reserves minors 0-247 for GPUs; 254 and 255 are shared devices.
    constexpr unsigned int kNvidiaDeviceMajor = 195;
    constexpr unsigned int kNvidiaGpuMinorMax = 247;

    struct UdevDeviceDeleter {
      void operator()(udev_device* device) const { udev_device_unref(device); }
    };
    struct UdevEnumerateDeleter {
      void operator()(udev_enumerate* enumerate) const { udev_enumerate_unref(enumerate); }
    };
    using UdevDevice = std::unique_ptr<udev_device, UdevDeviceDeleter>;
    using UdevEnumerate = std::unique_ptr<udev_enumerate, UdevEnumerateDeleter>;

    bool hasNumericSuffix(std::string_view value, std::string_view prefix) {
      if (!value.starts_with(prefix) || value.size() == prefix.size()) {
        return false;
      }
      return std::ranges::all_of(value.substr(prefix.size()), [](char character) {
        return character >= '0' && character <= '9';
      });
    }

    bool isDrmNode(udev_device* device) {
      const char* subsystem = udev_device_get_subsystem(device);
      const char* sysname = udev_device_get_sysname(device);
      return subsystem != nullptr
          && std::string_view(subsystem) == "drm"
          && sysname != nullptr
          && (hasNumericSuffix(sysname, "card") || hasNumericSuffix(sysname, "renderD"));
    }

    std::string physicalDevicePath(udev_device* device) {
      for (udev_device* parent = udev_device_get_parent(device); parent != nullptr;
           parent = udev_device_get_parent(parent)) {
        const char* subsystem = udev_device_get_subsystem(parent);
        if (subsystem != nullptr && std::string_view(subsystem) == "drm") {
          continue;
        }
        if (const char* syspath = udev_device_get_syspath(parent)) {
          return syspath;
        }
      }
      return {};
    }

    std::optional<std::string> pciAddress(udev_device* device) {
      udev_device* pci = udev_device_get_parent_with_subsystem_devtype(device, "pci", nullptr);
      if (pci == nullptr) {
        return std::nullopt;
      }
      const char* sysname = udev_device_get_sysname(pci);
      return sysname == nullptr ? std::nullopt : std::optional<std::string>(sysname);
    }

    DrmDeviceIdentity physicalIdentityFromUdev(udev_device* device, dev_t number, std::string_view fallbackNode = {}) {
      DrmDeviceIdentity identity;
      identity.node = fallbackNode;
      identity.device = number;
      if (device != nullptr) {
        if (const char* node = udev_device_get_devnode(device)) {
          identity.node = node;
        }
        identity.physicalDevice = physicalDevicePath(device);
        identity.pciAddress = pciAddress(device);
      }
      return identity;
    }

    std::optional<DrmDeviceIdentity> identityFromDevnum(udev* context, dev_t device, std::string_view node = {}) {
      UdevDevice udevDevice(udev_device_new_from_devnum(context, 'c', device));
      if (!udevDevice || !isDrmNode(udevDevice.get())) {
        return std::nullopt;
      }
      return physicalIdentityFromUdev(udevDevice.get(), udev_device_get_devnum(udevDevice.get()), node);
    }

    std::optional<unsigned int> nvidiaDeviceMinor(const std::filesystem::path& informationPath) {
      std::ifstream information(informationPath);
      std::string line;
      while (std::getline(information, line)) {
        if (const auto minorNumber = parseNvidiaDeviceMinor(line)) {
          return minorNumber;
        }
      }
      return std::nullopt;
    }

    // Some NVIDIA nodes lack a udev parent; driver metadata supplies their PCI addresses.
    std::optional<std::vector<std::pair<dev_t, std::string>>> nvidiaPciDevices() {
      std::error_code error;
      std::filesystem::directory_iterator gpu("/proc/driver/nvidia/gpus", error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory) {
          return std::vector<std::pair<dev_t, std::string>>{};
        }
        kLog.error("cannot inspect proprietary NVIDIA GPU metadata: {}", error.message());
        return std::nullopt;
      }

      std::vector<std::pair<dev_t, std::string>> devices;
      const std::filesystem::directory_iterator end;
      while (gpu != end) {
        // Missing records only matter if an open GPU needs their PCI identity.
        if (const auto minorNumber = nvidiaDeviceMinor(gpu->path() / "information")) {
          const std::filesystem::path node = "/dev" / std::filesystem::path("nvidia" + std::to_string(*minorNumber));
          struct stat statBuffer{};
          if (stat(node.c_str(), &statBuffer) == 0 && S_ISCHR(statBuffer.st_mode)) {
            devices.emplace_back(statBuffer.st_rdev, gpu->path().filename().string());
          }
        }
        gpu.increment(error);
        if (error) {
          kLog.error("cannot continue inspecting proprietary NVIDIA GPU metadata: {}", error.message());
          return std::nullopt;
        }
      }
      return devices;
    }

    std::expected<DrmDeviceIdentity, std::string> resolveDrmPath(udev* context, const std::string& path) {
      struct stat statBuffer{};
      if (stat(path.c_str(), &statBuffer) != 0) {
        return std::unexpected(std::string("stat failed: ") + std::strerror(errno));
      }
      if (!S_ISCHR(statBuffer.st_mode)) {
        return std::unexpected("not a character device");
      }
      auto identity = identityFromDevnum(context, statBuffer.st_rdev, path);
      if (!identity) {
        return std::unexpected("not a DRM card or render node");
      }
      return std::move(*identity);
    }

    bool environmentFlagEnabled(const char* name) {
      const char* value = std::getenv(name);
      return value != nullptr && std::string_view(value) == "1";
    }

    const char* exclusionMatchName(DrmExclusionMatch match) {
      switch (match) {
      case DrmExclusionMatch::DeviceNumber:
        return "device number";
      case DrmExclusionMatch::PhysicalDevice:
        return "physical GPU";
      case DrmExclusionMatch::PciAddress:
        return "PCI address";
      case DrmExclusionMatch::None:
        return "none";
      }
      return "unknown";
    }

    void disconnectListener(wl_listener& listener) {
      if (listener.notify == nullptr) {
        return;
      }
      wl_list_remove(&listener.link);
      listener.notify = nullptr;
    }

#endif

  } // namespace

  class BackendManager::Impl {
  public:
    Impl(wl_display* display, Config::Drm config) : m_display(display), m_config(std::move(config)) {}

    ~Impl() {
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      m_destroying = true;
      disconnectSessionListeners();
#endif
      if (m_backend != nullptr) {
        wlr_backend_destroy(m_backend);
      }
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      if (m_policyActive && m_session != nullptr) {
        wlr_session_destroy(m_session);
      }
#endif
    }

    [[nodiscard]] bool initialize() {
      const char* configuredBackends = std::getenv("WLR_BACKENDS");
      const DrmBackendEnvironment environment = resolveDrmBackendEnvironment(
          configuredBackends == nullptr ? std::nullopt : std::optional<std::string_view>(configuredBackends),
          std::getenv("WAYLAND_DISPLAY") != nullptr, std::getenv("WAYLAND_SOCKET") != nullptr,
          std::getenv("DISPLAY") != nullptr
      );
      m_policyActive = m_config.configured() && environment.native;
      if (m_config.configured() && !environment.native) {
        kLog.info("configuration is inactive because the selected backend is nested or headless");
      }

      if (!m_policyActive) {
        m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display), &m_session);
        return m_backend != nullptr;
      }

#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      if (!environment.exclusionsSupported) {
        kLog.error(
            "DRM exclusion supports one drm backend and at most one libinput backend; mixed native backends cannot be "
            "filtered safely"
        );
        return false;
      }
      return initializeFiltered(environment);
#else
      kLog.error("DRM policy requires wlroots DRM backend and libudev support in this build");
      return false;
#endif
    }

    [[nodiscard]] wlr_backend* backend() const { return m_backend; }
    [[nodiscard]] wlr_session* session() const { return m_session; }

    [[nodiscard]] wlr_renderer* createRenderer() {
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      if (m_policyActive) {
        const bool forceSoftware = environmentFlagEnabled("WLR_RENDERER_FORCE_SOFTWARE");
        if (forceSoftware) {
          kLog.error(
              "WLR_RENDERER_FORCE_SOFTWARE=1 cannot guarantee DRM exclusions; refusing to enumerate EGL devices"
          );
          return nullptr;
        }
        return createPrimaryRenderer();
      }
#endif
      return fx_renderer_create(m_backend);
    }

    [[nodiscard]] bool verifyOpenDevices([[maybe_unused]] std::string_view context) {
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      return !m_policyActive || verifyNoExcludedDeviceIsOpen(context);
#else
      return true;
#endif
    }

    void markStarted() {
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
      m_started = true;
#endif
    }

  private:
    wl_display* m_display = nullptr;
    Config::Drm m_config;
    bool m_policyActive = false;
    wlr_backend* m_backend = nullptr;
    wlr_session* m_session = nullptr;

#if UMBRIEL_HAS_NATIVE_DRM_POLICY
    struct DrmBackendRecord {
      Impl* owner = nullptr;
      wlr_backend* backend = nullptr;
      DrmDeviceIdentity identity;
      wl_listener destroy{};
    };

    [[nodiscard]] bool initializeFiltered(const DrmBackendEnvironment& environment) {
      logEnvironmentPrecedence();
      wl_event_loop* loop = wl_display_get_event_loop(m_display);
      m_backend = wlr_multi_backend_create(loop);
      if (m_backend == nullptr) {
        kLog.error("failed to create the filtered multi-backend");
        return false;
      }

      m_session = wlr_session_create(loop);
      if (m_session == nullptr) {
        kLog.error("failed to create a native session");
        return false;
      }
      connectSessionListeners();
      if (!waitForActiveSession()) {
        return false;
      }
      if (!resolveIgnoredDevices()) {
        return false;
      }
      if (environment.createLibinput
          && environment.libinputBeforeDrm
          && !createLibinputBackend(environment.allowMissingLibinput)) {
        return false;
      }

      auto candidates = enumerateDrmCards();
      if (!candidates) {
        return false;
      }
      if (candidates->empty()) {
        kLog.info("waiting up to 10 seconds for a DRM card");
        if (!waitForDrmCard()) {
          return false;
        }
        candidates = enumerateDrmCards();
        if (!candidates) {
          return false;
        }
      }
      if (candidates->empty()) {
        kLog.error("no DRM cards were found for seat {}", m_session->seat);
        return false;
      }

      for (unsigned int attempt = 0; attempt < 2 && m_primary == nullptr; ++attempt) {
        bool anyAllowed = false;
        for (const DrmDeviceIdentity& candidate : *candidates) {
          const DrmExclusionMatch match = drmExclusionMatch(candidate, m_ignoredDevices, m_config.ignoredPciAddresses);
          if (match != DrmExclusionMatch::None) {
            kLog.info("excluding {} before open (matched {})", candidate.node, exclusionMatchName(match));
            continue;
          }
          anyAllowed = true;
          openCandidate(candidate);
        }
        if (!anyAllowed) {
          kLog.error("all DRM cards on seat {} are excluded", m_session->seat);
          return false;
        }

        if (m_primary == nullptr && attempt == 0) {
          kLog.warn("no allowed DRM card initialized; retrying once from a fresh udev enumeration");
          candidates = enumerateDrmCards();
          if (!candidates || candidates->empty()) {
            break;
          }
        }
      }
      if (m_primary == nullptr) {
        kLog.error("could not create a backend on any allowed DRM card");
        return false;
      }
      if (environment.createLibinput
          && !environment.libinputBeforeDrm
          && !createLibinputBackend(environment.allowMissingLibinput)) {
        return false;
      }

      m_initializing = false;
      for (const std::string& path : std::exchange(m_pendingDrmCards, {})) {
        handleAddedDrmCard(path);
      }
      return true;
    }

    void logEnvironmentPrecedence() const {
      if (std::getenv("WLR_DRM_DEVICES") != nullptr) {
        kLog.warn("ignoring inherited WLR_DRM_DEVICES because DRM exclusions are configured");
      }
      if (std::getenv("WLR_RENDER_DRM_DEVICE") != nullptr) {
        kLog.warn("ignoring inherited WLR_RENDER_DRM_DEVICE because [drm] is configured");
      }
    }

    [[nodiscard]] bool waitForActiveSession() {
      if (m_session->active) {
        return true;
      }
      kLog.info("waiting up to 10 seconds for the native session to become active");
      const auto deadline = std::chrono::steady_clock::now() + kDeviceWaitTimeout;
      while (m_session != nullptr && !m_session->active) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          break;
        }
        if (wl_event_loop_dispatch(wl_display_get_event_loop(m_display), static_cast<int>(remaining.count())) < 0) {
          kLog.error("failed while waiting for the session: {}", std::strerror(errno));
          return false;
        }
      }
      if (m_session == nullptr || !m_session->active) {
        kLog.error("timed out waiting for the native session");
        return false;
      }
      return true;
    }

    [[nodiscard]] bool waitForDrmCard() {
      m_addSeen = false;
      const auto deadline = std::chrono::steady_clock::now() + kDeviceWaitTimeout;
      while (m_session != nullptr && !m_addSeen) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          break;
        }
        if (wl_event_loop_dispatch(wl_display_get_event_loop(m_display), static_cast<int>(remaining.count())) < 0) {
          kLog.error("failed while waiting for a DRM card: {}", std::strerror(errno));
          return false;
        }
      }
      return m_session != nullptr;
    }

    [[nodiscard]] bool createLibinputBackend(bool allowMissing) {
      wlr_backend* backend = wlr_libinput_backend_create(m_session);
      if (backend == nullptr) {
        if (allowMissing && environmentFlagEnabled("WLR_LIBINPUT_NO_DEVICES")) {
          kLog.info("starting without libinput because WLR_LIBINPUT_NO_DEVICES=1");
          return true;
        }
        kLog.error("failed to create the libinput backend");
        return false;
      }
      if (!wlr_multi_backend_add(m_backend, backend)) {
        kLog.error("failed to add libinput to the multi-backend");
        wlr_backend_destroy(backend);
        return false;
      }
      return true;
    }

    [[nodiscard]] std::optional<std::vector<DrmDeviceIdentity>> enumerateDrmCards() const {
      UdevEnumerate enumerate(udev_enumerate_new(m_session->udev));
      if (!enumerate) {
        kLog.error("failed to allocate a udev DRM enumeration");
        return std::nullopt;
      }
      udev_enumerate_add_match_subsystem(enumerate.get(), "drm");
      udev_enumerate_add_match_sysname(enumerate.get(), "card[0-9]*");
      if (udev_enumerate_scan_devices(enumerate.get()) != 0) {
        kLog.error("failed to enumerate DRM cards through udev");
        return std::nullopt;
      }

      std::vector<DrmDeviceIdentity> candidates;
      for (udev_list_entry* entry = udev_enumerate_get_list_entry(enumerate.get()); entry != nullptr;
           entry = udev_list_entry_get_next(entry)) {
        const char* syspath = udev_list_entry_get_name(entry);
        if (syspath == nullptr) {
          continue;
        }
        UdevDevice device(udev_device_new_from_syspath(m_session->udev, syspath));
        if (!device || !isDrmNode(device.get())) {
          continue;
        }
        const char* seat = udev_device_get_property_value(device.get(), "ID_SEAT");
        seat = seat == nullptr ? "seat0" : seat;
        if (m_session->seat[0] != '\0' && std::string_view(m_session->seat) != seat) {
          continue;
        }
        auto identity = physicalIdentityFromUdev(device.get(), udev_device_get_devnum(device.get()));
        if (identity.node.empty()) {
          continue;
        }
        bool bootDisplay = false;
        if (const char* value = udev_device_get_sysattr_value(device.get(), "boot_display")) {
          bootDisplay = std::string_view(value) == "1";
        }
        if (!bootDisplay) {
          udev_device* pci = udev_device_get_parent_with_subsystem_devtype(device.get(), "pci", nullptr);
          if (pci != nullptr) {
            if (const char* value = udev_device_get_sysattr_value(pci, "boot_vga")) {
              bootDisplay = std::string_view(value) == "1";
            }
          }
        }
        candidates.push_back(std::move(identity));
        if (bootDisplay) {
          // Match wlr_session_find_gpus(): retain udev enumeration order, but
          // move the boot display to the first slot.
          std::swap(candidates.front(), candidates.back());
        }
      }
      return candidates;
    }

    // Resolve all configured paths before opening any GPU. Runtime filtering
    // uses these physical identities even after paths disappear or get reused.
    [[nodiscard]] bool resolveIgnoredDevices() {
      m_ignoredDevices.reserve(m_config.ignoredDevices.size());
      for (const std::string& path : m_config.ignoredDevices) {
        auto identity = resolveDrmPath(m_session->udev, path);
        if (!identity) {
          kLog.error(
              "cannot resolve ignored DRM selector {}: {}. Use ignored_pci_addresses for a GPU that is absent or "
              "bound to vfio-pci at startup",
              path, identity.error()
          );
          return false;
        }
        if (!identity->pciAddress && identity->physicalDevice.empty()) {
          kLog.error("ignored DRM selector {} has no stable GPU identity; use ignored_pci_addresses instead", path);
          return false;
        }
        m_ignoredDevices.push_back(std::move(*identity));
        kLog.info("resolved ignored DRM selector {}", path);
      }
      return true;
    }

    void openCandidate(const DrmDeviceIdentity& candidate) {
      struct stat preOpenStat{};
      if (stat(candidate.node.c_str(), &preOpenStat) != 0 || !S_ISCHR(preOpenStat.st_mode)) {
        kLog.warn("allowed DRM card {} disappeared before open", candidate.node);
        return;
      }
      if (preOpenStat.st_rdev != candidate.device) {
        kLog.warn("allowed DRM card {} was re-enumerated before open; deferring it", candidate.node);
        return;
      }
      auto identity = identityFromDevnum(m_session->udev, preOpenStat.st_rdev, candidate.node);
      if (!identity) {
        kLog.warn("allowed DRM card {} no longer has a valid udev identity", candidate.node);
        return;
      }
      const DrmExclusionMatch preOpenMatch =
          drmExclusionMatch(*identity, m_ignoredDevices, m_config.ignoredPciAddresses);
      if (preOpenMatch != DrmExclusionMatch::None) {
        kLog.info(
            "excluding {} at the final pre-open check (matched {})", candidate.node, exclusionMatchName(preOpenMatch)
        );
        return;
      }

      wlr_device* device = wlr_session_open_file(m_session, identity->node.c_str());
      if (device == nullptr) {
        kLog.warn("failed to open allowed DRM card {}", identity->node);
        return;
      }
      if (device->dev != identity->device) {
        kLog.warn("DRM card {} changed identity while opening; deferring it", identity->node);
        wlr_session_close_file(m_session, device);
        return;
      }
      auto openedIdentity = identityFromDevnum(m_session->udev, device->dev, identity->node);
      if (!openedIdentity
          || drmExclusionMatch(*openedIdentity, m_ignoredDevices, m_config.ignoredPciAddresses)
              != DrmExclusionMatch::None) {
        kLog.error("DRM card {} became excluded while opening; closing it without creating a backend", identity->node);
        wlr_session_close_file(m_session, device);
        return;
      }
      if (drmIsKMS(device->fd) == 0) {
        kLog.warn("allowed DRM node {} is not KMS-capable", identity->node);
        wlr_session_close_file(m_session, device);
        return;
      }

      const bool primary = m_primary == nullptr;
      // A parent makes wlroots create an internal multi-GPU renderer through
      // EGL device enumeration. Independent backends keep exclusions strict;
      // secondary outputs import the compositor's DMA-BUFs directly instead.
      wlr_backend* drm = wlr_drm_backend_create(m_session, device, nullptr);
      if (drm == nullptr) {
        closeDeviceIfOwned(device);
        kLog.warn("failed to create a DRM backend for {}", identity->node);
        return;
      }
      if (!wlr_multi_backend_add(m_backend, drm)) {
        kLog.warn("failed to add {} to the multi-backend", identity->node);
        wlr_backend_destroy(drm);
        return;
      }

      auto record = std::make_unique<DrmBackendRecord>();
      record->owner = this;
      record->backend = drm;
      record->identity = std::move(*openedIdentity);
      record->destroy.notify = onDrmBackendDestroy;
      wl_signal_add(&drm->events.destroy, &record->destroy);
      if (primary) {
        m_primary = drm;
        kLog.info("selected {} as the primary DRM card", identity->node);
      } else {
        kLog.info("added allowed secondary DRM card {}", identity->node);
      }
      m_drmBackends.push_back(std::move(record));

      if (m_started && !wlr_backend_start(drm)) {
        kLog.warn("failed to start hotplugged DRM card {}", identity->node);
        wlr_multi_backend_remove(m_backend, drm);
        wlr_backend_destroy(drm);
      }
    }

    void closeDeviceIfOwned(wlr_device* candidate) const {
      wlr_device* device = nullptr;
      wl_list_for_each(device, &m_session->devices, link) {
        if (device == candidate) {
          wlr_session_close_file(m_session, device);
          return;
        }
      }
    }

    void handleAddedDrmCard(const std::string& path) {
      if (m_session == nullptr) {
        return;
      }

      auto identity = resolveDrmPath(m_session->udev, path);
      if (!identity) {
        kLog.warn("ignoring DRM add event for {}: {}", path, identity.error());
        return;
      }
      const bool alreadyActive = std::ranges::any_of(m_drmBackends, [&](const auto& record) {
        return sameDrmPhysicalDevice(record->identity, *identity);
      });
      if (alreadyActive) {
        return;
      }

      const DrmExclusionMatch match = drmExclusionMatch(*identity, m_ignoredDevices, m_config.ignoredPciAddresses);
      if (match != DrmExclusionMatch::None) {
        kLog.info("keeping reattached DRM card {} excluded (matched {})", path, exclusionMatchName(match));
        return;
      }
      openCandidate(*identity);
    }

    [[nodiscard]] wlr_renderer* createPrimaryRenderer() {
      if (m_primary == nullptr) {
        kLog.error("cannot create an exclusion-safe renderer without a primary DRM backend");
        return nullptr;
      }
      const int fd = wlr_backend_get_drm_fd(m_primary);
      if (fd < 0) {
        kLog.error("the primary DRM backend did not expose a renderer device");
        return nullptr;
      }
      wlr_renderer* renderer = fx_renderer_create_with_drm_fd_gbm(fd);
      if (renderer == nullptr) {
        return nullptr;
      }

      const auto primary =
          std::ranges::find_if(m_drmBackends, [&](const auto& record) { return record->backend == m_primary; });
      auto rendererDevice = rendererIdentity(renderer);
      if (primary == m_drmBackends.end()
          || !rendererDevice
          || !sameDrmPhysicalDevice((*primary)->identity, *rendererDevice)) {
        kLog.error("renderer did not initialize on the primary DRM device");
        wlr_renderer_destroy(renderer);
        return nullptr;
      }
      return renderer;
    }

    [[nodiscard]] bool verifyNoExcludedDeviceIsOpen(std::string_view context) const {
      if (m_session == nullptr) {
        kLog.error("native session is unavailable {}", context);
        return false;
      }
      std::error_code error;
      std::filesystem::directory_iterator descriptor("/proc/self/fd", error);
      if (error) {
        kLog.error("cannot inspect open devices {}: {}", context, error.message());
        return false;
      }

      std::optional<std::vector<std::pair<dev_t, std::string>>> nvidiaPciByDevice;
      const std::filesystem::directory_iterator end;
      while (descriptor != end) {
        struct stat statBuffer{};
        if (stat(descriptor->path().c_str(), &statBuffer) != 0) {
          const int statError = errno;
          if (statError != ENOENT) {
            kLog.error(
                "cannot inspect descriptor {} {}: {}", descriptor->path().string(), context, std::strerror(statError)
            );
            return false;
          }
        } else if (S_ISCHR(statBuffer.st_mode)) {
          std::error_code targetError;
          const std::filesystem::path target = std::filesystem::read_symlink(descriptor->path(), targetError);
          const std::string fallbackNode = targetError ? descriptor->path().string() : target.string();
          UdevDevice device(udev_device_new_from_devnum(m_session->udev, 'c', statBuffer.st_rdev));
          auto identity = physicalIdentityFromUdev(device.get(), statBuffer.st_rdev, fallbackNode);
          if (major(statBuffer.st_rdev) == kDrmDeviceMajor && !identity.pciAddress && identity.physicalDevice.empty()) {
            kLog.error(
                "cannot attribute open DRM device {} {} because its GPU identity is unavailable", identity.node, context
            );
            return false;
          }
          const bool nvidiaGpu =
              (major(statBuffer.st_rdev) == kNvidiaDeviceMajor && minor(statBuffer.st_rdev) <= kNvidiaGpuMinorMax)
              || isNvidiaGpuDeviceNode(identity.node);
          if (!identity.pciAddress && nvidiaGpu) {
            if (!nvidiaPciByDevice) {
              nvidiaPciByDevice = nvidiaPciDevices();
              if (!nvidiaPciByDevice) {
                return false;
              }
            }
            const auto nvidia = std::ranges::find_if(*nvidiaPciByDevice, [&](const auto& gpu) {
              return gpu.first == statBuffer.st_rdev;
            });
            if (nvidia == nvidiaPciByDevice->end()) {
              kLog.error(
                  "cannot attribute open NVIDIA GPU device {} {} because its PCI metadata is unavailable",
                  identity.node, context
              );
              return false;
            }
            identity.pciAddress = nvidia->second;
          }
          const DrmExclusionMatch match = drmExclusionMatch(identity, m_ignoredDevices, m_config.ignoredPciAddresses);
          if (match != DrmExclusionMatch::None) {
            kLog.error("excluded device {} is open {} (matched {})", identity.node, context, exclusionMatchName(match));
            return false;
          }
        }
        descriptor.increment(error);
        if (error) {
          kLog.error("cannot continue inspecting open devices {}: {}", context, error.message());
          return false;
        }
      }
      return true;
    }

    [[nodiscard]] std::optional<DrmDeviceIdentity> rendererIdentity(wlr_renderer* renderer) const {
      if (m_session == nullptr) {
        return std::nullopt;
      }
      const int fd = wlr_renderer_get_drm_fd(renderer);
      if (fd < 0) {
        return std::nullopt;
      }
      struct stat statBuffer{};
      if (fstat(fd, &statBuffer) != 0 || !S_ISCHR(statBuffer.st_mode)) {
        return std::nullopt;
      }
      return identityFromDevnum(m_session->udev, statBuffer.st_rdev);
    }

    void connectSessionListeners() {
      m_sessionAdd.notify = onSessionAdd;
      wl_signal_add(&m_session->events.add_drm_card, &m_sessionAdd);
      m_sessionDestroy.notify = onSessionDestroy;
      wl_signal_add(&m_session->events.destroy, &m_sessionDestroy);
    }

    void disconnectSessionListeners() {
      disconnectListener(m_sessionAdd);
      disconnectListener(m_sessionDestroy);
    }

    static void onSessionAdd(wl_listener* listener, void* data) {
      Impl* self;
      self = wl_container_of(listener, self, m_sessionAdd);
      self->m_addSeen = true;
      const auto* event = static_cast<const wlr_session_add_event*>(data);
      if (event == nullptr || event->path == nullptr) {
        return;
      }
      if (self->m_initializing) {
        self->m_pendingDrmCards.emplace_back(event->path);
      } else {
        self->handleAddedDrmCard(event->path);
      }
    }

    static void onSessionDestroy(wl_listener* listener, void* /*data*/) {
      Impl* self;
      self = wl_container_of(listener, self, m_sessionDestroy);
      self->disconnectSessionListeners();
      self->m_session = nullptr;
      if (!self->m_destroying) {
        kLog.error("native session was lost; terminating Umbriel");
        wl_display_terminate(self->m_display);
      }
    }

    static void onDrmBackendDestroy(wl_listener* listener, void* /*data*/) {
      DrmBackendRecord* record;
      record = wl_container_of(listener, record, destroy);
      Impl* self = record->owner;
      const bool primary = record->backend == self->m_primary;
      disconnectListener(record->destroy);
      if (primary) {
        self->m_primary = nullptr;
      }
      const auto position =
          std::ranges::find_if(self->m_drmBackends, [&](const auto& candidate) { return candidate.get() == record; });
      if (position != self->m_drmBackends.end()) {
        self->m_drmBackends.erase(position);
      }
      if (primary && !self->m_destroying) {
        kLog.error("primary DRM backend was lost; terminating Umbriel");
        wl_display_terminate(self->m_display);
      } else if (!primary && !self->m_destroying) {
        kLog.info("secondary DRM backend was removed");
      }
    }

    bool m_initializing = true;
    bool m_started = false;
    bool m_destroying = false;
    bool m_addSeen = false;
    wlr_backend* m_primary = nullptr;
    std::vector<DrmDeviceIdentity> m_ignoredDevices;
    std::vector<std::unique_ptr<DrmBackendRecord>> m_drmBackends;
    std::vector<std::string> m_pendingDrmCards;
    wl_listener m_sessionAdd{};
    wl_listener m_sessionDestroy{};
#endif
  };

  std::unique_ptr<BackendManager> BackendManager::create(wl_display* display, const Config::Drm& config) {
    if (display == nullptr) {
      return nullptr;
    }
    auto impl = std::make_unique<Impl>(display, config);
    if (!impl->initialize()) {
      return nullptr;
    }
    return std::unique_ptr<BackendManager>(new BackendManager(std::move(impl)));
  }

  BackendManager::BackendManager(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
  BackendManager::~BackendManager() = default;

  wlr_backend* BackendManager::backend() const { return m_impl->backend(); }
  wlr_session* BackendManager::session() const { return m_impl->session(); }
  wlr_renderer* BackendManager::createRenderer() { return m_impl->createRenderer(); }
  bool BackendManager::verifyOpenDevices(std::string_view context) { return m_impl->verifyOpenDevices(context); }
  void BackendManager::markStarted() { m_impl->markStarted(); }

} // namespace umbriel
