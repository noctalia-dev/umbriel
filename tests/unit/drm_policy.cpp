#include "server/drm_policy.h"

#include "check.h"

#include <sys/sysmacros.h>

using umbriel::allowedDrmDeviceIndices;
using umbriel::DrmDeviceIdentity;
using umbriel::DrmExclusionMatch;
using umbriel::drmExclusionMatch;
using umbriel::DrmPathSelectorIdentity;
using umbriel::isNvidiaGpuDeviceNode;
using umbriel::learnDrmPathSelectorIdentities;
using umbriel::parseNvidiaDeviceMinor;
using umbriel::planDrmReconciliation;
using umbriel::resolveDrmBackendEnvironment;
using umbriel::sameDrmPhysicalDevice;

namespace {
  const DrmDeviceIdentity kIntegrated{
      .node = "/dev/dri/card0",
      .device = makedev(226, 0),
      .physicalDevice = "/sys/devices/pci0000:00/0000:00:02.0",
      .pciAddress = "0000:00:02.0",
      .devlinks = {"/dev/dri/by-path/pci-0000:00:02.0-card"},
      .seat = "seat0",
  };
  const DrmDeviceIdentity kDiscrete{
      .node = "/dev/dri/card1",
      .device = makedev(226, 1),
      .physicalDevice = "/sys/devices/pci0000:00/0000:01:00.0",
      .pciAddress = "0000:01:00.0",
      .devlinks = {"/dev/dri/by-path/pci-0000:01:00.0-card"},
      .seat = "seat0",
  };
} // namespace

UMBRIEL_TEST(pathAliasExcludesCardBeforeSelection) {
  const std::vector<DrmPathSelectorIdentity> paths{{
      .path = "/dev/dri/by-path/pci-0000:01:00.0-card",
      .device = std::nullopt,
      .physicalDevice = {},
      .pciAddress = std::nullopt,
  }};
  const std::vector<DrmDeviceIdentity> devices{kIntegrated, kDiscrete};

  CHECK(drmExclusionMatch(kDiscrete, paths, {}) == DrmExclusionMatch::Path);
  CHECK_EQ(allowedDrmDeviceIndices(devices, paths, {}), std::vector<size_t>{0});
}

UMBRIEL_TEST(renderNodeIdentityExcludesTheWholePhysicalGpu) {
  const std::vector<DrmPathSelectorIdentity> paths{{
      .path = "/dev/dri/renderD129",
      .device = makedev(226, 129),
      .physicalDevice = kDiscrete.physicalDevice,
      .pciAddress = kDiscrete.pciAddress,
  }};

  CHECK(drmExclusionMatch(kDiscrete, paths, {}) == DrmExclusionMatch::PhysicalDevice);
  CHECK(drmExclusionMatch(kIntegrated, paths, {}) == DrmExclusionMatch::None);
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
}

UMBRIEL_TEST(missingPathRetainsItsLastPhysicalIdentity) {
  const std::vector<DrmPathSelectorIdentity> paths{{
      .path = "/dev/dri/by-path/pci-0000:01:00.0-render",
      .device = std::nullopt,
      .physicalDevice = kDiscrete.physicalDevice,
      .pciAddress = kDiscrete.pciAddress,
  }};

  CHECK(drmExclusionMatch(kDiscrete, paths, {}) == DrmExclusionMatch::PhysicalDevice);
}

UMBRIEL_TEST(missingPathLearnsPhysicalIdentityFromAdvertisedUdevLink) {
  std::vector<DrmPathSelectorIdentity> paths{{
      .path = "/dev/dri/by-path/pci-0000:01:00.0-card",
      .device = std::nullopt,
      .physicalDevice = {},
      .pciAddress = std::nullopt,
  }};
  DrmDeviceIdentity renderNode = kDiscrete;
  renderNode.node = "/dev/dri/renderD129";
  renderNode.device = makedev(226, 129);
  renderNode.devlinks = {"/dev/dri/by-path/pci-0000:01:00.0-render"};

  CHECK_EQ(learnDrmPathSelectorIdentities(paths, std::span{&kDiscrete, size_t{1}}), size_t{1});
  CHECK_EQ(paths.front().physicalDevice, kDiscrete.physicalDevice);
  CHECK(drmExclusionMatch(renderNode, paths, {}) == DrmExclusionMatch::PhysicalDevice);
}

UMBRIEL_TEST(pciAddressExcludesAReenumeratedGpu) {
  const std::vector<std::string> pciAddresses{"0000:01:00.0"};
  DrmDeviceIdentity reenumerated = kDiscrete;
  reenumerated.node = "/dev/dri/card7";
  reenumerated.device = makedev(226, 7);
  reenumerated.devlinks.clear();

  CHECK(drmExclusionMatch(reenumerated, {}, pciAddresses) == DrmExclusionMatch::PciAddress);
}

