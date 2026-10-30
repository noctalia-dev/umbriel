#include "server/color_description.h"

#include "check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <utility>
#include <vector>

#if UMBRIEL_HAS_LCMS2
#include <lcms2.h>
#endif

namespace {

  wlr_output_image_description hdrOutputDescription() {
    return {
        .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        .mastering_display_primaries =
            {
                .red = {.x = 0.68F, .y = 0.32F},
                .green = {.x = 0.265F, .y = 0.69F},
                .blue = {.x = 0.15F, .y = 0.06F},
                .white = {.x = 0.3127F, .y = 0.329F},
            },
        .mastering_luminance = {.min = 0.01, .max = 1000.0},
        .max_cll = 1000.0,
        .max_fall = 400.0,
    };
  }

  bool closeTo(double actual, double expected, double tolerance = 0.002) {
    return std::abs(actual - expected) < tolerance;
  }

#if UMBRIEL_HAS_LCMS2
  std::vector<uint8_t> saveProfile(cmsHPROFILE profile) {
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(profile, nullptr, &size);
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(profile, bytes.data(), &size);
    bytes.resize(size);
    return bytes;
  }

  // Matrix-shaper RGB display profile, as the colorimetry unit builds them.
  std::vector<uint8_t> matrixShaperProfile(const std::array<cmsToneCurve*, 3>& curves) {
    const cmsCIExyY white = {.x = 0.3127, .y = 0.329, .Y = 1.0};
    const cmsCIExyYTRIPLE primaries{
        .Red = {.x = 0.64, .y = 0.33, .Y = 1.0},
        .Green = {.x = 0.30, .y = 0.60, .Y = 1.0},
        .Blue = {.x = 0.15, .y = 0.06, .Y = 1.0},
    };
    cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves.data());
    std::vector<uint8_t> bytes;
    if (profile != nullptr) {
      bytes = saveProfile(profile);
      cmsCloseProfile(profile);
    }
    return bytes;
  }

  cmsToneCurve* srgbDecodeCurve() {
    // lcms2 parametric type 4 = ICC parametricCurveType format 3 (the sRGB EOTF).
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    return cmsBuildParametricToneCurve(nullptr, 4, params);
  }

  cmsToneCurve* tabulatedGamma24Curve() {
    constexpr size_t size = 256;
    std::vector<uint16_t> values(size);
    for (size_t index = 0; index < size; ++index) {
      const double encoded = static_cast<double>(index) / (size - 1);
      values[index] = static_cast<uint16_t>(std::lround(std::pow(encoded, 2.4) * 65535.0));
    }
    return cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(size), values.data());
  }
#endif

} // namespace

