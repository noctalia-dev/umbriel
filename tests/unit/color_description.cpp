#include "server/color_description.h"

#include "check.h"

#include <array>

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

int main() { return RUN_TESTS(); }
