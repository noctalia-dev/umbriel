#pragma once

#include "output/colorimetry.h"
#include "wlr_color.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

extern "C" {
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  struct ColorLuminances {
    float min = 0.2F;
    float max = 80.0F;
    float reference = 80.0F;
  };

  struct ColorDescription {
    wlr_image_description_v1_data data{};
    wlr_color_primaries customPrimaries{};
    // Raw ICC bytes get_information advertises via its icc_file event ("ICC
    // profile matching the image description"). Empty when the description has
    // no matching profile: inert or HDR/PQ descriptions. Output descriptions
    // carry the characterization's profile; client-created ICC descriptions
    // carry the client's exact bytes (color-management-v1.xml,
    // wp_image_description_info_v1: "if the image description contains an ICC
    // profile, it must send the icc_file event").
    IccProfileStore iccProfile;
    ColorLuminances luminances;
    float luminanceMultiplier = 1.0F;
    bool customLuminances = false;
    bool absoluteLuminance = false;
    bool requiresHdrOutput = false;
  };

  ColorLuminances defaultColorLuminances(uint32_t transferFunction);
  bool colorDescriptionsEqual(const ColorDescription& left, const ColorDescription& right);

  // Translate the piecewise sRGB curve at the negotiated protocol boundary.
  uint32_t colorTransferFunctionToWire(uint32_t transferFunction, uint32_t version);
  uint32_t colorTransferFunctionFromWire(uint32_t transferFunction, uint32_t version);

  // HDR encodings can carry SDR content. A known content peak at or below
  // reference white does not request an automatic display mode switch unless
  // the client also supplies HDR mastering luminance.
  bool colorDescriptionRequestsHdrOutput(const ColorDescription& description);

  // Automatic HDR offers a PQ encoding before scanout switches. Its target
  // volume still describes the current output, independently of that encoding.
  ColorDescription outputColorDescription(
      const wlr_output_image_description* outputDescription, float effectiveSdrWhite, bool automaticHdrAvailable = false
  );

  // Characterized-panel override for output descriptions. While
  // an ICC/EDID characterization is active the description reflects the
  // panel's real colorimetry (custom primaries) so tagged clients convert
  // accurately — their pixels can change even under sdr_gamut = "native" —
  // while untagged content stays untouched unless sdr_gamut = "srgb" stages
  // the mapping. Independent of the sdr_gamut policy; the named transfer keeps
  // coming from the sdr_transfer policy (`sdrTransferTfNamed`), and HDR (PQ)
  // descriptions keep their encoding contract untouched. The characterization's
  // ICC byte store rides along for get_information's icc_file advertising under
  // the same gate — the profile matches exactly these described SDR outputs,
  // never inert or PQ descriptions. Inert without a characterization
  // (`panelPrimaries == nullptr`).
  [[nodiscard]] ColorDescription characterizedOutputDescription(
      ColorDescription description, const wlr_color_primaries* panelPrimaries, uint32_t sdrTransferTfNamed,
      IccProfileStore iccProfile
  );

  // Client-provided ICC descriptions
  // (wp_image_description_creator_icc_v1.set_icc_file). The profile must be ICC
  // v2/v4, 3-channel, of Display or ColorSpace class, matrix-shaper only, and
  // at most 32 MB; anything else is rejected here and delivered as
  // wp_image_description_v1.failed ('unsupported') by the caller — never a
  // protocol error. On success the description carries the client's exact
  // bytes in iccProfile plus the colorimetry the renderer can express:
  // custom primaries from the colorants and one of the two exactly
  // representable transfer curves (sRGB, gamma 2.2). Any other TRC is refused
  // rather than approximated by a different named curve.
  struct ClientIccDescription {
    std::optional<ColorDescription> description;
    // Rejection reason, empty on success.
    std::string error;
  };
  // set_icc_file's size limit (color-management-v1.xml: "The maximum size of
  // the ICC profile is 32 MB").
  inline constexpr size_t kClientIccSizeLimit = 32ULL * 1024ULL * 1024ULL;
  [[nodiscard]] ClientIccDescription
  clientIccDescription(std::span<const uint8_t> iccBytes, IccPayloadRelease release = {});

  // The named transfer function whose curve is exactly the power curve for
  // `exponent`, or 0. The renderer paints client content through named curves
  // only (wlr_scene_buffer_set_transfer_function takes no exponent), so only
  // ext_linear (1.0) and gamma 2.2 are exact: BT.1886 carries the BT.2035
  // luminance offset and is not a plain power, and every other exponent has no
  // named equivalent at all.
  [[nodiscard]] uint32_t namedTransferForPower(float exponent);

  // Buffer-side transfer function for a description. Named transfer functions
  // map directly; a name the setter cannot express stays unspecified (0), and
  // the renderer resolves it through the output's SDR transfer policy.
  [[nodiscard]] wlr_color_transfer_function bufferTransferFunction(const ColorDescription& description);

} // namespace umbriel