UMBRIEL_TEST(hdrReferenceWhiteChangesIndependentlyOfDisplayMetadata) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription original = umbriel::outputColorDescription(&output, 203.0F);
  const umbriel::ColorDescription brighter = umbriel::outputColorDescription(&output, 500.0F);
  const umbriel::ColorDescription restored = umbriel::outputColorDescription(&output, 203.0F);

  CHECK_EQ(original.luminances.reference, 203.0F);
  CHECK_EQ(brighter.luminances.reference, 500.0F);
  CHECK(!umbriel::colorDescriptionsEqual(original, brighter));
  CHECK(umbriel::colorDescriptionsEqual(original, restored));

  for (const auto* description : {&original, &brighter, &restored}) {
    CHECK_EQ(description->data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    CHECK_EQ(description->data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    CHECK_EQ(description->luminances.min, 0.005F);
    CHECK_EQ(description->luminances.max, 10000.0F);
    CHECK(description->data.has_mastering_display_primaries);
    CHECK_EQ(description->data.mastering_display_primaries.red.x, 0.68F);
    CHECK_EQ(description->data.mastering_display_primaries.green.y, 0.69F);
    CHECK_EQ(description->data.mastering_display_primaries.blue.x, 0.15F);
    CHECK_EQ(description->data.mastering_display_primaries.white.y, 0.329F);
    CHECK(description->data.has_mastering_luminance);
    CHECK_EQ(description->data.mastering_luminance.min, 0.01F);
    CHECK_EQ(description->data.mastering_luminance.max, 1000.0F);
    CHECK_EQ(description->data.max_cll, 1000U);
    CHECK_EQ(description->data.max_fall, 400U);
    CHECK_EQ(description->luminanceMultiplier, 1.0F);
    CHECK(description->customLuminances);
    CHECK(description->requiresHdrOutput);
  }
}

UMBRIEL_TEST(sdrFeedbackKeepsNominalWhiteAcrossConfiguredWhiteChanges) {
  const umbriel::ColorDescription original = umbriel::outputColorDescription(nullptr, 203.0F);
  const umbriel::ColorDescription changed = umbriel::outputColorDescription(nullptr, 500.0F);

  CHECK(umbriel::colorDescriptionsEqual(original, changed));
  CHECK_EQ(changed.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
  CHECK_EQ(changed.data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
  CHECK_EQ(changed.luminances.min, 0.2F);
  CHECK_EQ(changed.luminances.max, 80.0F);
  CHECK_EQ(changed.luminances.reference, 80.0F);
  CHECK(!changed.data.has_mastering_luminance);
  CHECK(!changed.data.has_mastering_display_primaries);
  CHECK_EQ(changed.data.max_cll, 0U);
  CHECK_EQ(changed.data.max_fall, 0U);
}

UMBRIEL_TEST(automaticHdrFeedbackKeepsClientHeadroomAcrossScanoutTransitions) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription disabled = umbriel::outputColorDescription(nullptr, 203.0F);
  const umbriel::ColorDescription idle = umbriel::outputColorDescription(nullptr, 203.0F, true);
  const umbriel::ColorDescription active = umbriel::outputColorDescription(&output, 203.0F, true);
  const umbriel::ColorDescription returnedIdle = umbriel::outputColorDescription(nullptr, 203.0F, true);
  const umbriel::ColorDescription unavailable = umbriel::outputColorDescription(nullptr, 203.0F, false);

  // Wine detects HDR from primary luminance headroom before it submits HDR content.
  CHECK(!(disabled.luminances.max > disabled.luminances.reference));
  CHECK(idle.luminances.max > idle.luminances.reference);
  CHECK(active.luminances.max > active.luminances.reference);
  CHECK(returnedIdle.luminances.max > returnedIdle.luminances.reference);
  CHECK(!(unavailable.luminances.max > unavailable.luminances.reference));
  CHECK(!umbriel::colorDescriptionsEqual(disabled, idle));
  CHECK(!umbriel::colorDescriptionsEqual(idle, active));
  CHECK(umbriel::colorDescriptionsEqual(idle, returnedIdle));
  CHECK(umbriel::colorDescriptionsEqual(disabled, unavailable));

  // Reusing the logical HDR encoding without an SDR content limit remains eligible for automatic HDR.
  CHECK_EQ(idle.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  CHECK_EQ(idle.data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  CHECK_EQ(idle.luminances.max, 10000.0F);
  CHECK(idle.requiresHdrOutput);
  CHECK(active.requiresHdrOutput);
  CHECK(returnedIdle.requiresHdrOutput);
  CHECK(!unavailable.requiresHdrOutput);

  // Target metadata continues to describe the current SDR or HDR destination.
  CHECK(idle.data.has_mastering_display_primaries);
  CHECK_EQ(idle.data.mastering_display_primaries.red.x, 0.64F);
  CHECK(idle.data.has_mastering_luminance);
  CHECK_EQ(idle.data.mastering_luminance.min, 0.2F);
  CHECK_EQ(idle.data.mastering_luminance.max, 80.0F);
  CHECK_EQ(idle.data.max_cll, 0U);
  CHECK_EQ(idle.data.max_fall, 0U);
  CHECK_EQ(active.data.mastering_luminance.max, 1000.0F);
  CHECK_EQ(active.data.max_cll, 1000U);
}

UMBRIEL_TEST(automaticHdrWhiteChangeUpdatesEncodingWithoutChangingSdrTarget) {
  const umbriel::ColorDescription original = umbriel::outputColorDescription(nullptr, 203.0F, true);
  const umbriel::ColorDescription brighter = umbriel::outputColorDescription(nullptr, 500.0F, true);
  const umbriel::ColorDescription restored = umbriel::outputColorDescription(nullptr, 203.0F, true);

  CHECK_EQ(original.luminances.reference, 203.0F);
  CHECK_EQ(brighter.luminances.reference, 500.0F);
  CHECK(original.customLuminances);
  CHECK(brighter.customLuminances);
  CHECK(brighter.luminances.max > brighter.luminances.reference);
  CHECK(!umbriel::colorDescriptionsEqual(original, brighter));
  CHECK(umbriel::colorDescriptionsEqual(original, restored));
  CHECK_EQ(original.data.mastering_luminance.max, 80.0F);
  CHECK_EQ(brighter.data.mastering_luminance.max, 80.0F);
  CHECK_EQ(brighter.data.mastering_display_primaries.red.x, original.data.mastering_display_primaries.red.x);
}

UMBRIEL_TEST(sdrContentInsidePqEncodingDoesNotRequestHdrOutput) {
  umbriel::ColorDescription description = umbriel::outputColorDescription(nullptr, 203.0F, true);
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));

  // SDR desktop renderers can use the offered PQ encoding while explicitly
  // limiting the content to SDR white. The encoding still decodes as PQ.
  description.data.max_cll = 203;
  description.data.max_fall = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  CHECK_EQ(description.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  CHECK_EQ(description.luminances.max, 10000.0F);
  CHECK(description.requiresHdrOutput);

  description.data.max_cll = 1000;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(contentPeakUsesItsOwnReferenceWhiteRatherThanAConstant) {
  umbriel::ColorDescription description = umbriel::outputColorDescription(nullptr, 350.0F, true);
  description.data.max_cll = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 350;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 351;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 349;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(unknownContentPeakAndLowFrameAveragePreserveHdrRequests) {
  umbriel::ColorDescription description = umbriel::outputColorDescription(nullptr, 203.0F, true);
  description.data.max_cll = 1000;
  description.data.max_fall = 203;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 0;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_fall = 0;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(explicitMasteringHeadroomPreservesHdrWhenContentPeakDefaultsToSdrWhite) {
  umbriel::ColorDescription description = umbriel::outputColorDescription(nullptr, 203.0F, true);
  description.data.max_cll = 203;
  description.data.max_fall = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.has_mastering_luminance = true;
  description.data.mastering_luminance = {.min = 0.0F, .max = 1000.0F};
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.mastering_luminance.max = 203.0F;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.mastering_luminance.max = 100.0F;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.mastering_luminance.max = 1000.0F;
  description.data.has_mastering_luminance = false;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(contentLightMetadataDoesNotInventHdrForSdrEncoding) {
  umbriel::ColorDescription description = umbriel::outputColorDescription(nullptr, 203.0F);
  description.data.max_cll = 1000;
  description.data.max_fall = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 0;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(absoluteScrgbContentPeakKeepsItsReferenceSeparateFromUnitLuminance) {
  umbriel::ColorDescription description;
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
  description.data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
  description.luminances = {.min = 0.0F, .max = 10000.0F, .reference = 203.0F};
  description.luminanceMultiplier = 80.0F / 203.0F;
  description.absoluteLuminance = true;
  description.requiresHdrOutput = true;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 203;
  CHECK(!umbriel::colorDescriptionRequestsHdrOutput(description));
  description.data.max_cll = 1000;
  description.data.max_fall = 203;
  CHECK(umbriel::colorDescriptionRequestsHdrOutput(description));
}

UMBRIEL_TEST(hdrWithoutDisplayMetadataStillUsesConfiguredWhite) {
  const wlr_output_image_description output{
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
      .mastering_display_primaries = {},
      .mastering_luminance = {},
      .max_cll = 0.0,
      .max_fall = 0.0,
  };
  const umbriel::ColorDescription description = umbriel::outputColorDescription(&output, 350.0F);

  CHECK_EQ(description.luminances.reference, 350.0F);
  CHECK_EQ(description.luminances.max, 10000.0F);
  CHECK(!description.data.has_mastering_luminance);
  CHECK(!description.data.has_mastering_display_primaries);
  CHECK_EQ(description.data.max_cll, 0U);
  CHECK_EQ(description.data.max_fall, 0U);
}

UMBRIEL_TEST(hdrToSdrDropsHdrTargetAndRestoresNominalWhite) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription hdr = umbriel::outputColorDescription(&output, 500.0F);
  const umbriel::ColorDescription sdr = umbriel::outputColorDescription(nullptr, 500.0F);

  CHECK(!umbriel::colorDescriptionsEqual(hdr, sdr));
  CHECK_EQ(sdr.luminances.reference, 80.0F);
  CHECK(!sdr.data.has_mastering_display_primaries);
  CHECK(!sdr.data.has_mastering_luminance);
  CHECK_EQ(sdr.data.max_cll, 0U);
  CHECK_EQ(sdr.data.max_fall, 0U);
}

UMBRIEL_TEST(nonPqOutputKeepsTransferFunctionLuminanceDefaults) {
  const wlr_output_image_description output{
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_BT1886,
      .mastering_display_primaries = {},
      .mastering_luminance = {},
      .max_cll = 0.0,
      .max_fall = 0.0,
  };
  const umbriel::ColorDescription description = umbriel::outputColorDescription(&output, 500.0F);

  CHECK_EQ(description.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886);
  CHECK_EQ(description.luminances.min, 0.01F);
  CHECK_EQ(description.luminances.max, 100.0F);
  CHECK_EQ(description.luminances.reference, 100.0F);
}

UMBRIEL_TEST(preferredDescriptionComparisonIncludesReferenceWhiteAndTargetMetadata) {
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription original = umbriel::outputColorDescription(&output, 203.0F);
  std::array<umbriel::ColorDescription, 7> changed;
  changed.fill(original);
  changed[0].luminances.reference = 500.0F;
  changed[1].data.mastering_luminance.max = 800.0F;
  changed[2].data.mastering_display_primaries.red.x = 0.64F;
  changed[3].data.max_cll = 800;
  changed[4].data.max_fall = 300;
  changed[5].luminanceMultiplier = 80.0F / 203.0F;
  changed[6].requiresHdrOutput = false;

  for (const auto& description : changed) {
    CHECK(!umbriel::colorDescriptionsEqual(original, description));
  }
}

UMBRIEL_TEST(characterizedPanelReportsCustomPrimariesAndPolicyTransfer) {
  const wlr_color_primaries panel{
      .red = {.x = 0.68F, .y = 0.32F},
      .green = {.x = 0.265F, .y = 0.69F},
      .blue = {.x = 0.15F, .y = 0.06F},
      .white = {.x = 0.3127F, .y = 0.329F},
  };
  const umbriel::ColorDescription base = umbriel::outputColorDescription(nullptr, 203.0F);

  for (const uint32_t transfer : {
           static_cast<uint32_t>(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB),
           static_cast<uint32_t>(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22),
       }) {
    const umbriel::ColorDescription characterized =
        umbriel::characterizedOutputDescription(base, &panel, transfer, nullptr);
    CHECK_EQ(characterized.data.primaries_named, 0U);
    CHECK_EQ(characterized.customPrimaries.red.x, 0.68F);
    CHECK_EQ(characterized.customPrimaries.green.y, 0.69F);
    CHECK_EQ(characterized.customPrimaries.blue.x, 0.15F);
    CHECK_EQ(characterized.customPrimaries.white.y, 0.329F);
    // The named transfer keeps coming from the sdr_transfer policy, not the panel TRC.
    CHECK_EQ(characterized.data.tf_named, transfer);
    // A characterization change must be visible to the description refresh comparison.
    CHECK(!umbriel::colorDescriptionsEqual(base, characterized));
    // Untouched HDR metadata and luminance model.
    CHECK(!characterized.data.has_mastering_display_primaries);
    CHECK_EQ(characterized.luminances.reference, base.luminances.reference);
  }
}

UMBRIEL_TEST(inertCharacterizationKeepsNamedPrimaries) {
  const umbriel::ColorDescription base = umbriel::outputColorDescription(nullptr, 203.0F);
  const umbriel::ColorDescription inert =
      umbriel::characterizedOutputDescription(base, nullptr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, nullptr);

  CHECK(umbriel::colorDescriptionsEqual(base, inert));
  CHECK_EQ(inert.data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
  CHECK_EQ(inert.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
}

UMBRIEL_TEST(hdrDescriptionsKeepTheirEncodingUnderCharacterization) {
  const wlr_color_primaries panel{
      .red = {.x = 0.68F, .y = 0.32F},
      .green = {.x = 0.265F, .y = 0.69F},
      .blue = {.x = 0.15F, .y = 0.06F},
      .white = {.x = 0.3127F, .y = 0.329F},
  };
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription hdr = umbriel::outputColorDescription(&output, 203.0F);
  const umbriel::ColorDescription automatic = umbriel::outputColorDescription(nullptr, 203.0F, true);
  const uint32_t transfer = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;

  const umbriel::ColorDescription characterizedHdr =
      umbriel::characterizedOutputDescription(hdr, &panel, transfer, nullptr);
  const umbriel::ColorDescription characterizedAutomatic =
      umbriel::characterizedOutputDescription(automatic, &panel, transfer, nullptr);

  CHECK(umbriel::colorDescriptionsEqual(hdr, characterizedHdr));
  CHECK(umbriel::colorDescriptionsEqual(automatic, characterizedAutomatic));
  CHECK_EQ(characterizedHdr.data.primaries_named, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  CHECK_EQ(characterizedHdr.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  CHECK_EQ(characterizedAutomatic.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
}

UMBRIEL_TEST(characterizedDescriptionCarriesOnlyItsMatchingIccProfile) {
  const wlr_color_primaries panel{
      .red = {.x = 0.68F, .y = 0.32F},
      .green = {.x = 0.265F, .y = 0.69F},
      .blue = {.x = 0.15F, .y = 0.06F},
      .white = {.x = 0.3127F, .y = 0.329F},
  };
  const umbriel::ColorDescription base = umbriel::outputColorDescription(nullptr, 203.0F);
  const std::array<uint8_t, 4> profileBytes{0x0A, 0x0B, 0x0C, 0x0D};
  const umbriel::IccProfileStore store = umbriel::iccStoreForFileProfile(profileBytes);
  CHECK(store != nullptr);
  if (store == nullptr) {
    return;
  }

  // The characterized SDR description carries the store get_information
  // advertises via its icc_file event.
  const umbriel::ColorDescription characterized =
      umbriel::characterizedOutputDescription(base, &panel, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, store);
  CHECK(characterized.iccProfile == store);

  // Inert descriptions carry no profile even when a store is passed.
  const umbriel::ColorDescription inert =
      umbriel::characterizedOutputDescription(base, nullptr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, store);
  CHECK(inert.iccProfile == nullptr);

  // HDR/PQ descriptions never advertise the panel's SDR profile: it does not
  // match their encoding ("ICC profile matching the image description").
  const wlr_output_image_description output = hdrOutputDescription();
  const umbriel::ColorDescription hdr = umbriel::outputColorDescription(&output, 203.0F);
  const umbriel::ColorDescription characterizedHdr =
      umbriel::characterizedOutputDescription(hdr, &panel, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, store);
  CHECK(characterizedHdr.iccProfile == nullptr);

  // A profile swap with identical colorimetry is still a description change.
  const std::array<uint8_t, 4> swappedBytes{0x0A, 0x0B, 0x0C, 0x0E};
  const umbriel::IccProfileStore swapped = umbriel::iccStoreForFileProfile(swappedBytes);
  const umbriel::ColorDescription reswapped =
      umbriel::characterizedOutputDescription(base, &panel, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, swapped);
  CHECK(!umbriel::colorDescriptionsEqual(characterized, reswapped));
}

UMBRIEL_TEST(clientIccDescriptionsFitColorimetryAndKeepExactBytes) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Exact named fast path: a pure gamma 2.2 TRC becomes the named gamma 2.2
  // transfer function, with the colorants as custom primaries.
  {
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 2.2);
    CHECK(curve != nullptr);
    const std::vector<uint8_t> bytes = matrixShaperProfile({curve, curve, curve});
    cmsFreeToneCurve(curve);
    CHECK(!bytes.empty());
    const umbriel::ClientIccDescription result = umbriel::clientIccDescription(bytes);
    CHECK(result.description.has_value());
    if (!result.description) {
      return;
    }
    const umbriel::ColorDescription& description = *result.description;
    CHECK_EQ(description.data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    CHECK_EQ(description.tfPower, 0.0F);
    CHECK_EQ(description.data.primaries_named, 0U);
    CHECK(closeTo(description.customPrimaries.red.x, 0.64));
    CHECK(closeTo(description.customPrimaries.green.y, 0.60));
    CHECK(closeTo(description.customPrimaries.white.x, 0.3127, 0.005));
    // The client's exact bytes stay on the description for identity and
    // change detection; the create contract denies get_information, so no
    // icc_file is ever sent for this description.
    CHECK(description.iccProfile != nullptr);
    if (description.iccProfile != nullptr) {
      CHECK(*description.iccProfile == bytes);
    }
    CHECK(!description.requiresHdrOutput);
  }

  // Exact named fast path: the sRGB TRC becomes the named sRGB transfer.
  {
    cmsToneCurve* curve = srgbDecodeCurve();
    CHECK(curve != nullptr);
    const std::vector<uint8_t> bytes = matrixShaperProfile({curve, curve, curve});
    cmsFreeToneCurve(curve);
    const umbriel::ClientIccDescription result = umbriel::clientIccDescription(bytes);
    CHECK(result.description.has_value());
    if (result.description) {
      CHECK_EQ(result.description->data.tf_named, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
      CHECK_EQ(result.description->tfPower, 0.0F);
    }
  }

  // A non-representable TRC is approximated by a power fit (gamma 2.4 here),
  // reported in the set_tf_power/tf_power vocabulary rather than a named TF.
  {
    cmsToneCurve* curve = tabulatedGamma24Curve();
    CHECK(curve != nullptr);
    const std::vector<uint8_t> bytes = matrixShaperProfile({curve, curve, curve});
    cmsFreeToneCurve(curve);
    const umbriel::ClientIccDescription result = umbriel::clientIccDescription(bytes);
    CHECK(result.description.has_value());
    if (result.description) {
      CHECK_EQ(result.description->data.tf_named, 0U);
      CHECK(closeTo(result.description->tfPower, 2.4, 0.02));
    }
  }
#endif
}

UMBRIEL_TEST(clientIccValidationRejectsWithoutProtocolErrors) {
  // Everything invalid takes the wp_image_description_v1.failed path
  // ('unsupported'), never a hard protocol error: the caller turns these
  // errors into failed events.
  const umbriel::ClientIccDescription empty = umbriel::clientIccDescription({});
  CHECK(!empty.description.has_value());
  CHECK(!empty.error.empty());

  // set_icc_file's 32 MB limit stays part of the validation vocabulary even
  // though the request rejects oversize lengths with bad_size first.
  const std::vector<uint8_t> oversized(umbriel::kClientIccSizeLimit + 1, 0x42);
  const umbriel::ClientIccDescription tooBig = umbriel::clientIccDescription(oversized);
  CHECK(!tooBig.description.has_value());
  CHECK(tooBig.error.contains("32 MB"));

  const std::vector<uint8_t> shortProfile(100, 0x00);
  const umbriel::ClientIccDescription truncated = umbriel::clientIccDescription(shortProfile);
  CHECK(!truncated.description.has_value());
  CHECK(truncated.error.contains("truncated"));

  // Unknown profile versions fail before any parse: the major version is the
  // ICC header's byte 8 and the protocol admits exactly 2 and 4.
  std::vector<uint8_t> header(200, 0x00);
  for (const uint8_t version : {1, 5}) {
    header[8] = version;
    const umbriel::ClientIccDescription wrongVersion = umbriel::clientIccDescription(header);
    CHECK(!wrongVersion.description.has_value());
    CHECK(wrongVersion.error.contains("version"));
  }

  // Structurally unreadable data is rejected in every build shape.
  header[8] = 4;
  const umbriel::ClientIccDescription garbage = umbriel::clientIccDescription(header);
  CHECK(!garbage.description.has_value());
  CHECK(!garbage.error.empty());

#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // A valid v2 matrix-shaper profile of the Display class is the accepted baseline.
  cmsToneCurve* curve = cmsBuildGamma(nullptr, 2.2);
  CHECK(curve != nullptr);
  std::vector<uint8_t> bytes = matrixShaperProfile({curve, curve, curve});
  cmsFreeToneCurve(curve);
  CHECK(!bytes.empty());
  CHECK(umbriel::clientIccDescription(bytes).description.has_value());

  // The client path admits the ICC ColorSpace class per set_icc_file.
  std::vector<uint8_t> colorSpace = bytes;
  colorSpace[12] = 's';
  colorSpace[13] = 'p';
  colorSpace[14] = 'a';
  colorSpace[15] = 'c';
  CHECK(umbriel::clientIccDescription(colorSpace).description.has_value());

  // Wrong class fails the same graceful path.
  std::vector<uint8_t> printer = bytes;
  printer[12] = 'p';
  printer[13] = 'r';
  printer[14] = 't';
  printer[15] = 'r';
  const umbriel::ClientIccDescription wrongClass = umbriel::clientIccDescription(printer);
  CHECK(!wrongClass.description.has_value());
  CHECK(!wrongClass.error.empty());

  // A client LUT profile fails per the matrix-shaper parser rules.
  std::vector<uint8_t> lutProfile = bytes;
  {
    cmsHPROFILE profile = cmsOpenProfileFromMem(lutProfile.data(), static_cast<cmsUInt32Number>(lutProfile.size()));
    CHECK(profile != nullptr);
    if (profile != nullptr) {
      const std::array<uint8_t, 4> lutTag{0x6D, 0x66, 0x74, 0x32}; // 'mft2'
      cmsWriteRawTag(profile, cmsSigAToB0Tag, lutTag.data(), static_cast<cmsUInt32Number>(lutTag.size()));
      lutProfile = saveProfile(profile);
      cmsCloseProfile(profile);
    }
  }
  const umbriel::ClientIccDescription lut = umbriel::clientIccDescription(lutProfile);
  CHECK(!lut.description.has_value());
  CHECK(lut.error.contains("LUT"));

  // Truncated real profile data: the truncated path at the parse level.
  const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
  CHECK(!umbriel::clientIccDescription(cut).description.has_value());

  // A v4 header stamp is admitted as well (the major version is byte 8).
  std::vector<uint8_t> v4 = bytes;
  v4[8] = 4;
  CHECK(umbriel::clientIccDescription(v4).description.has_value());
#endif
}

UMBRIEL_TEST(bufferTransferFunctionsMapPowerFitsToNamedCurves) {
  umbriel::ColorDescription description;
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
  CHECK(umbriel::bufferTransferFunction(description) == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
  CHECK(umbriel::bufferTransferFunction(description) == WLR_COLOR_TRANSFER_FUNCTION_SRGB);
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
  CHECK(umbriel::bufferTransferFunction(description) == WLR_COLOR_TRANSFER_FUNCTION_SRGB);
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
  CHECK(umbriel::bufferTransferFunction(description) == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
  // Named curves the buffer setter cannot express never abort (the wlroots
  // converter would) and stay unspecified for the renderer to resolve.
  description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28;
  CHECK(umbriel::bufferTransferFunction(description) == static_cast<wlr_color_transfer_function>(0));

  // A fitted power curve renders through the nearest named EOTF exponent
  // (1.0 ext_linear, 2.2 gamma22, 2.4 bt1886) — the documented approximation.
  description.data.tf_named = 0;
  for (const auto& [exponent, expected] : {
           std::pair{1.0F, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR},
           std::pair{1.4F, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR},
           std::pair{1.8F, WLR_COLOR_TRANSFER_FUNCTION_GAMMA22},
           std::pair{2.2F, WLR_COLOR_TRANSFER_FUNCTION_GAMMA22},
           std::pair{2.4F, WLR_COLOR_TRANSFER_FUNCTION_BT1886},
           std::pair{2.8F, WLR_COLOR_TRANSFER_FUNCTION_BT1886},
       }) {
    description.tfPower = exponent;
    CHECK(umbriel::bufferTransferFunction(description) == expected);
  }

  // No transfer characteristic at all stays unspecified for the renderer.
  description.tfPower = 0.0F;
  CHECK(umbriel::bufferTransferFunction(description) == static_cast<wlr_color_transfer_function>(0));
}

UMBRIEL_TEST(clientIccDegeneratePrimariesFailGracefully) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // A collapsed colorant triangle must fail the description gracefully and
  // never reach the matrix math downstream.
  cmsToneCurve* curve = cmsBuildGamma(nullptr, 2.2);
  CHECK(curve != nullptr);
  if (curve == nullptr) {
    return;
  }
  cmsToneCurve* curves[3]{curve, curve, curve};
  const cmsCIExyY white = {.x = 0.3127, .y = 0.329, .Y = 1.0};
  const cmsCIExyYTRIPLE primaries{
      .Red = {.x = 0.64, .y = 0.33, .Y = 1.0},
      .Green = {.x = 0.30, .y = 0.60, .Y = 1.0},
      .Blue = {.x = 0.15, .y = 0.06, .Y = 1.0},
  };
  cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves);
  cmsFreeToneCurve(curve);
  CHECK(profile != nullptr);
  if (profile == nullptr) {
    return;
  }
  // cmsCreateRGBProfileTHR refuses singular colorants outright, so collapse
  // an accepted profile's colorants at the byte level instead.
  cmsUInt8Number redColorant[64];
  const cmsUInt32Number colorantSize = cmsReadRawTag(profile, cmsSigRedColorantTag, redColorant, sizeof(redColorant));
  CHECK(colorantSize >= 20);
  CHECK(cmsWriteRawTag(profile, cmsSigGreenColorantTag, redColorant, colorantSize));
  CHECK(cmsWriteRawTag(profile, cmsSigBlueColorantTag, redColorant, colorantSize));
  const std::vector<uint8_t> bytes = saveProfile(profile);
  cmsCloseProfile(profile);
  CHECK(!bytes.empty());

  const umbriel::ClientIccDescription result = umbriel::clientIccDescription(bytes);
  CHECK(!result.description.has_value());
  CHECK(!result.error.empty());
#endif
}

UMBRIEL_TEST(clientIccMalformedBytesFailGracefully) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Deterministic malformed inputs take the failed path with an error, never
  // a crash or a half-formed description.
  cmsToneCurve* curve = cmsBuildGamma(nullptr, 2.2);
  CHECK(curve != nullptr);
  const std::vector<uint8_t> base = matrixShaperProfile({curve, curve, curve});
  cmsFreeToneCurve(curve);
  CHECK(!base.empty());

  const auto rejects = [&](const std::vector<uint8_t>& bytes, const char* what) {
    const umbriel::ClientIccDescription result = umbriel::clientIccDescription(bytes);
    if (result.description.has_value() || result.error.empty()) {
      umbriel::test::reportFailure(__FILE__, __LINE__, std::format("client ICC accepted malformed input: {}", what));
    }
  };

  for (const size_t cut : {size_t{0}, size_t{64}, size_t{127}, base.size() / 2, base.size() - 1}) {
    rejects(std::vector<uint8_t>(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(cut)), "truncated");
  }
  {
    std::vector<uint8_t> bytes = base;
    std::fill(bytes.begin() + 128, bytes.begin() + 132, 0xFF);
    rejects(bytes, "oversized tag count");
  }
  {
    std::vector<uint8_t> bytes = base;
    std::fill(bytes.begin() + 132, bytes.begin() + 168, 0xFF);
    rejects(bytes, "flipped tag offsets");
  }
  {
    std::vector<uint8_t> bytes = base;
    std::fill(bytes.begin() + 200, bytes.begin() + 232, 0);
    rejects(bytes, "embedded NUL run");
  }
#endif
}

int main() { return RUN_TESTS(); }
