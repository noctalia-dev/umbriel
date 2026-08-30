#include "server/drm_policy.h"

#include <algorithm>
#include <charconv>

namespace umbriel {

  namespace {
    std::vector<std::string_view> backendNames(std::string_view configured) {
      std::vector<std::string_view> names;
      size_t offset = 0;
      while (offset < configured.size()) {
        offset = configured.find_first_not_of(',', offset);
        if (offset == std::string_view::npos) {
          break;
        }
        const size_t separator = configured.find(',', offset);
        names.push_back(configured.substr(offset, separator - offset));
        if (separator == std::string_view::npos) {
          break;
        }
        offset = separator + 1;
      }
      return names;
    }
  } // namespace

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

    const auto names = backendNames(*wlrBackends);
    const bool native = std::ranges::find(names, "drm") != names.end();
    bool drmSeen = false;
    bool libinputSeen = false;
    bool libinputBeforeDrm = false;
    bool supported = native;
    for (const std::string_view name : names) {
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
        .native = native,
        .exclusionsSupported = supported,
        .createLibinput = libinputSeen,
        .libinputBeforeDrm = libinputBeforeDrm,
        .allowMissingLibinput = false,
    };
  }

  DrmExclusionMatch drmExclusionMatch(
      const DrmDeviceIdentity& device, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses
  ) {
    if (device.pciAddress && std::ranges::find(pciAddresses, *device.pciAddress) != pciAddresses.end()) {
      return DrmExclusionMatch::PciAddress;
    }

    for (const DrmPathSelectorIdentity& selector : paths) {
      if (device.node == selector.path || std::ranges::find(device.devlinks, selector.path) != device.devlinks.end()) {
        return DrmExclusionMatch::Path;
      }
      if (selector.device && device.device == *selector.device) {
        return DrmExclusionMatch::DeviceNumber;
      }
      if (!selector.physicalDevice.empty() && device.physicalDevice == selector.physicalDevice) {
        return DrmExclusionMatch::PhysicalDevice;
      }
      if (selector.pciAddress && device.pciAddress == selector.pciAddress) {
        return DrmExclusionMatch::PciAddress;
      }
    }
    return DrmExclusionMatch::None;
  }

  size_t
  learnDrmPathSelectorIdentities(std::span<DrmPathSelectorIdentity> paths, std::span<const DrmDeviceIdentity> devices) {
    size_t learned = 0;
    for (DrmPathSelectorIdentity& selector : paths) {
      if (selector.device) {
        continue;
      }
      const auto device = std::ranges::find_if(devices, [&](const DrmDeviceIdentity& candidate) {
        return candidate.node == selector.path
            || std::ranges::find(candidate.devlinks, selector.path) != candidate.devlinks.end();
      });
      if (device == devices.end()) {
        continue;
      }
      selector.device = device->device;
      selector.physicalDevice = device->physicalDevice;
      selector.pciAddress = device->pciAddress;
      ++learned;
    }
    return learned;
  }

  std::vector<size_t> allowedDrmDeviceIndices(
      std::span<const DrmDeviceIdentity> devices, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses
  ) {
    std::vector<size_t> allowed;
    allowed.reserve(devices.size());
    for (size_t index = 0; index < devices.size(); ++index) {
      if (drmExclusionMatch(devices[index], paths, pciAddresses) == DrmExclusionMatch::None) {
        allowed.push_back(index);
      }
    }
    return allowed;
  }

  std::optional<unsigned int> parseNvidiaDeviceMinor(std::string_view information) {
    constexpr std::string_view prefix = "Device Minor:";
    size_t offset = 0;
    while (offset < information.size()) {
      const size_t newline = information.find('\n', offset);
      std::string_view line = information.substr(offset, newline - offset);
      if (const size_t first = line.find_first_not_of(" \t"); first != std::string_view::npos) {
        line.remove_prefix(first);
      }
      if (line.starts_with(prefix)) {
        line.remove_prefix(prefix.size());
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos) {
          return std::nullopt;
        }
        line.remove_prefix(first);
        const size_t last = line.find_last_not_of(" \t\r");
        line = line.substr(0, last + 1);

        unsigned int minorNumber = 0;
        const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), minorNumber);
        if (error != std::errc{} || end != line.data() + line.size()) {
          return std::nullopt;
        }
        return minorNumber;
      }
      if (newline == std::string_view::npos) {
        break;
      }
      offset = newline + 1;
    }
    return std::nullopt;
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

  DrmReconcilePlan planDrmReconciliation(
      std::span<const DrmActiveDevice> active, const std::optional<DrmDeviceIdentity>& renderer,
      std::span<const DrmDeviceIdentity> candidates, std::span<const DrmPathSelectorIdentity> paths,
      std::span<const std::string> pciAddresses, bool selectorsValid
  ) {
    DrmReconcilePlan plan;
    if (renderer && drmExclusionMatch(*renderer, paths, pciAddresses) != DrmExclusionMatch::None) {
      plan.termination = DrmReconcileTermination::RendererExcluded;
      return plan;
    }

    for (size_t index = 0; index < active.size(); ++index) {
      const auto current = std::ranges::find_if(candidates, [&](const DrmDeviceIdentity& candidate) {
        return sameDrmPhysicalDevice(active[index].identity, candidate);
      });
      const DrmDeviceIdentity& identity = current == candidates.end() ? active[index].identity : *current;
      if (drmExclusionMatch(identity, paths, pciAddresses) == DrmExclusionMatch::None) {
        continue;
      }
      if (active[index].primary) {
        plan.termination = DrmReconcileTermination::PrimaryExcluded;
        plan.removeActiveIndices.clear();
        return plan;
      }
      plan.removeActiveIndices.push_back(index);
    }

    if (!selectorsValid) {
      return plan;
    }
    for (size_t index = 0; index < candidates.size(); ++index) {
      if (drmExclusionMatch(candidates[index], paths, pciAddresses) != DrmExclusionMatch::None) {
        continue;
      }
      const bool alreadyActive = std::ranges::any_of(active, [&](const DrmActiveDevice& device) {
        return sameDrmPhysicalDevice(device.identity, candidates[index]);
      });
      if (!alreadyActive) {
        plan.addCandidateIndices.push_back(index);
      }
    }
    return plan;
  }

} // namespace umbriel
