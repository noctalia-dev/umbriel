#include "server/color_description.h"

#include "output/colorimetry_internal.h"

#include <cmath>
#include <utility>

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

    // A profile swap with identical colorimetry is still a description change
    // for the clients reading it through icc_file.
    bool iccProfilesEqual(const IccProfileStore& left, const IccProfileStore& right) {
      return left == right || (left != nullptr && right != nullptr && *left == *right);
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
        && left.tfPower == right.tfPower
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
        && iccProfilesEqual(left.iccProfile, right.iccProfile)
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

  ColorDescription characterizedOutputDescription(
      ColorDescription description, const wlr_color_primaries* panelPrimaries, uint32_t sdrTransferTfNamed,
      IccProfileStore iccProfile
  ) {
    // HDR (PQ) descriptions keep their BT.2020 encoding contract, including
    // the automatic-HDR offer; only SDR descriptions reflect the panel.
    if (panelPrimaries == nullptr || description.data.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
      return description;
    }
    description.data.primaries_named = 0;
    description.customPrimaries = *panelPrimaries;
    description.data.tf_named = sdrTransferTfNamed;
    description.iccProfile = std::move(iccProfile);
    return description;
  }

  ClientIccDescription clientIccDescription(std::span<const uint8_t> iccBytes) {
    // Wire-level set_icc_file enforces the size contract first (bad_size);
    // repeating it here keeps the validation vocabulary whole for callers that
    // hand bytes over without the request, and pins the limit in unit tests.
    if (iccBytes.empty()) {
      return {.description = std::nullopt, .error = "empty ICC profile data"};
    }
    if (iccBytes.size() > kClientIccSizeLimit) {
      return {.description = std::nullopt, .error = "ICC profile exceeds the 32 MB limit"};
    }
    // The ICC.1 profile header is 128 bytes; the version field's major
    // version is its byte 8. The protocol admits exactly versions 2 and 4.
    if (iccBytes.size() < 128) {
      return {.description = std::nullopt, .error = "truncated ICC profile (header incomplete)"};
    }
    const uint8_t versionMajor = iccBytes[8];
    if (versionMajor != 2 && versionMajor != 4) {
      return {.description = std::nullopt, .error = "unsupported ICC profile version (must be 2 or 4)"};
    }
    // The bytes are client-supplied up to 32 MB: structural bounds are
    // re-checked at this untrusted boundary before any parser sees them.
    if (const std::string structureError = color_detail::iccStructureError(iccBytes); !structureError.empty()) {
      return {.description = std::nullopt, .error = structureError};
    }
    const IccParse parsed = parseIccProfile(iccBytes, IccClassScope::DisplayOrColorSpace);
    if (!parsed.colorimetry) {
      return {.description = std::nullopt, .error = parsed.error.empty() ? "unsupported ICC profile" : parsed.error};
    }
    const PanelColorimetry& colorimetry = *parsed.colorimetry;
    const TransferFit fit = fitTransfer(colorimetry.transfer);

    ColorDescription description;
    description.data.primaries_named = 0;
    description.customPrimaries = colorimetry.primaries;
    switch (fit.kind) {
    case TransferFit::Kind::Srgb:
      description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
      break;
    case TransferFit::Kind::Gamma22:
      description.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
      break;
    case TransferFit::Kind::Power:
      description.tfPower = fit.exponent;
      break;
    }
    description.luminances = defaultColorLuminances(description.data.tf_named);
    // The client's exact bytes, never a reserialization: get_information's
    // icc_file event must hand back precisely what set_icc_file received.
    description.iccProfile = iccStoreForFileProfile(iccBytes);
    return {.description = std::move(description), .error = {}};
  }

  wlr_color_transfer_function bufferTransferFunction(const ColorDescription& description) {
    // Deliberately not wlr_color_manager_v1_transfer_function_to_wlr: that
    // converter aborts on named values wlroots does not map (including the
    // deprecated-but-live WP_..._SRGB, which only maps through
    // compound_power_2_4) and untrusted client descriptions must never reach
    // an assert.
    switch (description.data.tf_named) {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
      return WLR_COLOR_TRANSFER_FUNCTION_BT1886;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
      return WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
      return WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
      return WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
      // The IEC 61966-2-1 curve under either protocol name.
      return WLR_COLOR_TRANSFER_FUNCTION_SRGB;
    default:
      break;
    }
    if (description.tfPower <= 0.0F) {
      // A named curve the buffer setter cannot express (or no curve at all)
      // stays unspecified: the renderer resolves it to the output's SDR
      // transfer policy instead of guessing.
      return static_cast<wlr_color_transfer_function>(0);
    }
    // A fitted power curve has no buffer-side representation — the setter
    // takes only the named wlr_color_transfer_function values — so it renders
    // through the nearest named EOTF exponent (ext_linear 1.0, gamma22 2.2,
    // bt1886 2.4), midpoint-bounded. This is an approximation: faithful
    // non-representable TRCs need per-buffer 3D LUTs (see fitTransfer).
    if (description.tfPower <= 1.6F) {
      return WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
    }
    if (description.tfPower <= 2.3F) {
      return WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
    }
    return WLR_COLOR_TRANSFER_FUNCTION_BT1886;
  }

} // namespace umbriel
