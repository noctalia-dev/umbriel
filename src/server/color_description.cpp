#include "server/color_description.h"

#include <cmath>

namespace umbriel {

  namespace {

    bool primariesEqual(const wlr_color_primaries& left, const wlr_color_primaries& right) {
      return left.red.x == right.red.x
          && left.red.y == right.red.y
          && left.green.x == right.green.x
          && left.green.y == right.green.y
          && left.blue.x == right.blue.x
          && left.blue.y == right.blue.y
          && left.white.x == right.white.x
          && left.white.y == right.white.y;
    }

    bool primariesSet(const wlr_color_primaries& primaries) {
      return primaries.red.x != 0.0F
          || primaries.red.y != 0.0F
          || primaries.green.x != 0.0F
          || primaries.green.y != 0.0F
          || primaries.blue.x != 0.0F
          || primaries.blue.y != 0.0F
          || primaries.white.x != 0.0F
          || primaries.white.y != 0.0F;
    }

  } // namespace

  ColorLuminances defaultColorLuminances(uint32_t transferFunction) {
    switch (transferFunction) {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
      return {.min = 0.005F, .max = 10000.0F, .reference = 203.0F};
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
      return {.min = 0.01F, .max = 100.0F, .reference = 100.0F};
    default:
      return {};
    }
  }

  bool colorDescriptionsEqual(const ColorDescription& left, const ColorDescription& right) {
    const auto& a = left.data;
    const auto& b = right.data;
    return a.tf_named == b.tf_named
        && a.primaries_named == b.primaries_named
        && (a.primaries_named != 0 || primariesEqual(left.customPrimaries, right.customPrimaries))
        && a.has_mastering_display_primaries == b.has_mastering_display_primaries
        && (!a.has_mastering_display_primaries
            || primariesEqual(a.mastering_display_primaries, b.mastering_display_primaries))
        && a.has_mastering_luminance == b.has_mastering_luminance
        && (!a.has_mastering_luminance
            || (a.mastering_luminance.min == b.mastering_luminance.min
                && a.mastering_luminance.max == b.mastering_luminance.max))
        && a.max_cll == b.max_cll
        && a.max_fall == b.max_fall
        && left.luminances.min == right.luminances.min
        && left.luminances.max == right.luminances.max
        && left.luminances.reference == right.luminances.reference
        && left.luminanceMultiplier == right.luminanceMultiplier
        && left.customLuminances == right.customLuminances
        && left.absoluteLuminance == right.absoluteLuminance
        && left.requiresHdrOutput == right.requiresHdrOutput;
  }

  bool colorDescriptionRequestsHdrOutput(const ColorDescription& description) {
    // Clients can compose SDR into PQ using the advertised preferred encoding.
    // A declared SDR content peak must not trigger HDR just because of that
    // encoding. Keep HDR mastering metadata and unknown peaks eligible.
    return description.requiresHdrOutput
        && (description.data.max_cll == 0
            || static_cast<float>(description.data.max_cll) > description.luminances.reference
            || (description.data.has_mastering_luminance
                && description.data.mastering_luminance.max > description.luminances.reference));
  }

  ColorDescription outputColorDescription(
      const wlr_output_image_description* outputDescription, float effectiveSdrWhite, bool automaticHdrAvailable
  ) {
    ColorDescription description;
    description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
    description.data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    if (outputDescription == nullptr) {
      if (automaticHdrAvailable) {
        // The compositor accepts HDR content while the display is still SDR.
        // Keep its current SDR destination separate from the logical PQ range
        // so clients can request HDR without waiting for an HDR scanout first.
        description.data.has_mastering_display_primaries = true;
        wlr_color_primaries_from_named(&description.data.mastering_display_primaries, WLR_COLOR_NAMED_PRIMARIES_SRGB);
        description.data.has_mastering_luminance = true;
        description.data.mastering_luminance = {
            .min = description.luminances.min,
            .max = description.luminances.max,
        };
        description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        description.data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        description.luminances = defaultColorLuminances(description.data.tf_named);
        description.luminances.reference = effectiveSdrWhite;
        description.customLuminances = true;
        description.requiresHdrOutput = true;
      }
      return description;
    }

    description.data.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(outputDescription->transfer_function);
    description.data.primaries_named = wlr_color_manager_v1_primaries_from_wlr(outputDescription->primaries);
    description.luminances = defaultColorLuminances(description.data.tf_named);
    if (description.data.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
      description.luminances.reference = effectiveSdrWhite;
      description.customLuminances = true;
      description.requiresHdrOutput = true;
    }
    if (primariesSet(outputDescription->mastering_display_primaries)) {
      description.data.has_mastering_display_primaries = true;
      description.data.mastering_display_primaries = outputDescription->mastering_display_primaries;
    }
    if (outputDescription->mastering_luminance.max > outputDescription->mastering_luminance.min) {
      description.data.has_mastering_luminance = true;
      description.data.mastering_luminance = {
          .min = static_cast<float>(outputDescription->mastering_luminance.min),
          .max = static_cast<float>(outputDescription->mastering_luminance.max),
      };
    }
    description.data.max_cll = static_cast<uint32_t>(std::lround(outputDescription->max_cll));
    description.data.max_fall = static_cast<uint32_t>(std::lround(outputDescription->max_fall));
    return description;
  }

} // namespace umbriel