UMBRIEL_TEST(allExcludedProducesNoBackendCandidates) {
  const std::vector<DrmDeviceIdentity> devices{kIntegrated, kDiscrete};
  const std::vector<std::string> pciAddresses{"0000:00:02.0", "0000:01:00.0"};

  CHECK(allowedDrmDeviceIndices(devices, {}, pciAddresses).empty());
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

UMBRIEL_TEST(nvidiaDeviceMinorParsesDriverInformation) {
  constexpr std::string_view information = R"(Model: NVIDIA GeForce RTX 5090
Bus Location: 0000:01:00.0
Device Minor: 	 0
)";

  CHECK_EQ(parseNvidiaDeviceMinor(information), std::optional<unsigned int>{0});
  CHECK_EQ(parseNvidiaDeviceMinor("  Device Minor: 17\r\n"), std::optional<unsigned int>{17});
  CHECK(!parseNvidiaDeviceMinor("Device Minor: none\n"));
  CHECK(!parseNvidiaDeviceMinor("Device Minor: 3 trailing\n"));
  CHECK(!parseNvidiaDeviceMinor("Model: no minor\n"));
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

UMBRIEL_TEST(reconciliationTerminatesWhenRendererBecomesExcluded) {
  DrmDeviceIdentity renderer = kDiscrete;
  renderer.node = "/dev/dri/renderD129";
  renderer.device = makedev(226, 129);
  const std::vector<umbriel::DrmActiveDevice> active{{.identity = kIntegrated, .primary = true}};
  const std::vector<DrmDeviceIdentity> candidates{kIntegrated, kDiscrete};
  const std::vector<std::string> excluded{"0000:01:00.0"};

  const auto plan = planDrmReconciliation(active, renderer, candidates, {}, excluded, true);

  CHECK(plan.termination == umbriel::DrmReconcileTermination::RendererExcluded);
  CHECK(plan.removeActiveIndices.empty());
  CHECK(plan.addCandidateIndices.empty());
}

UMBRIEL_TEST(reconciliationTerminatesWhenPrimaryBecomesExcluded) {
  const std::vector<umbriel::DrmActiveDevice> active{
      {.identity = kIntegrated, .primary = true},
      {.identity = kDiscrete, .primary = false},
  };
  const std::vector<DrmDeviceIdentity> candidates{kIntegrated, kDiscrete};
  const std::vector<std::string> excluded{"0000:00:02.0"};

  const auto plan = planDrmReconciliation(active, std::nullopt, candidates, {}, excluded, true);

  CHECK(plan.termination == umbriel::DrmReconcileTermination::PrimaryExcluded);
  CHECK(plan.removeActiveIndices.empty());
  CHECK(plan.addCandidateIndices.empty());
}

UMBRIEL_TEST(reconciliationRemovesExcludedSecondaryAndAddsAllowedGpu) {
  DrmDeviceIdentity replacement = kDiscrete;
  replacement.node = "/dev/dri/card2";
  replacement.device = makedev(226, 2);
  replacement.physicalDevice = "/sys/devices/pci0000:00/0000:02:00.0";
  replacement.pciAddress = "0000:02:00.0";
  const std::vector<umbriel::DrmActiveDevice> active{
      {.identity = kIntegrated, .primary = true},
      {.identity = kDiscrete, .primary = false},
  };
  const std::vector<DrmDeviceIdentity> candidates{kIntegrated, kDiscrete, replacement};
  const std::vector<std::string> excluded{"0000:01:00.0"};

  const auto plan = planDrmReconciliation(active, std::nullopt, candidates, {}, excluded, true);

  CHECK(plan.termination == umbriel::DrmReconcileTermination::None);
  CHECK_EQ(plan.removeActiveIndices, std::vector<size_t>{1});
  CHECK_EQ(plan.addCandidateIndices, std::vector<size_t>{2});
}

UMBRIEL_TEST(reconciliationSuppressesNewBackendsWhileSelectorsAreInvalid) {
  const std::vector<umbriel::DrmActiveDevice> active{{.identity = kIntegrated, .primary = true}};
  const std::vector<DrmDeviceIdentity> candidates{kIntegrated, kDiscrete};

  const auto plan = planDrmReconciliation(active, std::nullopt, candidates, {}, {}, false);

  CHECK(plan.termination == umbriel::DrmReconcileTermination::None);
  CHECK(plan.removeActiveIndices.empty());
  CHECK(plan.addCandidateIndices.empty());
}

int main() { return RUN_TESTS(); }
