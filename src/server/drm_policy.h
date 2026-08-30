#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace umbriel {

  // Identity shared by a GPU's card and render nodes. physicalDevice is the
  // parent sysfs device, so selecting either node applies to the whole GPU.
  struct DrmDeviceIdentity {
    std::string node;
    dev_t device = 0;
    std::string physicalDevice;
    std::optional<std::string> pciAddress;
    std::vector<std::string> devlinks;
    std::string seat;
  };

  // Runtime resolution of one configured path. A missing selector retains its
  // last physical identity so a temporarily unbound GPU stays excluded.
  struct DrmPathSelectorIdentity {
    std::string path;
    std::optional<dev_t> device;
    std::string physicalDevice;
    std::optional<std::string> pciAddress;
  };

  enum class DrmExclusionMatch {
    None,
    Path,
    DeviceNumber,
    PhysicalDevice,
    PciAddress,
  };

  struct DrmBackendEnvironment {
    bool native = false;
    bool exclusionsSupported = false;
    bool createLibinput = false;
    bool libinputBeforeDrm = false;
    bool allowMissingLibinput = false;
    bool operator==(const DrmBackendEnvironment&) const = default;
  };

  struct DrmActiveDevice {
    DrmDeviceIdentity identity;
    bool primary = false;
  };

  enum class DrmReconcileTermination {
    None,
    RendererExcluded,
    PrimaryExcluded,
  };

  struct DrmReconcilePlan {
    DrmReconcileTermination termination = DrmReconcileTermination::None;
    std::vector<size_t> removeActiveIndices;
    std::vector<size_t> addCandidateIndices;
  };

  // Card and render nodes have different device numbers but share a physical
  // GPU. Prefer stable PCI and sysfs identities before the node number.
  [[nodiscard]] bool sameDrmPhysicalDevice(const DrmDeviceIdentity& first, const DrmDeviceIdentity& second);

  // Mirrors wlroots' WLR_BACKENDS precedence and comma tokenization without
  // touching process state. Presence of a display variable matters even when
  // its value is empty, matching wlr_backend_autocreate().
  [[nodiscard]] DrmBackendEnvironment resolveDrmBackendEnvironment(
      std::optional<std::string_view> wlrBackends, bool waylandDisplaySet, bool waylandSocketSet, bool x11DisplaySet
  );

  [[nodiscard]] DrmExclusionMatch drmExclusionMatch(
      const DrmDeviceIdentity& device, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses
  );

  // Fill unresolved path selectors from card nodes that advertise the exact
  // path as a udev link. This keeps a missing on-disk link useful and lets its
  // card identity exclude the corresponding render node too.
  [[nodiscard]] size_t
  learnDrmPathSelectorIdentities(std::span<DrmPathSelectorIdentity> paths, std::span<const DrmDeviceIdentity> devices);

  [[nodiscard]] std::vector<size_t> allowedDrmDeviceIndices(
      std::span<const DrmDeviceIdentity> devices, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses
  );

  // Extracts the per-GPU device minor published by the proprietary NVIDIA
  // driver. Some versions expose no udev parent for /dev/nvidiaN.
  [[nodiscard]] std::optional<unsigned int> parseNvidiaDeviceMinor(std::string_view information);

  // True only for proprietary NVIDIA per-GPU character devices. Shared nodes
  // such as nvidiactl and nvidia-modeset do not identify one physical GPU.
  [[nodiscard]] bool isNvidiaGpuDeviceNode(std::string_view node);

  // Decide runtime removals, additions, and fail-closed termination without
  // performing any wlroots operations. The manager applies this plan only
  // after it has refreshed device identities and selector state.
  [[nodiscard]] DrmReconcilePlan planDrmReconciliation(
      std::span<const DrmActiveDevice> active, const std::optional<DrmDeviceIdentity>& renderer,
      std::span<const DrmDeviceIdentity> candidates, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses, bool selectorsValid
  );

} // namespace umbriel
