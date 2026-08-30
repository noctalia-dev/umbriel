#include "server/backend_manager.h"

#include "core/log.h"
#include "server/drm_policy.h"
#include "wlr.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <libudev.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace umbriel {

  namespace {

    constexpr Logger kLog("drm");
    constexpr auto kDeviceWaitTimeout = std::chrono::seconds(10);

    class UniqueFd {
    public:
      explicit UniqueFd(int fd = -1) : m_fd(fd) {}
      ~UniqueFd() {
        if (m_fd >= 0) {
          close(m_fd);
        }
      }
      UniqueFd(const UniqueFd&) = delete;
      UniqueFd& operator=(const UniqueFd&) = delete;
      [[nodiscard]] int get() const { return m_fd; }

    private:
      int m_fd;
    };

    class ScopedEnvironmentUnset {
    public:
      explicit ScopedEnvironmentUnset(std::string name) : m_name(std::move(name)) {
        if (const char* value = std::getenv(m_name.c_str())) {
          m_previous = value;
        }
        m_ok = unsetenv(m_name.c_str()) == 0;
        if (!m_ok) {
          kLog.error("failed to unset {}: {}", m_name, std::strerror(errno));
        }
      }

      ~ScopedEnvironmentUnset() {
        const int result = m_previous ? setenv(m_name.c_str(), m_previous->c_str(), 1) : unsetenv(m_name.c_str());
        if (result != 0) {
          kLog.error("failed to restore {}: {}", m_name, std::strerror(errno));
        }
      }

      ScopedEnvironmentUnset(const ScopedEnvironmentUnset&) = delete;
      ScopedEnvironmentUnset& operator=(const ScopedEnvironmentUnset&) = delete;
      [[nodiscard]] bool ok() const { return m_ok; }

    private:
      std::string m_name;
      std::optional<std::string> m_previous;
      bool m_ok = false;
    };

    struct UdevDeviceDeleter {
      void operator()(udev_device* device) const {
        if (device != nullptr) {
          udev_device_unref(device);
        }
      }
    };
    struct UdevEnumerateDeleter {
      void operator()(udev_enumerate* enumerate) const {
        if (enumerate != nullptr) {
          udev_enumerate_unref(enumerate);
        }
      }
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

    std::optional<DrmDeviceIdentity> identityFromUdev(udev_device* device, std::string_view fallbackNode = {}) {
      if (device == nullptr || !isDrmNode(device)) {
        return std::nullopt;
      }

      DrmDeviceIdentity identity;
      if (const char* node = udev_device_get_devnode(device)) {
        identity.node = node;
      } else {
        identity.node = fallbackNode;
      }
      identity.device = udev_device_get_devnum(device);
      identity.physicalDevice = physicalDevicePath(device);
      identity.pciAddress = pciAddress(device);
      if (const char* seat = udev_device_get_property_value(device, "ID_SEAT")) {
        identity.seat = seat;
      } else {
        identity.seat = "seat0";
      }
      for (udev_list_entry* entry = udev_device_get_devlinks_list_entry(device); entry != nullptr;
           entry = udev_list_entry_get_next(entry)) {
        if (const char* link = udev_list_entry_get_name(entry)) {
          identity.devlinks.emplace_back(link);
        }
      }
      return identity;
    }

    std::optional<DrmDeviceIdentity> identityFromDevnum(udev* context, dev_t device, std::string_view node = {}) {
      if (context == nullptr) {
        return std::nullopt;
      }
      UdevDevice udevDevice(udev_device_new_from_devnum(context, 'c', device));
      return identityFromUdev(udevDevice.get(), node);
    }

    std::optional<DrmDeviceIdentity>
    physicalIdentityFromDevnum(udev* context, dev_t device, std::string_view fallbackNode = {}) {
      if (context == nullptr) {
        return std::nullopt;
      }
      UdevDevice udevDevice(udev_device_new_from_devnum(context, 'c', device));

      DrmDeviceIdentity identity;
      if (udevDevice != nullptr && udev_device_get_devnode(udevDevice.get()) != nullptr) {
        const char* node = udev_device_get_devnode(udevDevice.get());
        identity.node = node;
      } else {
        identity.node = fallbackNode;
      }
      identity.device = device;
      if (udevDevice != nullptr) {
        identity.physicalDevice = physicalDevicePath(udevDevice.get());
        identity.pciAddress = pciAddress(udevDevice.get());
      }
      return identity;
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
        const auto minorNumber = nvidiaDeviceMinor(gpu->path() / "information");
        if (!minorNumber) {
          kLog.error("cannot resolve proprietary NVIDIA GPU metadata in {}", gpu->path().string());
          return std::nullopt;
        }
        const std::filesystem::path node = "/dev" / std::filesystem::path("nvidia" + std::to_string(*minorNumber));
        struct stat statBuffer{};
        if (stat(node.c_str(), &statBuffer) == 0 && S_ISCHR(statBuffer.st_mode)) {
          devices.emplace_back(statBuffer.st_rdev, gpu->path().filename().string());
        }
        gpu.increment(error);
        if (error) {
          kLog.error("cannot continue inspecting proprietary NVIDIA GPU metadata: {}", error.message());
          return std::nullopt;
        }
      }
      return devices;
    }

    enum class PathResolutionKind {
      Resolved,
      Missing,
      Invalid,
    };

    struct PathResolution {
      PathResolutionKind kind = PathResolutionKind::Invalid;
      std::optional<DrmDeviceIdentity> identity;
      std::string error;
    };

    PathResolution resolveDrmPath(udev* context, const std::string& path) {
      struct stat statBuffer{};
      if (stat(path.c_str(), &statBuffer) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
          return {.kind = PathResolutionKind::Missing, .identity = std::nullopt, .error = {}};
        }
        return {
            .kind = PathResolutionKind::Invalid,
            .identity = std::nullopt,
            .error = std::string("stat failed: ") + std::strerror(errno),
        };
      }
      if (!S_ISCHR(statBuffer.st_mode)) {
        return {
            .kind = PathResolutionKind::Invalid,
            .identity = std::nullopt,
            .error = "not a character device",
        };
      }
      auto identity = identityFromDevnum(context, statBuffer.st_rdev, path);
      if (!identity) {
        return {
            .kind = PathResolutionKind::Invalid,
            .identity = std::nullopt,
            .error = "not a DRM card or render node",
        };
      }
      return {.kind = PathResolutionKind::Resolved, .identity = std::move(identity), .error = {}};
    }

    bool environmentFlagEnabled(const char* name) {
      const char* value = std::getenv(name);
      return value != nullptr && std::string_view(value) == "1";
    }

    const char* exclusionMatchName(DrmExclusionMatch match) {
      switch (match) {
      case DrmExclusionMatch::Path:
        return "path";
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

  } // namespace

  class BackendManager::Impl {
  public:
    Impl(wl_display* display, Config::Drm config)
        : m_display(display), m_loop(wl_display_get_event_loop(display)), m_config(std::move(config)) {
      m_ignoredPaths.reserve(m_config.ignoredDevices.size());
      m_pathStatuses.resize(m_config.ignoredDevices.size());
      for (const std::string& path : m_config.ignoredDevices) {
        m_ignoredPaths.push_back({
            .path = path,
            .device = std::nullopt,
            .physicalDevice = {},
            .pciAddress = std::nullopt,
        });
      }
    }

    ~Impl() {
      m_destroying = true;
      if (m_retrySource != nullptr) {
        wl_event_source_remove(m_retrySource);
      }
      disconnectSessionListeners();
      if (m_backend != nullptr) {
        wlr_backend_destroy(m_backend);
      }
      m_drmBackends.clear();
      if (m_ownsSession && m_session != nullptr) {
        wlr_session_destroy(m_session);
      }
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

      if (!m_policyActive || !m_config.hasExclusions()) {
        return initializeAutomatic();
      }

      if (!environment.exclusionsSupported) {
        kLog.error(
            "DRM exclusion supports one drm backend and at most one libinput backend; mixed native backends cannot be "
            "filtered safely"
        );
        return false;
      }
      return initializeFiltered(
          environment.createLibinput, environment.libinputBeforeDrm, environment.allowMissingLibinput
      );
    }

    [[nodiscard]] wlr_backend* backend() const { return m_backend; }
    [[nodiscard]] wlr_session* session() const { return m_session; }

    [[nodiscard]] wlr_renderer* createRenderer() {
      if (!m_policyActive) {
        return fx_renderer_create(m_backend);
      }

      m_rendererDevice.reset();
      if (!activePolicyStillAllowed("before renderer creation")) {
        return nullptr;
      }

      const bool forceSoftware = environmentFlagEnabled("WLR_RENDERER_FORCE_SOFTWARE");
      if (forceSoftware && m_config.hasExclusions()) {
        kLog.error("WLR_RENDERER_FORCE_SOFTWARE=1 cannot guarantee DRM exclusions; refusing to enumerate EGL devices");
        return nullptr;
      }

      wlr_renderer* renderer = nullptr;
      if (m_config.renderDevice && !forceSoftware) {
        renderer = createConfiguredRenderer(*m_config.renderDevice);
        if (renderer == nullptr) {
          kLog.warn("falling back to the first allowed backend renderer");
        }
      } else if (m_config.renderDevice) {
        kLog.info("WLR_RENDERER_FORCE_SOFTWARE=1 overrides drm.render_device");
      }

      if (renderer == nullptr) {
        if (m_config.hasExclusions()) {
          renderer = createPrimaryRenderer();
        } else if (forceSoftware) {
          renderer = fx_renderer_create(m_backend);
        } else {
          renderer = createBackendRenderer();
        }
      }
      if (renderer == nullptr) {
        return nullptr;
      }
      if (!m_rendererDevice) {
        m_rendererDevice = rendererIdentity(renderer);
      }
      if (!verifyOpenDevices("after renderer creation")) {
        wlr_renderer_destroy(renderer);
        m_rendererDevice.reset();
        return nullptr;
      }
      return renderer;
    }

    [[nodiscard]] bool verifyOpenDevices(std::string_view context) {
      return !m_policyActive
          || !m_config.hasExclusions()
          || (activePolicyStillAllowed(context) && verifyNoExcludedDeviceIsOpen(context));
    }

    void markStarted() { m_started = true; }

  private:
    struct DrmBackendRecord {
      Impl* owner = nullptr;
      wlr_backend* backend = nullptr;
      DrmDeviceIdentity identity;
      wl_listener destroy{};
    };

    struct PathStatus {
      PathResolutionKind kind = PathResolutionKind::Missing;
      bool initialized = false;
      std::string error;
    };

    [[nodiscard]] bool initializeAutomatic() {
      if (!m_policyActive) {
        m_backend = wlr_backend_autocreate(m_loop, &m_session);
        return m_backend != nullptr;
      }

      logEnvironmentPrecedence();
      ScopedEnvironmentUnset renderDevice("WLR_RENDER_DRM_DEVICE");
      if (!renderDevice.ok()) {
        return false;
      }
      m_backend = wlr_backend_autocreate(m_loop, &m_session);
      return m_backend != nullptr;
    }

    [[nodiscard]] bool initializeFiltered(bool createLibinput, bool libinputBeforeDrm, bool allowMissingLibinput) {
      logEnvironmentPrecedence();
      ScopedEnvironmentUnset drmDevices("WLR_DRM_DEVICES");
      ScopedEnvironmentUnset renderDevice("WLR_RENDER_DRM_DEVICE");
      if (!drmDevices.ok() || !renderDevice.ok()) {
        return false;
      }
      m_backend = wlr_multi_backend_create(m_loop);
      if (m_backend == nullptr) {
        kLog.error("failed to create the filtered multi-backend");
        return false;
      }

      m_session = wlr_session_create(m_loop);
      m_ownsSession = true;
      if (m_session == nullptr) {
        kLog.error("failed to create a native session");
        return false;
      }
      connectSessionListeners();
      if (!waitForActiveSession()) {
        return false;
      }
      if (!refreshIgnoredPaths(true)) {
        return false;
      }
      if (createLibinput && libinputBeforeDrm && !createLibinputBackend(allowMissingLibinput)) {
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
        if (const size_t learned = learnDrmPathSelectorIdentities(m_ignoredPaths, *candidates); learned > 0) {
          kLog.info("learned {} missing ignored DRM selector identity from udev links", learned);
        }
        const auto allowed = allowedDrmDeviceIndices(*candidates, m_ignoredPaths, m_config.ignoredPciAddresses);
        logExcludedCandidates(*candidates);
        if (allowed.empty()) {
          kLog.error("all DRM cards on seat {} are excluded", m_session->seat);
          return false;
        }

        for (const size_t index : allowed) {
          (void)openCandidate((*candidates)[index]);
        }
        if (m_primary == nullptr && attempt == 0) {
          kLog.warn("no allowed DRM card initialized; retrying once from a fresh udev enumeration");
          if (!refreshIgnoredPaths(true)) {
            return false;
          }
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
      if (createLibinput && !libinputBeforeDrm && !createLibinputBackend(allowMissingLibinput)) {
        return false;
      }

      m_initializing = false;
      if (m_reconcilePending && !reconcile()) {
        scheduleRetry();
      }
      return true;
    }

    void logEnvironmentPrecedence() const {
      if (m_config.hasExclusions() && std::getenv("WLR_DRM_DEVICES") != nullptr) {
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
        if (wl_event_loop_dispatch(m_loop, static_cast<int>(remaining.count())) < 0) {
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
        if (wl_event_loop_dispatch(m_loop, static_cast<int>(remaining.count())) < 0) {
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
      m_libinputDestroy.notify = onLibinputDestroy;
      wl_signal_add(&backend->events.destroy, &m_libinputDestroy);
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
        if (!device) {
          continue;
        }
        const char* seat = udev_device_get_property_value(device.get(), "ID_SEAT");
        seat = seat == nullptr ? "seat0" : seat;
        if (m_session->seat[0] != '\0' && std::string_view(m_session->seat) != seat) {
          continue;
        }
        auto identity = identityFromUdev(device.get());
        if (!identity || identity->node.empty()) {
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
        candidates.push_back(std::move(*identity));
        if (bootDisplay) {
          // Match wlr_session_find_gpus(): retain udev enumeration order, but
          // move the boot display to the first slot.
          std::swap(candidates.front(), candidates.back());
        }
      }
      return candidates;
    }

    void logExcludedCandidates(const std::vector<DrmDeviceIdentity>& candidates) const {
      for (const DrmDeviceIdentity& candidate : candidates) {
        const DrmExclusionMatch match = drmExclusionMatch(candidate, m_ignoredPaths, m_config.ignoredPciAddresses);
        if (match != DrmExclusionMatch::None) {
          kLog.info("excluding {} before open (matched {})", candidate.node, exclusionMatchName(match));
        }
      }
    }

    [[nodiscard]] bool refreshIgnoredPaths(bool initial) {
      bool valid = true;
      for (size_t index = 0; index < m_ignoredPaths.size(); ++index) {
        DrmPathSelectorIdentity& selector = m_ignoredPaths[index];
        PathStatus& previous = m_pathStatuses[index];
        PathResolution resolution = resolveDrmPath(m_session->udev, selector.path);

        const bool changed =
            !previous.initialized || previous.kind != resolution.kind || previous.error != resolution.error;
        if (resolution.kind == PathResolutionKind::Resolved) {
          selector.device = resolution.identity->device;
          selector.physicalDevice = resolution.identity->physicalDevice;
          selector.pciAddress = resolution.identity->pciAddress;
          if (changed) {
            kLog.info("resolved ignored DRM selector {}", selector.path);
          }
        } else {
          selector.device.reset();
          if (resolution.kind == PathResolutionKind::Missing) {
            if (changed) {
              kLog.warn("ignored DRM selector {} is missing; it remains armed", selector.path);
            }
          } else {
            valid = false;
            if (changed) {
              kLog.error("ignored DRM selector {} is invalid: {}", selector.path, resolution.error);
            }
          }
        }
        previous = {.kind = resolution.kind, .initialized = true, .error = std::move(resolution.error)};
      }
      if (!valid && !initial) {
        kLog.error("not opening new DRM cards until every ignored path is valid or missing");
      }
      return valid;
    }

    [[nodiscard]] bool openCandidate(const DrmDeviceIdentity& candidate) {
      if (!refreshIgnoredPaths(m_initializing)) {
        return false;
      }
      struct stat preOpenStat{};
      if (stat(candidate.node.c_str(), &preOpenStat) != 0 || !S_ISCHR(preOpenStat.st_mode)) {
        kLog.warn("allowed DRM card {} disappeared before open", candidate.node);
        return false;
      }
      if (preOpenStat.st_rdev != candidate.device) {
        kLog.warn("allowed DRM card {} was re-enumerated before open; deferring it", candidate.node);
        return false;
      }
      auto identity = identityFromDevnum(m_session->udev, preOpenStat.st_rdev, candidate.node);
      if (!identity) {
        kLog.warn("allowed DRM card {} no longer has a valid udev identity", candidate.node);
        return false;
      }
      const DrmExclusionMatch preOpenMatch = drmExclusionMatch(*identity, m_ignoredPaths, m_config.ignoredPciAddresses);
      if (preOpenMatch != DrmExclusionMatch::None) {
        kLog.info(
            "excluding {} at the final pre-open check (matched {})", candidate.node, exclusionMatchName(preOpenMatch)
        );
        return true;
      }

      wlr_device* device = wlr_session_open_file(m_session, identity->node.c_str());
      if (device == nullptr) {
        kLog.warn("failed to open allowed DRM card {}", identity->node);
        return false;
      }
      if (device->dev != identity->device) {
        kLog.warn("DRM card {} changed identity while opening; deferring it", identity->node);
        wlr_session_close_file(m_session, device);
        return false;
      }
      // Resolve selectors again after the privileged open. A by-path symlink
      // can be retargeted between the pre-open check and libseat opening the
      // card; the opened identity must be checked against that newest state.
      if (!refreshIgnoredPaths(m_initializing)) {
        wlr_session_close_file(m_session, device);
        return false;
      }
      auto openedIdentity = identityFromDevnum(m_session->udev, device->dev, identity->node);
      if (!openedIdentity
          || drmExclusionMatch(*openedIdentity, m_ignoredPaths, m_config.ignoredPciAddresses)
              != DrmExclusionMatch::None) {
        kLog.error("DRM card {} became excluded while opening; closing it without creating a backend", identity->node);
        wlr_session_close_file(m_session, device);
        return true;
      }
      if (drmIsKMS(device->fd) == 0) {
        kLog.warn("allowed DRM node {} is not KMS-capable", identity->node);
        wlr_session_close_file(m_session, device);
        return false;
      }

      const bool primary = m_primary == nullptr;
      // A parent makes wlroots create an internal multi-GPU renderer through
      // EGL device enumeration. Independent backends keep exclusions strict;
      // secondary outputs import the compositor's DMA-BUFs directly instead.
      wlr_backend* drm = wlr_drm_backend_create(m_session, device, nullptr);
      if (drm == nullptr) {
        closeDeviceIfOwned(device);
        kLog.warn("failed to create a DRM backend for {}", identity->node);
        return false;
      }
      if (!wlr_multi_backend_add(m_backend, drm)) {
        kLog.warn("failed to add {} to the multi-backend", identity->node);
        wlr_backend_destroy(drm);
        return false;
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
        return false;
      }
      return true;
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

    [[nodiscard]] bool activePolicyStillAllowed(std::string_view context) {
      if (m_session == nullptr) {
        kLog.error("native session is unavailable {}", context);
        return false;
      }
      if (!refreshIgnoredPaths(false)) {
        return false;
      }
      std::vector<DrmDeviceIdentity> identities;
      identities.reserve(m_drmBackends.size());
      for (const auto& record : m_drmBackends) {
        identities.push_back(record->identity);
      }
      (void)learnDrmPathSelectorIdentities(m_ignoredPaths, identities);

      for (const auto& record : m_drmBackends) {
        if (drmExclusionMatch(record->identity, m_ignoredPaths, m_config.ignoredPciAddresses)
            != DrmExclusionMatch::None) {
          kLog.error(
              "{} DRM card {} is excluded {}", record->backend == m_primary ? "primary" : "secondary",
              record->identity.node, context
          );
          return false;
        }
      }
      if (m_rendererDevice
          && drmExclusionMatch(*m_rendererDevice, m_ignoredPaths, m_config.ignoredPciAddresses)
              != DrmExclusionMatch::None) {
        kLog.error("renderer DRM device {} is excluded {}", m_rendererDevice->node, context);
        return false;
      }
      return true;
    }

    [[nodiscard]] bool reconcile() {
      m_reconcilePending = false;
      if (m_session == nullptr || !m_session->active || m_primary == nullptr) {
        return m_primary != nullptr;
      }

      const bool selectorsValid = refreshIgnoredPaths(false);
      auto candidates = enumerateDrmCards();
      if (!candidates) {
        return false;
      }
      (void)learnDrmPathSelectorIdentities(m_ignoredPaths, *candidates);

      std::vector<DrmActiveDevice> active;
      active.reserve(m_drmBackends.size());
      for (const auto& record : m_drmBackends) {
        active.push_back({.identity = record->identity, .primary = record->backend == m_primary});
      }
      const DrmReconcilePlan plan = planDrmReconciliation(
          active, m_rendererDevice, *candidates, m_ignoredPaths, m_config.ignoredPciAddresses, selectorsValid
      );
      if (plan.termination == DrmReconcileTermination::RendererExcluded) {
        kLog.error("the active renderer GPU is now excluded; terminating the session");
        wl_display_terminate(m_display);
        return true;
      }
      if (plan.termination == DrmReconcileTermination::PrimaryExcluded) {
        kLog.error("the primary DRM card is now excluded; terminating the session");
        wl_display_terminate(m_display);
        return true;
      }

      std::vector<wlr_backend*> remove;
      remove.reserve(plan.removeActiveIndices.size());
      for (const size_t index : plan.removeActiveIndices) {
        remove.push_back(m_drmBackends[index]->backend);
      }
      for (wlr_backend* backend : remove) {
        kLog.warn("removing a secondary DRM backend that is now excluded");
        wlr_multi_backend_remove(m_backend, backend);
        wlr_backend_destroy(backend);
      }

      bool success = true;
      for (const size_t index : plan.addCandidateIndices) {
        success = openCandidate((*candidates)[index]) && success;
      }
      return selectorsValid && success;
    }

    void requestReconcile() {
      if (m_initializing) {
        m_reconcilePending = true;
        return;
      }
      if (m_retrySource != nullptr) {
        wl_event_source_remove(m_retrySource);
        m_retrySource = nullptr;
      }
      if (m_reconciling) {
        m_reconcilePending = true;
        return;
      }
      m_reconciling = true;
      bool success = true;
      do {
        success = reconcile() && success;
      } while (m_reconcilePending);
      m_reconciling = false;
      if (!success) {
        scheduleRetry();
      }
    }

    void scheduleRetry() {
      if (m_retrySource != nullptr || m_destroying) {
        return;
      }
      m_retrySource = wl_event_loop_add_idle(m_loop, onRetry, this);
      if (m_retrySource == nullptr) {
        kLog.warn("failed to schedule one DRM reconciliation retry");
      }
    }

    [[nodiscard]] wlr_renderer* createConfiguredRenderer(const std::string& path) {
      if (!refreshIgnoredPaths(false)) {
        kLog.warn("cannot safely use configured renderer while an ignored path is invalid");
        return nullptr;
      }
      PathResolution resolved = resolveDrmPath(m_session == nullptr ? nullptr : m_session->udev, path);
      if (resolved.kind == PathResolutionKind::Missing) {
        kLog.warn("configured DRM render device {} is missing", path);
        return nullptr;
      }
      if (resolved.kind == PathResolutionKind::Invalid) {
        kLog.warn("configured DRM render device {} is invalid: {}", path, resolved.error);
        return nullptr;
      }
      if (drmExclusionMatch(*resolved.identity, m_ignoredPaths, m_config.ignoredPciAddresses)
          != DrmExclusionMatch::None) {
        kLog.warn("configured DRM render device {} belongs to an excluded GPU", path);
        return nullptr;
      }

      UniqueFd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
      if (fd.get() < 0) {
        kLog.warn("failed to open configured DRM render device {}: {}", path, std::strerror(errno));
        return nullptr;
      }
      struct stat statBuffer{};
      if (fstat(fd.get(), &statBuffer) != 0 || !S_ISCHR(statBuffer.st_mode)) {
        kLog.warn("configured DRM render device {} changed while opening", path);
        return nullptr;
      }
      if (statBuffer.st_rdev != resolved.identity->device) {
        kLog.warn("configured DRM render device {} was retargeted while opening", path);
        return nullptr;
      }
      if (!refreshIgnoredPaths(false)) {
        kLog.warn("cannot safely use configured renderer while an ignored path is invalid");
        return nullptr;
      }
      auto openedIdentity = identityFromDevnum(m_session->udev, statBuffer.st_rdev, path);
      if (!openedIdentity || drmGetNodeTypeFromFd(fd.get()) != DRM_NODE_RENDER) {
        kLog.warn("configured DRM render device {} is not a render node", path);
        return nullptr;
      }
      if (m_session->seat[0] != '\0' && openedIdentity->seat != m_session->seat) {
        kLog.warn(
            "configured DRM render device {} belongs to seat {}, not {}", path, openedIdentity->seat, m_session->seat
        );
        return nullptr;
      }
      if (drmExclusionMatch(*openedIdentity, m_ignoredPaths, m_config.ignoredPciAddresses) != DrmExclusionMatch::None) {
        kLog.warn("configured DRM render device {} resolved to an excluded GPU", path);
        return nullptr;
      }

      wlr_renderer* renderer = m_config.hasExclusions() ? fx_renderer_create_with_drm_fd_gbm(fd.get())
                                                        : fx_renderer_create_with_drm_fd(fd.get());
      if (renderer == nullptr) {
        kLog.warn("failed to initialize the renderer on {}", path);
        return nullptr;
      }
      auto rendererDevice = rendererIdentity(renderer);
      if (!rendererDevice || !sameDrmPhysicalDevice(*openedIdentity, *rendererDevice)) {
        kLog.error("renderer initialized on a different DRM device than {}", path);
        wlr_renderer_destroy(renderer);
        return nullptr;
      }
      m_rendererDevice = std::move(*rendererDevice);
      kLog.info("using configured DRM render device {}", path);
      return renderer;
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
      m_rendererDevice = std::move(*rendererDevice);
      return renderer;
    }

    [[nodiscard]] wlr_renderer* createBackendRenderer() const {
      const int fd = wlr_backend_get_drm_fd(m_backend);
      if (fd >= 0) {
        return fx_renderer_create_with_drm_fd(fd);
      }

      // A native backend normally exposes its DRM FD. Preserve umbrielfx's
      // fallback for unusual mixed backends without letting an inherited
      // renderer override take precedence over [drm].
      ScopedEnvironmentUnset renderDevice("WLR_RENDER_DRM_DEVICE");
      return renderDevice.ok() ? fx_renderer_create(m_backend) : nullptr;
    }

    [[nodiscard]] bool verifyNoExcludedDeviceIsOpen(std::string_view context) const {
      std::error_code error;
      std::filesystem::directory_iterator descriptor("/proc/self/fd", error);
      if (error) {
        kLog.error("cannot inspect open devices {}: {}", context, error.message());
        return false;
      }

      const auto nvidiaPciByDevice = nvidiaPciDevices();
      if (!nvidiaPciByDevice) {
        return false;
      }
      const std::filesystem::directory_iterator end;
      while (descriptor != end) {
        struct stat statBuffer{};
        if (stat(descriptor->path().c_str(), &statBuffer) == 0 && S_ISCHR(statBuffer.st_mode)) {
          std::error_code targetError;
          const std::filesystem::path target = std::filesystem::read_symlink(descriptor->path(), targetError);
          const std::string fallbackNode = targetError ? descriptor->path().string() : target.string();
          auto identity = physicalIdentityFromDevnum(m_session->udev, statBuffer.st_rdev, fallbackNode);
          if (identity) {
            if (!identity->pciAddress) {
              const auto nvidia = std::ranges::find_if(*nvidiaPciByDevice, [&](const auto& device) {
                return device.first == statBuffer.st_rdev;
              });
              if (nvidia != nvidiaPciByDevice->end()) {
                identity->pciAddress = nvidia->second;
              } else if (isNvidiaGpuDeviceNode(identity->node)) {
                kLog.error(
                    "cannot attribute open NVIDIA GPU device {} {} because its PCI metadata is unavailable",
                    identity->node, context
                );
                return false;
              }
            }
            const DrmExclusionMatch match = drmExclusionMatch(*identity, m_ignoredPaths, m_config.ignoredPciAddresses);
            if (match != DrmExclusionMatch::None) {
              kLog.error(
                  "excluded device {} is open {} (matched {})", identity->node, context, exclusionMatchName(match)
              );
              return false;
            }
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
      m_sessionActive.notify = onSessionActive;
      wl_signal_add(&m_session->events.active, &m_sessionActive);
      m_sessionDestroy.notify = onSessionDestroy;
      wl_signal_add(&m_session->events.destroy, &m_sessionDestroy);
    }

    void disconnectSessionListeners() {
      disconnectListener(m_sessionAdd);
      disconnectListener(m_sessionActive);
      disconnectListener(m_sessionDestroy);
    }

    static void onSessionAdd(wl_listener* listener, void* /*data*/) {
      Impl* self;
      self = wl_container_of(listener, self, m_sessionAdd);
      self->m_addSeen = true;
      self->requestReconcile();
    }

    static void onSessionActive(wl_listener* listener, void* /*data*/) {
      Impl* self;
      self = wl_container_of(listener, self, m_sessionActive);
      if (self->m_session != nullptr && self->m_session->active) {
        self->requestReconcile();
      }
    }

    static void onSessionDestroy(wl_listener* listener, void* /*data*/) {
      Impl* self;
      self = wl_container_of(listener, self, m_sessionDestroy);
      self->disconnectSessionListeners();
      self->m_session = nullptr;
      self->m_ownsSession = false;
      if (!self->m_destroying) {
        kLog.error("native session was lost; terminating Umbriel");
        wl_display_terminate(self->m_display);
      }
    }

    static void onLibinputDestroy(wl_listener* listener, void* /*data*/) {
      Impl* self;
      self = wl_container_of(listener, self, m_libinputDestroy);
      disconnectListener(self->m_libinputDestroy);
      if (!self->m_destroying) {
        kLog.error("libinput backend was lost; terminating Umbriel");
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

    static void onRetry(void* data) {
      auto* self = static_cast<Impl*>(data);
      self->m_retrySource = nullptr;
      if (self->m_destroying) {
        return;
      }
      self->m_reconciling = true;
      do {
        (void)self->reconcile();
      } while (self->m_reconcilePending);
      self->m_reconciling = false;
    }

    wl_display* m_display = nullptr;
    wl_event_loop* m_loop = nullptr;
    Config::Drm m_config;
    bool m_policyActive = false;
    bool m_ownsSession = false;
    bool m_initializing = true;
    bool m_started = false;
    bool m_destroying = false;
    bool m_addSeen = false;
    bool m_reconcilePending = false;
    bool m_reconciling = false;
    wlr_backend* m_backend = nullptr;
    wlr_backend* m_primary = nullptr;
    wlr_session* m_session = nullptr;
    wl_event_source* m_retrySource = nullptr;
    std::vector<DrmPathSelectorIdentity> m_ignoredPaths;
    std::vector<PathStatus> m_pathStatuses;
    std::vector<std::unique_ptr<DrmBackendRecord>> m_drmBackends;
    std::optional<DrmDeviceIdentity> m_rendererDevice;
    wl_listener m_sessionAdd{};
    wl_listener m_sessionActive{};
    wl_listener m_sessionDestroy{};
    wl_listener m_libinputDestroy{};
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
