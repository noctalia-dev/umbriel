#include "output/colorimetry.h"

#include "output/colorimetry_internal.h"
#include "output/hdr_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
}

namespace umbriel {

  namespace {

    constexpr size_t kEncodeLutDim = 1024;

    float srgbEncode(float linear) {
      // Exact endpoints keep baked LUT ramps anchored at 0 and 65535.
      if (linear >= 1.0F) {
        return 1.0F;
      }
      return linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
    }

    float clampedUnit(float value) { return std::clamp(value, 0.0F, 1.0F); }

    wlr_color_cie1931_xy lerpXy(const wlr_color_cie1931_xy& from, const wlr_color_cie1931_xy& to, float amount) {
      return {
          .x = from.x + (to.x - from.x) * amount,
          .y = from.y + (to.y - from.y) * amount,
      };
    }

    std::vector<float> samplesFromEncode(float (*encode)(float, float), float shape) {
      std::vector<float> samples(color_detail::kCurveSamples);
      for (size_t index = 0; index < color_detail::kCurveSamples; ++index) {
        samples[index] = encode(color_detail::encodeGridLinear(index, color_detail::kCurveSamples), shape);
      }
      return samples;
    }

    void identityMatrix(float matrix[9]) {
      constexpr float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      std::copy(identity + 0, identity + 9, matrix);
    }

