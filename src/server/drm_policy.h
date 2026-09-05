#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace umbriel {

  // Identity shared by a GPU's card and render nodes. physicalDevice is the
  // parent sysfs device, so selecting either node applies to the whole GPU.
  struct DrmDeviceIdentity {
    std::string node;
    dev_t device = 0;
    std::string physicalDevice;
    std::optional<std::string> pciAddress;
  };

  enum class DrmExclusionMatch {
    None,
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
      const DrmDeviceIdentity& device, std::span<const DrmDeviceIdentity> ignoredDevices,
      std::span<const std::string> pciAddresses
  );

  // Parses one "Device Minor:" line from proprietary NVIDIA GPU metadata.
  [[nodiscard]] std::optional<unsigned int> parseNvidiaDeviceMinor(std::string_view line);

  // True only for proprietary NVIDIA per-GPU character devices. Shared nodes
  // such as nvidiactl and nvidia-modeset do not identify one physical GPU.
  [[nodiscard]] bool isNvidiaGpuDeviceNode(std::string_view node);

} // namespace umbriel
