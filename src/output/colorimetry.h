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
#include <functional>
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
    // Kind::Curve only: the EOTF exponent when the sampled curve is exactly a
    // power (an ICC gamma or EDID gamma reaches us that way), 0 otherwise. It
    // lets the output description state the curve as the protocol's tf_power
    // instead of claiming a named curve the panel does not have.
    float exponent = 0.0F;
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

  // Decode sample: panel-encoded signal -> linear light, the exact inverse of
  // panelEncodeSample for every kind (analytic for a named curve or a power,
  // numerically inverted for a sampled table). Content tagged with a panel
  // description decodes through this.
  [[nodiscard]] float panelDecodeSample(const PanelTransfer& transfer, float encoded);

  // Classification of a parsed TRC against the curves the renderer can paint
  // for client content (wp_image_description_creator_icc_v1). Only exact
  // matches are representable: wlr_scene_buffer_set_transfer_function takes a
  // named curve, so Unsupported means the caller must refuse the description.
  // Output characterization does not use this: it keeps the sampled curve and
  // renders it exactly through a 3x1D LUT.
  struct TransferFit {
    enum class Kind {
      Srgb,
      Linear,
      Gamma22,
      Unsupported,
    };
    Kind kind = Kind::Srgb;
  };

  // Classify a transfer curve against the three exactly representable shapes:
  // linear, sRGB, and gamma 2.2.
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

  // The content decode for a sampled panel curve: a 3x1D LUT mapping encoded
  // values back to linear light. NULL when the curve is named or an exact
  // power, which the renderer decodes without a table.
  [[nodiscard]] wlr_color_transform* buildPanelDecodeTransform(const PanelColorimetry& panel);

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

  // Refcounted transform ownership for one that outlives the object that built
  // it: a description handed to clients and held by a scene buffer meanwhile.
  using SharedColorTransform = std::shared_ptr<wlr_color_transform>;
  [[nodiscard]] SharedColorTransform shareColorTransform(wlr_color_transform* transform);

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

  // Runs when the last copy of a store's payload dies. Client-created
  // descriptions attach one so their byte-budget charge follows the payload
  // itself: surface states and cached commits copy the store, so the client
  // resource's lifetime is the wrong proxy for the memory's. Empty for stores
  // that are not budgeted.
  using IccPayloadRelease = std::function<void()>;

  // File characterization: the store keeps the exact bytes the parser accepted
  // (the profile as written on disk, never a reserialization). Empty input
  // stores nothing. A release hook runs once, with the last copy.
  [[nodiscard]] IccProfileStore
  iccStoreForFileProfile(std::span<const uint8_t> fileBytes, IccPayloadRelease release = {});

  // Panel-only (EDID) characterization: the store synthesizes a matrix-shaper
  // ICC (colorants + white + TRC) matching the panel colorimetry. A null panel
  // (inert) stores nothing, and so does a build without lcms2 (no synthesis).
  [[nodiscard]] IccProfileStore iccStoreForPanel(const PanelColorimetry* panel);

} // namespace umbriel
