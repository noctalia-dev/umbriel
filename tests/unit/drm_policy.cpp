#include "server/drm_policy.h"

#include "check.h"

#include <sys/sysmacros.h>
#include <vector>

using umbriel::DrmDeviceIdentity;
using umbriel::DrmExclusionMatch;
using umbriel::drmExclusionMatch;
using umbriel::isNvidiaGpuDeviceNode;
using umbriel::parseNvidiaDeviceMinor;
using umbriel::resolveDrmBackendEnvironment;
using umbriel::sameDrmPhysicalDevice;

namespace {
  const DrmDeviceIdentity kIntegrated{
      .node = "/dev/dri/card0",
      .device = makedev(226, 0),
      .physicalDevice = "/sys/devices/pci0000:00/0000:00:02.0",
      .pciAddress = "0000:00:02.0",
  };
  const DrmDeviceIdentity kDiscrete{
      .node = "/dev/dri/card1",
      .device = makedev(226, 1),
      .physicalDevice = "/sys/devices/pci0000:00/0000:01:00.0",
      .pciAddress = "0000:01:00.0",
  };
} // namespace

UMBRIEL_TEST(renderNodeIdentityExcludesTheWholePhysicalGpu) {
  const std::vector<DrmDeviceIdentity> ignored{{
      .node = "/dev/dri/renderD129",
      .device = makedev(226, 129),
      .physicalDevice = kDiscrete.physicalDevice,
      .pciAddress = kDiscrete.pciAddress,
  }};

  CHECK(drmExclusionMatch(kDiscrete, ignored, {}) == DrmExclusionMatch::PciAddress);
  CHECK(drmExclusionMatch(kIntegrated, ignored, {}) == DrmExclusionMatch::None);
}

UMBRIEL_TEST(cardAndRenderNodesCompareAsOnePhysicalGpu) {
  DrmDeviceIdentity renderNode = kDiscrete;
  renderNode.node = "/dev/dri/renderD129";
  renderNode.device = makedev(226, 129);

  CHECK(sameDrmPhysicalDevice(kDiscrete, renderNode));

  renderNode.pciAddress = "0000:02:00.0";
  CHECK(!sameDrmPhysicalDevice(kDiscrete, renderNode));

  DrmDeviceIdentity platformDevice = kDiscrete;
  platformDevice.pciAddress.reset();
  renderNode = platformDevice;
  renderNode.node = "/dev/dri/renderD129";
  renderNode.device = makedev(226, 129);
  CHECK(sameDrmPhysicalDevice(platformDevice, renderNode));
  CHECK(drmExclusionMatch(renderNode, std::span{&platformDevice, size_t{1}}, {}) == DrmExclusionMatch::PhysicalDevice);
}

UMBRIEL_TEST(resolvedIdentityDoesNotFollowAReusedDeviceNode) {
  const std::vector<DrmDeviceIdentity> ignored{kDiscrete};
  DrmDeviceIdentity replacement = kIntegrated;
  replacement.node = kDiscrete.node;
  replacement.device = kDiscrete.device;

  CHECK(drmExclusionMatch(replacement, ignored, {}) == DrmExclusionMatch::None);
}

UMBRIEL_TEST(resolvedIdentitySurvivesNodeRenumbering) {
  const std::vector<DrmDeviceIdentity> ignored{kDiscrete};
  DrmDeviceIdentity reattached = kDiscrete;
  reattached.node = "/dev/dri/card7";
  reattached.device = makedev(226, 7);
  CHECK(drmExclusionMatch(reattached, ignored, {}) == DrmExclusionMatch::PciAddress);
}

UMBRIEL_TEST(pciAddressKeepsAReattachedGpuExcluded) {
  const std::vector<std::string> pciAddresses{"0000:01:00.0"};
  DrmDeviceIdentity reenumerated = kDiscrete;
  reenumerated.node = "/dev/dri/card7";
  reenumerated.device = makedev(226, 7);

  CHECK(drmExclusionMatch(reenumerated, {}, pciAddresses) == DrmExclusionMatch::PciAddress);
}

UMBRIEL_TEST(backendEnvironmentMatchesWlrootsPrecedence) {
  CHECK_EQ(
      resolveDrmBackendEnvironment(std::nullopt, false, false, false),
      (umbriel::DrmBackendEnvironment{
          .native = true,
          .exclusionsSupported = true,
          .createLibinput = true,
          .libinputBeforeDrm = true,
          .allowMissingLibinput = true,
      })
  );
  CHECK(!resolveDrmBackendEnvironment(std::nullopt, true, false, false).native);
  CHECK(!resolveDrmBackendEnvironment(std::nullopt, false, true, false).native);
  CHECK(!resolveDrmBackendEnvironment(std::nullopt, false, false, true).native);
  CHECK(!resolveDrmBackendEnvironment(std::string_view{}, false, false, false).native);
}

UMBRIEL_TEST(filteredBackendSupportsOnlyOneDrmAndOptionalLibinput) {
  const auto drmOnly = resolveDrmBackendEnvironment("drm", true, true, true);
  CHECK(drmOnly.native);
  CHECK(drmOnly.exclusionsSupported);
  CHECK(!drmOnly.createLibinput);

  const auto native = resolveDrmBackendEnvironment(",drm,,libinput,", true, false, false);
  CHECK(native.native);
  CHECK(native.exclusionsSupported);
  CHECK(native.createLibinput);
  CHECK(!native.libinputBeforeDrm);

  const auto inputFirst = resolveDrmBackendEnvironment("libinput,drm", false, false, false);
  CHECK(inputFirst.exclusionsSupported);
  CHECK(inputFirst.libinputBeforeDrm);

  CHECK(!resolveDrmBackendEnvironment("drm,headless", false, false, false).exclusionsSupported);
  CHECK(!resolveDrmBackendEnvironment("drm,drm", false, false, false).exclusionsSupported);
  CHECK(!resolveDrmBackendEnvironment("drm, libinput", false, false, false).exclusionsSupported);
}

UMBRIEL_TEST(nvidiaDeviceMinorParsesDriverLine) {
  CHECK_EQ(parseNvidiaDeviceMinor("Device Minor: \t 0"), std::optional<unsigned int>{0});
  CHECK_EQ(parseNvidiaDeviceMinor("  Device Minor: 17\r"), std::optional<unsigned int>{17});
  CHECK(!parseNvidiaDeviceMinor("Device Minor: none"));
  CHECK(!parseNvidiaDeviceMinor("Device Minor: 3 trailing"));
  CHECK(!parseNvidiaDeviceMinor("Model: no minor"));
}

UMBRIEL_TEST(nvidiaGpuDeviceNodeExcludesOnlyPerGpuNodes) {
  CHECK(isNvidiaGpuDeviceNode("/dev/nvidia0"));
  CHECK(isNvidiaGpuDeviceNode("/dev/nvidia17"));
  CHECK(isNvidiaGpuDeviceNode("/dev/nvidia17 (deleted)"));
  CHECK(!isNvidiaGpuDeviceNode("/dev/nvidia"));
  CHECK(!isNvidiaGpuDeviceNode("/dev/nvidiactl"));
  CHECK(!isNvidiaGpuDeviceNode("/dev/nvidia-modeset"));
  CHECK(!isNvidiaGpuDeviceNode("/dev/nvidia0-extra"));
}

int main() { return RUN_TESTS(); }
