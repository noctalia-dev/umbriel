#include "server/drm_policy.h"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace umbriel {

  bool sameDrmPhysicalDevice(const DrmDeviceIdentity& first, const DrmDeviceIdentity& second) {
    if (first.pciAddress && second.pciAddress) {
      return first.pciAddress == second.pciAddress;
    }
    if (!first.physicalDevice.empty() && !second.physicalDevice.empty()) {
      return first.physicalDevice == second.physicalDevice;
    }
    return first.device == second.device;
  }

  DrmBackendEnvironment resolveDrmBackendEnvironment(
      std::optional<std::string_view> wlrBackends, bool waylandDisplaySet, bool waylandSocketSet, bool x11DisplaySet
  ) {
    if (!wlrBackends) {
      return {
          .native = !waylandDisplaySet && !waylandSocketSet && !x11DisplaySet,
          .exclusionsSupported = true,
          .createLibinput = true,
          .libinputBeforeDrm = true,
          .allowMissingLibinput = true,
      };
    }

    bool drmSeen = false;
    bool libinputSeen = false;
    bool libinputBeforeDrm = false;
    bool supported = true;
    for (const auto token : *wlrBackends | std::views::split(',')) {
      const std::string_view name(token.begin(), token.end());
      if (name.empty()) {
        continue;
      }
      if (name == "drm" && !drmSeen) {
        drmSeen = true;
      } else if (name == "libinput" && !libinputSeen) {
        libinputSeen = true;
        libinputBeforeDrm = !drmSeen;
      } else {
        supported = false;
      }
    }
    return {
        .native = drmSeen,
        .exclusionsSupported = drmSeen && supported,
        .createLibinput = libinputSeen,
        .libinputBeforeDrm = libinputBeforeDrm,
        .allowMissingLibinput = false,
    };
  }

  DrmExclusionMatch drmExclusionMatch(
      const DrmDeviceIdentity& device, std::span<const DrmDeviceIdentity> ignoredDevices,
      std::span<const std::string> pciAddresses
  ) {
    if (device.pciAddress && std::ranges::find(pciAddresses, *device.pciAddress) != pciAddresses.end()) {
      return DrmExclusionMatch::PciAddress;
    }

    for (const DrmDeviceIdentity& ignored : ignoredDevices) {
      if (sameDrmPhysicalDevice(device, ignored)) {
        if (device.pciAddress && ignored.pciAddress) {
          return DrmExclusionMatch::PciAddress;
        }
        return !device.physicalDevice.empty() && !ignored.physicalDevice.empty() ? DrmExclusionMatch::PhysicalDevice
                                                                                 : DrmExclusionMatch::DeviceNumber;
      }
    }
    return DrmExclusionMatch::None;
  }

  std::optional<unsigned int> parseNvidiaDeviceMinor(std::string_view line) {
    constexpr std::string_view prefix = "Device Minor:";
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    line.remove_prefix(first);
    if (!line.starts_with(prefix)) {
      return std::nullopt;
    }
    line.remove_prefix(prefix.size());
    const size_t numberStart = line.find_first_not_of(" \t");
    if (numberStart == std::string_view::npos) {
      return std::nullopt;
    }
    line.remove_prefix(numberStart);
    line = line.substr(0, line.find_last_not_of(" \t\r") + 1);

    unsigned int minorNumber = 0;
    const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), minorNumber);
    if (error != std::errc{} || end != line.data() + line.size()) {
      return std::nullopt;
    }
    return minorNumber;
  }

  bool isNvidiaGpuDeviceNode(std::string_view node) {
    constexpr std::string_view deletedSuffix = " (deleted)";
    if (node.ends_with(deletedSuffix)) {
      node.remove_suffix(deletedSuffix.size());
    }
    const size_t separator = node.find_last_of('/');
    const std::string_view name = node.substr(separator == std::string_view::npos ? 0 : separator + 1);
    constexpr std::string_view prefix = "nvidia";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
      return false;
    }
    return std::ranges::all_of(name.substr(prefix.size()), [](char character) {
      return character >= '0' && character <= '9';
    });
  }

} // namespace umbriel
