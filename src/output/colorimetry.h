#pragma once

// Panel colorimetry characterization and SDR gamut mapping.
// A PanelColorimetry describes what untagged SDR signal values mean to the
// panel: its primaries, white point, and transfer curve. With that, "gamut
// limiting" is a plain output transform that interprets untagged SDR as sRGB
// and converts it into the panel's colorimetry.

#include "config/config.h"
#include "wlr_color.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace umbriel {

  // How the panel encodes linear light into signal values (the inverse of its
  // EOTF). Srgb and Gamma22 map to named inverse-EOTF stages in the renderer;
  // Curve is a sampled encode curve baked into a 3x1D LUT.
  struct PanelTransfer {
    enum class Kind {
      Srgb,
      Gamma22,
      Curve,
    };
    Kind kind = Kind::Srgb;
    // Kind::Curve only: encode((i / (size - 1))²) for linear inputs in [0, 1].
    // The input grid clusters at zero because encode curves rise steeply
    // there; a uniform grid keeps several 8-bit codes of lerp error in the
    // shadows no matter how densely it is sampled.
    std::vector<float> encode;
  };

  struct PanelColorimetry {
    wlr_color_primaries primaries;
    // Panel white point (x, y); primaries.white mirrors it.
    float white[2];
    PanelTransfer transfer;
  };

  // Encode sample: linear light -> panel-encoded signal. For Curve the sampled
  // encode table is linearly interpolated.
  [[nodiscard]] float panelEncodeSample(const PanelTransfer& transfer, float linear);

  // Fitted transfer characteristic of a parsed profile
  // (wp_image_description_creator_icc_v1). Srgb and Gamma22 are exact named
  // fast paths; Power is a parametric power fit in the protocol's
  // set_tf_power/tf_power vocabulary: the EOTF exponent e with
  // optical = electrical^e, fitted to minimize the worst-case linear-light
  // error (the request's valid range is 1.0..10.0; a fit outside it is kept
  // as fitted and only clamped by the buffer mapping).
  struct TransferFit {
    enum class Kind {
      Srgb,
      Gamma22,
      Power,
    };
    Kind kind = Kind::Srgb;
    // Kind::Power only: the fitted EOTF exponent.
    float exponent = 0.0F;
  };

  // Fit a transfer curve to what the renderer can express. Non-representable
  // TRCs are approximated by a power fit — faithful rendering of those needs
  // per-buffer 3D LUTs (a new render transform type).
  [[nodiscard]] TransferFit fitTransfer(const PanelTransfer& transfer);

  // Output-side SDR gamut mapping matrix, generic over `wideness` in [0, 1]
  // (0 = interpret untagged SDR strictly as sRGB, 1 = as the
  // panel's native gamut, linear primary interpolation in between). The
  // mapping is white-preserving (relative colorimetric): source white always
  // lands on the panel white, so switching native <-> srgb never changes how
  // white renders, and only chroma is confined to the sRGB gamut. Wideness 1
  // is the identity matrix — it maps nothing and applies no panel TRC (the
  // runtime stages no transform at all instead, see sdrGamutStaged).
  // Intermediate values are the future slider's math and are never selected
  // at runtime yet.
  void sdrGamutMatrix(const wlr_color_primaries& panel, float wideness, float matrix[9]);

  // The supplied scene transform for SDR gamut mapping: one matrix stage
  // (sRGB -> panel, wideness 0) followed by the panel's encode stage — the
  // named inverse-EOTF for named curves, or an analytic gamma 2.2 stage with
  // the residual baked to a 3x1D LUT for sampled curves. Returns NULL on
  // allocation failure.
  [[nodiscard]] wlr_color_transform* buildSdrGamutTransform(const PanelColorimetry& panel);

  // The staging decision, factored out of Output: stage the SDR gamut
  // transform only when a characterization is active, the policy is "srgb",
  // and no HDR image description owns the output transform. "native" stages
  // nothing at all so pixels stay bit-identical to an uncharacterized output.
  [[nodiscard]] constexpr bool sdrGamutStaged(bool characterized, SdrGamut gamut, bool hdrActive) {
    return characterized && gamut == SdrGamut::Srgb && !hdrActive;
  }

  struct IccParse {
    // Empty when the profile is rejected or the build lacks ICC support.
    std::optional<PanelColorimetry> colorimetry;
    // Rejection reason, empty on success.
    std::string error;
    // Non-fatal notes (e.g. a vcgt tag that was ignored).
    std::vector<std::string> warnings;
  };

  // Owning handle for a wlroots color transform.
  struct ColorTransformDeleter {
    void operator()(wlr_color_transform* transform) const;
  };
  using ColorTransformPtr = std::unique_ptr<wlr_color_transform, ColorTransformDeleter>;

  // Which device classes the parser admits. Output characterization wants the
  // display profile a factory shipped; wp_image_description_creator_icc_v1
  // additionally allows the ICC ColorSpace class (color-management-v1.xml,
  // set_icc_file: "the class must be Display or ColorSpace").
  enum class IccClassScope {
    DisplayOnly,
    DisplayOrColorSpace,
  };

  // Parses an ICC v2/v4 matrix-shaper profile (rXYZ/gXYZ/bXYZ + wtpt +
  // chad + rTRC/gTRC/bTRC). LUT profiles (A2B0/B2A0) and out-of-scope classes
  // are rejected; a vcgt tag is ignored with a warning. Built without lcms2
  // this always fails with an "unsupported" error.
  [[nodiscard]] IccParse
  parseIccProfile(std::span<const uint8_t> data, IccClassScope scope = IccClassScope::DisplayOnly);

  // EDID-based characterization: colorants from the backend's default
  // primaries, gamma and white point from the EDID. Empty EDID or missing
  // primaries (nested/headless outputs) stays inert with nullopt.
  [[nodiscard]] std::optional<PanelColorimetry>
  panelColorimetryFromEdid(std::span<const uint8_t> edid, const wlr_color_primaries* defaultPrimaries);

  // Raw ICC bytes advertised by get_information's icc_file for a characterized
  // output. Shared because image descriptions snapshot the store: every
  // information request on the same description must return the exact same
  // data.
  using IccProfileStore = std::shared_ptr<const std::vector<uint8_t>>;

  // File characterization: the store keeps the exact bytes the parser accepted
  // (the profile as written on disk, never a reserialization). Empty input
  // stores nothing.
  [[nodiscard]] IccProfileStore iccStoreForFileProfile(std::span<const uint8_t> fileBytes);

  // Panel-only (EDID) characterization: the store synthesizes a matrix-shaper
  // ICC (colorants + white + TRC) matching the panel colorimetry. A null panel
  // (inert) stores nothing, and so does a build without lcms2 (no synthesis).
  [[nodiscard]] IccProfileStore iccStoreForPanel(const PanelColorimetry* panel);

} // namespace umbriel