    void multiplyMatrix3(const double left[9], const double right[9], double out[9]) {
      double product[9];
      for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
          product[row * 3 + column] = left[row * 3] * right[column]
              + left[row * 3 + 1] * right[3 + column]
              + left[row * 3 + 2] * right[6 + column];
        }
      }
      std::copy(product, product + 9, out);
    }

    void xyzFromXy(wlr_color_cie1931_xy xy, double out[3]) {
      if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || xy.y <= 0.0F) {
        out[0] = out[1] = out[2] = 0.0;
        return;
      }
      out[0] = static_cast<double>(xy.x) / xy.y;
      out[1] = 1.0;
      out[2] = (1.0 - static_cast<double>(xy.x) - xy.y) / xy.y;
    }

    // RGB -> XYZ as row-major 3x3, primaries in columns scaled so the white
    // is the image of (1,1,1). False for degenerate chromaticities.
    bool primariesToXyzMatrix(const wlr_color_primaries& primaries, double matrix[9]) {
      // Row-major with each primary as a column: colorants[3 * row + column]
      // is the `row`-th XYZ component of the `column`-th primary.
      double colorants[9];
      const wlr_color_cie1931_xy chromaticities[3] = {primaries.red, primaries.green, primaries.blue};
      for (size_t column = 0; column < 3; ++column) {
        double primary[3];
        xyzFromXy(chromaticities[column], primary);
        if (primary[0] == 0.0 && primary[1] == 0.0 && primary[2] == 0.0) {
          return false;
        }
        for (size_t row = 0; row < 3; ++row) {
          colorants[3 * row + column] = primary[row];
        }
      }
      double white[3];
      xyzFromXy(primaries.white, white);
      if (white[1] == 0.0) {
        return false;
      }
      double inverse[9];
      if (!color_detail::invert3x3(colorants, inverse)) {
        return false;
      }
      double scale[3];
      for (size_t row = 0; row < 3; ++row) {
        scale[row] = inverse[row * 3] * white[0] + inverse[row * 3 + 1] * white[1] + inverse[row * 3 + 2] * white[2];
      }
      for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
          matrix[row * 3 + column] = scale[column] * colorants[row * 3 + column];
        }
      }
      return true;
    }

  } // namespace

  void ColorTransformDeleter::operator()(wlr_color_transform* transform) const { wlr_color_transform_unref(transform); }

  float panelEncodeSample(const PanelTransfer& transfer, float linear) {
    const float x = clampedUnit(linear);
    switch (transfer.kind) {
    case PanelTransfer::Kind::Srgb:
      return srgbEncode(x);
    case PanelTransfer::Kind::Gamma22:
      return std::pow(x, 1.0F / 2.2F);
    case PanelTransfer::Kind::Curve:
      break;
    }
    if (transfer.encode.empty()) {
      return x;
    }
    const float position = color_detail::encodeGridPosition(x, transfer.encode.size());
    const auto lower = static_cast<size_t>(position);
    const size_t upper = std::min(lower + 1, transfer.encode.size() - 1);
    const float fraction = position - static_cast<float>(lower);
    return transfer.encode[lower] + (transfer.encode[upper] - transfer.encode[lower]) * fraction;
  }

  namespace {

    // Probe set for the power fit: uniform light levels plus the geometric
    // shadow floor, where transfer curves separate most.
    std::vector<float> powerFitProbes() {
      std::vector<float> probes;
      for (int index = 1; index <= 512; ++index) {
        probes.push_back(static_cast<float>(index) / 512.0F);
      }
      for (int index = 1; index <= 96; ++index) {
        probes.push_back(std::pow(2.0F, -static_cast<float>(index) / 8.0F));
      }
      probes.push_back(1e-6F);
      return probes;
    }

    double powerFitWorst(const PanelTransfer& transfer, const std::vector<float>& probes, float exponent) {
      double worst = 0.0;
      for (const float linear : probes) {
        const double approximated = std::pow(static_cast<double>(linear), 1.0 / static_cast<double>(exponent));
        worst = std::max(worst, std::abs(approximated - static_cast<double>(panelEncodeSample(transfer, linear))));
      }
      return worst;
    }

  } // namespace

  TransferFit fitTransfer(const PanelTransfer& transfer) {
    switch (transfer.kind) {
    case PanelTransfer::Kind::Srgb:
      return {.kind = TransferFit::Kind::Srgb};
    case PanelTransfer::Kind::Gamma22:
      return {.kind = TransferFit::Kind::Gamma22};
    case PanelTransfer::Kind::Curve:
      break;
    }
    if (transfer.encode.size() < 2) {
      // An empty sampled table is the identity encode, i.e. a linear EOTF.
      return {.kind = TransferFit::Kind::Power, .exponent = 1.0F};
    }
    // Named fast paths first: an exact match is reported as the named curve,
    // never as an approximating power fit. Geometric probes reach the shadow
    // toes a uniform probe grid cannot see, where named curves differ most.
    constexpr float kMatchTolerance = 1e-3F;
    bool matchesSrgb = true;
    bool matchesGamma22 = true;
    for (int probe = 0; probe <= 384; ++probe) {
      const float linear =
          probe <= 256 ? static_cast<float>(probe) / 256.0F : std::pow(2.0F, -static_cast<float>(probe - 256) / 8.0F);
      const float encoded = panelEncodeSample(transfer, linear);
      if (std::abs(encoded - srgbEncode(linear)) > kMatchTolerance) {
        matchesSrgb = false;
      }
      if (std::abs(encoded - std::pow(linear, 1.0F / 2.2F)) > kMatchTolerance) {
        matchesGamma22 = false;
      }
    }
    if (matchesSrgb) {
      return {.kind = TransferFit::Kind::Srgb};
    }
    if (matchesGamma22) {
      return {.kind = TransferFit::Kind::Gamma22};
    }
    // The exponent is fitted to the worst-case linear-light error — the error
    // the panel actually renders. A log-domain least squares is
    // shadow-weighted: the deep samples of a linear toe drag the exponent away
    // from the power region and cost whole 8-bit codes at the top of the
    // range. Minimax over a coarse grid with a fine refinement is
    // deterministic and reaches the protocol vocabulary's shape.
    const std::vector<float> probes = powerFitProbes();
    auto worstAt = [&](float exponent) { return powerFitWorst(transfer, probes, exponent); };
    float bestExponent = 1.0F;
    double bestWorst = std::numeric_limits<double>::infinity();
    for (int step = 2; step <= 80; ++step) {
      const float candidate = static_cast<float>(step) * 0.25F;
      const double worst = worstAt(candidate);
      if (worst < bestWorst) {
        bestWorst = worst;
        bestExponent = candidate;
      }
    }
    const float coarse = bestExponent;
    for (int step = -50; step <= 50; ++step) {
      const float candidate = std::max(0.05F, coarse + static_cast<float>(step) * 0.005F);
      const double worst = worstAt(candidate);
      if (worst < bestWorst) {
        bestWorst = worst;
        bestExponent = candidate;
      }
    }
    return {.kind = TransferFit::Kind::Power, .exponent = bestExponent};
  }

  void sdrGamutMatrix(const wlr_color_primaries& panel, float wideness, float matrix[9]) {
    wlr_color_primaries srgb{};
    wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
    const float amount = std::clamp(wideness, 0.0F, 1.0F);
    if (amount >= 1.0F) {
      // Native: content is already in the panel's gamut, so the mapping is the
      // identity and only the panel TRC would remain (the runtime stages
      // nothing at all instead, see sdrGamutStaged).
      identityMatrix(matrix);
      return;
    }
    wlr_color_primaries source = srgb;
    if (amount > 0.0F) {
      // Linear interpolation between the two gamuts; the primaries slide while
      // the white stays the source white.
      source = {
          .red = lerpXy(srgb.red, panel.red, amount),
          .green = lerpXy(srgb.green, panel.green, amount),
          .blue = lerpXy(srgb.blue, panel.blue, amount),
          .white = srgb.white,
      };
    }
    // White-preserving (relative colorimetric) mapping: source white lands on
    // the panel white exactly, so switching native <-> srgb never changes how
    // white renders and non-D65 panels neither tint nor clip it. Only chroma
    // is confined: the source primaries are converted through the panel's
    // colorimetry with one Bradford adaptation step between the two whites.
    double sourceToXyz[9];
    double panelToXyz[9];
    double xyzToPanel[9];
    double sourceWhite[3];
    double panelWhite[3];
    if (!primariesToXyzMatrix(source, sourceToXyz)
        || !primariesToXyzMatrix(panel, panelToXyz)
        || !color_detail::invert3x3(panelToXyz, xyzToPanel)) {
      identityMatrix(matrix);
      return;
    }
    xyzFromXy(source.white, sourceWhite);
    xyzFromXy(panel.white, panelWhite);
    double adapt[9];
    color_detail::bradfordAdapt(sourceWhite, panelWhite, adapt);
    double adaptedSource[9];
    double composed[9];
    multiplyMatrix3(adapt, sourceToXyz, adaptedSource);
    multiplyMatrix3(xyzToPanel, adaptedSource, composed);
    for (size_t index = 0; index < 9; ++index) {
      matrix[index] = static_cast<float>(composed[index]);
    }
  }

  wlr_color_transform* buildSdrGamutTransform(const PanelColorimetry& panel) {
    float matrix[9];
    sdrGamutMatrix(panel.primaries, 0.0F, matrix);
    wlr_color_transform* matrixStage = wlr_color_transform_init_matrix(matrix);
    if (matrixStage == nullptr) {
      return nullptr;
    }

    wlr_color_transform* stages[3] = {matrixStage, nullptr, nullptr};
    size_t stageCount = 2;
    switch (panel.transfer.kind) {
    case PanelTransfer::Kind::Srgb:
      stages[1] = wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
      break;
    case PanelTransfer::Kind::Gamma22:
      stages[1] = wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
      break;
    case PanelTransfer::Kind::Curve: {
      // The sampled curve is split around an analytic stage: the renderer's
      // gamma 2.2 stage carries the near-vertical shadow slope exactly, and
      // the LUT only corrects the residual, which is flat enough for the LUT's
      // uniform linear sampling to follow without shadow error.
      stages[1] = wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
      std::vector<uint16_t> lut(kEncodeLutDim);
      for (size_t index = 0; index < lut.size(); ++index) {
        const float corrected = static_cast<float>(index) / static_cast<float>(lut.size() - 1);
        const float encoded = panelEncodeSample(panel.transfer, std::pow(corrected, 2.2F));
        lut[index] = static_cast<uint16_t>(std::lround(clampedUnit(encoded) * 65535.0F));
      }
      stages[2] = wlr_color_transform_init_lut_3x1d(lut.size(), lut.data(), lut.data(), lut.data());
      stageCount = 3;
      break;
    }
    }
    bool failed = stages[1] == nullptr;
    if (stageCount == 3) {
      failed = failed || stages[2] == nullptr;
    }
    if (failed) {
      for (size_t index = 0; index < stageCount; ++index) {
        if (stages[index] != nullptr) {
          wlr_color_transform_unref(stages[index]);
        }
      }
      return nullptr;
    }

    wlr_color_transform* pipeline = wlr_color_transform_init_pipeline(stages, stageCount);
    for (size_t index = 0; index < stageCount; ++index) {
      wlr_color_transform_unref(stages[index]);
    }
    return pipeline;
  }

  IccProfileStore iccStoreForFileProfile(std::span<const uint8_t> fileBytes) {
    if (fileBytes.empty()) {
      return {};
    }
    // The store is the file exactly as read: get_information advertises the
    // profile the user configured, quirks and all.
    return std::make_shared<const std::vector<uint8_t>>(fileBytes.begin(), fileBytes.end());
  }

  std::optional<PanelColorimetry>
  panelColorimetryFromEdid(std::span<const uint8_t> edid, const wlr_color_primaries* defaultPrimaries) {
    if (edid.empty() || defaultPrimaries == nullptr || !validDisplayPrimaries(*defaultPrimaries)) {
      return std::nullopt;
    }
    std::unique_ptr<di_info, decltype(&di_info_destroy)> info(
        di_info_parse_edid(edid.data(), edid.size()), di_info_destroy
    );
    if (!info) {
      return std::nullopt;
    }
    const di_edid* parsed = di_info_get_edid(info.get());
    if (parsed == nullptr) {
      return std::nullopt;
    }

    PanelColorimetry panel{
        .primaries = *defaultPrimaries,
        .white = {defaultPrimaries->white.x, defaultPrimaries->white.y},
        .transfer = {},
    };
    if (const di_edid_chromaticity_coords* coords = di_edid_get_chromaticity_coords(parsed); coords != nullptr
        && std::isfinite(coords->white_x)
        && std::isfinite(coords->white_y)
        && coords->white_x > 0.0F
        && coords->white_y > 0.0F
        && coords->white_x + coords->white_y <= 1.0F) {
      panel.white[0] = coords->white_x;
      panel.white[1] = coords->white_y;
    }
    panel.primaries.white = {.x = panel.white[0], .y = panel.white[1]};

    // EDID gamma is the decode exponent; its inverse is the panel encode.
    // Unknown/implausible values fall back to the common 2.2 convention.
    const float gamma = di_edid_get_basic_gamma(parsed);
    if (std::isfinite(gamma) && std::abs(gamma - 2.2F) <= 0.01F) {
      panel.transfer.kind = PanelTransfer::Kind::Gamma22;
    } else if (std::isfinite(gamma) && gamma >= 1.0F && gamma <= 4.0F) {
      panel.transfer.kind = PanelTransfer::Kind::Curve;
      panel.transfer.encode =
          samplesFromEncode([](float linear, float shape) { return std::pow(linear, 1.0F / shape); }, gamma);
    } else {
      panel.transfer.kind = PanelTransfer::Kind::Gamma22;
    }
    return panel;
  }

  namespace color_detail {

    namespace {

      uint32_t readU32At(std::span<const uint8_t> data, size_t at) {
        return (static_cast<uint32_t>(data[at]) << 24)
            | (static_cast<uint32_t>(data[at + 1]) << 16)
            | (static_cast<uint32_t>(data[at + 2]) << 8)
            | static_cast<uint32_t>(data[at + 3]);
      }

      uint16_t readU16At(std::span<const uint8_t> data, size_t at) {
        return static_cast<uint16_t>((static_cast<uint16_t>(data[at]) << 8) | data[at + 1]);
      }

      // A curv or para tag must declare counts that fit its own tag data.
      std::string curveCountError(std::span<const uint8_t> data, size_t tagOffset, size_t tagSize) {
        if (tagSize < 12) {
          return {};
        }
        const uint32_t type = readU32At(data, tagOffset);
        if (type == 0x63757276) { // 'curv'
          const uint32_t count = readU32At(data, tagOffset + 8);
          if (12ULL + 2ULL * count > tagSize) {
            return "ICC curv tag count is not in bounds";
          }
        } else if (type == 0x70617261) { // 'para'
          const uint16_t functionType = readU16At(data, tagOffset + 8);
          constexpr uint32_t parameterCounts[5] = {1, 3, 4, 5, 7};
          if (functionType > 4) {
            return "ICC para tag function type is invalid";
          }
          if (12ULL + 4ULL * parameterCounts[functionType] > tagSize) {
            return "ICC para tag parameters are not in bounds";
          }
        }
        return {};
      }

    } // namespace

    std::string iccStructureError(std::span<const uint8_t> data) {
      if (data.size() < 128) {
        return "truncated ICC profile (header incomplete)";
      }
      const uint32_t declaredSize = readU32At(data, 0);
      if (declaredSize < 132) {
        return "truncated ICC profile (size field under the tag table)";
      }
      if (declaredSize > data.size()) {
        return "truncated ICC profile (size field exceeds the file)";
      }
      if (data[36] != 'a' || data[37] != 'c' || data[38] != 's' || data[39] != 'p') {
        return "not an ICC profile (magic signature missing)";
      }
      const uint8_t versionMajor = data[8];
      if (versionMajor != 2 && versionMajor != 4) {
        return "unsupported ICC profile version (must be 2 or 4)";
      }
      const uint64_t usable = declaredSize;
      const uint32_t tagCount = readU32At(data, 128);
      if (tagCount > (usable - 132) / 12) {
        return "ICC tag table is not in bounds";
      }
      for (uint32_t index = 0; index < tagCount; ++index) {
        const size_t entry = 132 + 12 * index;
        const uint32_t tagOffset = readU32At(data, entry + 4);
        const uint32_t tagSize = readU32At(data, entry + 8);
        if (tagOffset > usable || tagSize > usable - tagOffset) {
          return "ICC tag entry leaves the profile";
        }
        if (const std::string curveError = curveCountError(data, tagOffset, tagSize); !curveError.empty()) {
          return curveError;
        }
      }
      return {};
    }

    bool invert3x3(const double in[9], double out[9]) {
      const double determinant = in[0] * (in[4] * in[8] - in[5] * in[7])
          - in[1] * (in[3] * in[8] - in[5] * in[6])
          + in[2] * (in[3] * in[7] - in[4] * in[6]);
      if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
        return false;
      }
      out[0] = (in[4] * in[8] - in[5] * in[7]) / determinant;
      out[1] = (in[2] * in[7] - in[1] * in[8]) / determinant;
      out[2] = (in[1] * in[5] - in[2] * in[4]) / determinant;
      out[3] = (in[5] * in[6] - in[3] * in[8]) / determinant;
      out[4] = (in[0] * in[8] - in[2] * in[6]) / determinant;
      out[5] = (in[2] * in[3] - in[0] * in[5]) / determinant;
      out[6] = (in[3] * in[7] - in[4] * in[6]) / determinant;
      out[7] = (in[1] * in[6] - in[0] * in[7]) / determinant;
      out[8] = (in[0] * in[4] - in[1] * in[3]) / determinant;
      return true;
    }

    void bradfordAdapt(const double fromXyz[3], const double toXyz[3], double out[9]) {
      constexpr double bradford[9] = {
          0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296,
      };
      double bradfordInverse[9];
      if (!invert3x3(bradford, bradfordInverse)) {
        std::fill(out, out + 9, 0.0);
        out[0] = out[4] = out[8] = 1.0;
        return;
      }
      double fromCones[3];
      double toCones[3];
      for (size_t row = 0; row < 3; ++row) {
        fromCones[row] =
            bradford[row * 3] * fromXyz[0] + bradford[row * 3 + 1] * fromXyz[1] + bradford[row * 3 + 2] * fromXyz[2];
        toCones[row] =
            bradford[row * 3] * toXyz[0] + bradford[row * 3 + 1] * toXyz[1] + bradford[row * 3 + 2] * toXyz[2];
      }
      double scale[3] = {1.0, 1.0, 1.0};
      for (size_t row = 0; row < 3; ++row) {
        if (!std::isfinite(fromCones[row])
            || !std::isfinite(toCones[row])
            || fromCones[row] <= 0.0
            || toCones[row] <= 0.0) {
          std::fill(out, out + 9, 0.0);
          out[0] = out[4] = out[8] = 1.0;
          return;
        }
        scale[row] = toCones[row] / fromCones[row];
      }
      double scaled[9];
      for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
          scaled[row * 3 + column] = bradford[row * 3 + column] * scale[row];
        }
      }
      double composed[9];
      multiplyMatrix3(bradfordInverse, scaled, composed);
      std::copy(composed, composed + 9, out);
    }

  } // namespace color_detail

} // namespace umbriel
