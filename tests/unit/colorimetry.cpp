// Panel characterization and SDR gamut mapping.
// ICC cases skip when built without lcms2; the pure matrix/TRC/decision pins always run.

#include "output/colorimetry.h"

#include "check.h"
#include "output/hdr_metadata.h"
#include "server/color_description.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if UMBRIEL_HAS_LCMS2
#include <lcms2.h>
#endif

namespace {

  bool closeTo(double actual, double expected, double tolerance = 0.002) {
    return std::abs(actual - expected) < tolerance;
  }

  wlr_color_primaries srgbPrimaries() {
    return {
        .red = {.x = 0.64F, .y = 0.33F},
        .green = {.x = 0.30F, .y = 0.60F},
        .blue = {.x = 0.15F, .y = 0.06F},
        .white = {.x = 0.3127F, .y = 0.329F},
    };
  }

  wlr_color_primaries displayP3Primaries() {
    return {
        .red = {.x = 0.68F, .y = 0.32F},
        .green = {.x = 0.265F, .y = 0.69F},
        .blue = {.x = 0.15F, .y = 0.06F},
        .white = {.x = 0.3127F, .y = 0.329F},
    };
  }

  wlr_color_primaries bt2020Primaries() {
    return {
        .red = {.x = 0.708F, .y = 0.292F},
        .green = {.x = 0.170F, .y = 0.797F},
        .blue = {.x = 0.131F, .y = 0.046F},
        .white = {.x = 0.3127F, .y = 0.329F},
    };
  }

  // EDID base block with gamma 2.2 and D65 chromaticity (spec 3.7: two
  // low-bits blobs at bytes 25-26, then the 8 MSB bytes).
  std::array<uint8_t, 128> edidWithD65AndGamma22() {
    std::array<uint8_t, 128> edid{};
    edid[0] = 0x00;
    edid[1] = 0xFF;
    edid[2] = 0xFF;
    edid[3] = 0xFF;
    edid[4] = 0xFF;
    edid[5] = 0xFF;
    edid[6] = 0xFF;
    edid[7] = 0x00;
    edid[18] = 0x01;
    edid[19] = 0x04;
    edid[23] = 120; // gamma = 2.2
    // 10-bit fixed point: value = coordinate * 1024.
    const std::array<std::array<uint16_t, 2>, 4> xy{{
        {655, 338}, // red (0.64, 0.33)
        {307, 614}, // green (0.30, 0.60)
        {154, 61},  // blue (0.15, 0.06)
        {320, 337}, // white (D65 at EDID precision)
    }};
    edid[25] =
        static_cast<uint8_t>(((xy[0][0] & 3) << 6) | ((xy[0][1] & 3) << 4) | ((xy[1][0] & 3) << 2) | (xy[1][1] & 3));
    edid[26] =
        static_cast<uint8_t>(((xy[2][0] & 3) << 6) | ((xy[2][1] & 3) << 4) | ((xy[3][0] & 3) << 2) | (xy[3][1] & 3));
    edid[27] = static_cast<uint8_t>(xy[0][0] >> 2);
    edid[28] = static_cast<uint8_t>(xy[0][1] >> 2);
    edid[29] = static_cast<uint8_t>(xy[1][0] >> 2);
    edid[30] = static_cast<uint8_t>(xy[1][1] >> 2);
    edid[31] = static_cast<uint8_t>(xy[2][0] >> 2);
    edid[32] = static_cast<uint8_t>(xy[2][1] >> 2);
    edid[33] = static_cast<uint8_t>(xy[3][0] >> 2);
    edid[34] = static_cast<uint8_t>(xy[3][1] >> 2);
    uint8_t checksum = 0;
    for (size_t index = 0; index < 127; ++index) {
      checksum = static_cast<uint8_t>(checksum + edid[index]);
    }
    edid[127] = static_cast<uint8_t>(0 - checksum);
    return edid;
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

  // Matrix-shaper RGB display profile fixture generator.
  std::vector<uint8_t> matrixShaperProfile(
      const cmsCIExyY& white, const cmsCIExyYTRIPLE& primaries, const std::array<cmsToneCurve*, 3>& curves
  ) {
    cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves.data());
    std::vector<uint8_t> bytes;
    if (profile != nullptr) {
      bytes = saveProfile(profile);
      cmsCloseProfile(profile);
    }
    return bytes;
  }

  cmsToneCurve* srgbDecodeCurve() {
    // lcms2 parametric type 4 = ICC parametricCurveType format 3 (the sRGB
    // EOTF): Y = (aX + b)^g above d, else cX.
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    return cmsBuildParametricToneCurve(nullptr, 4, params);
  }

  // Gamma 2.4 is deliberately not a named fast path, so it exercises the
  // sampled-curve baking.
  cmsToneCurve* tabulatedGamma24Curve() {
    constexpr size_t size = 256;
    std::vector<uint16_t> values(size);
    for (size_t index = 0; index < size; ++index) {
      const double encoded = static_cast<double>(index) / (size - 1);
      values[index] = static_cast<uint16_t>(std::lround(std::pow(encoded, 2.4) * 65535.0));
    }
    return cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(size), values.data());
  }

  cmsCIExyY xyY(double x, double y) { return {.x = x, .y = y, .Y = 1.0}; }
#endif

  // --- ICC byte surgery: lcms-written profiles patched into the exact
  // tag-table and curve layouts real-world profiles carry (v2 headers, shared
  // TRC tags, measured wtpt with or without chad).

  uint32_t readU32Be(const std::vector<uint8_t>& bytes, size_t at) {
    return (uint32_t{bytes[at]} << 24)
        | (uint32_t{bytes[at + 1]} << 16)
        | (uint32_t{bytes[at + 2]} << 8)
        | uint32_t{bytes[at + 3]};
  }

  void writeU32Be(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value >> 24);
    bytes[at + 1] = static_cast<uint8_t>(value >> 16);
    bytes[at + 2] = static_cast<uint8_t>(value >> 8);
    bytes[at + 3] = static_cast<uint8_t>(value);
  }

  void writeU16Be(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value >> 8);
    bytes[at + 1] = static_cast<uint8_t>(value);
  }

  void writeS15Fixed16Be(std::vector<uint8_t>& bytes, size_t at, double value) {
    writeU32Be(bytes, at, static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * 65536.0))));
  }

  struct TagSpan {
    size_t entryOffset;
    size_t dataOffset;
    size_t dataSize;
  };

  std::optional<TagSpan> findTag(const std::vector<uint8_t>& bytes, const std::array<uint8_t, 4>& signature) {
    if (bytes.size() < 132) {
      return std::nullopt;
    }
    const uint32_t count = readU32Be(bytes, 128);
    for (uint32_t index = 0; index < count; ++index) {
      const size_t entry = 132 + 12 * index;
      if (entry + 12 > bytes.size()) {
        return std::nullopt;
      }
      if (std::equal(signature.begin(), signature.end(), bytes.begin() + static_cast<std::ptrdiff_t>(entry))) {
        return TagSpan{entry, readU32Be(bytes, entry + 4), readU32Be(bytes, entry + 8)};
      }
    }
    return std::nullopt;
  }

  // Swap-remove keeps every tag data offset valid; the dropped tag's bytes
  // stay in the file but no table entry references them.
  void stripTag(std::vector<uint8_t>& bytes, const std::array<uint8_t, 4>& signature) {
    const TagSpan tag = *findTag(bytes, signature);
    const uint32_t count = readU32Be(bytes, 128);
    const size_t lastEntry = 132 + 12 * (count - 1);
    std::copy(
        bytes.begin() + static_cast<std::ptrdiff_t>(lastEntry),
        bytes.begin() + static_cast<std::ptrdiff_t>(lastEntry + 12),
        bytes.begin() + static_cast<std::ptrdiff_t>(tag.entryOffset)
    );
    writeU32Be(bytes, 128, count - 1);
  }

  void writeXyzTagBody(std::vector<uint8_t>& bytes, size_t dataOffset, double x, double y, double z) {
    writeU32Be(bytes, dataOffset, 0x58595A20); // 'XYZ '
    writeU32Be(bytes, dataOffset + 4, 0);
    writeS15Fixed16Be(bytes, dataOffset + 8, x);
    writeS15Fixed16Be(bytes, dataOffset + 12, y);
    writeS15Fixed16Be(bytes, dataOffset + 16, z);
  }

  void patchWtptTo(std::vector<uint8_t>& bytes, double whiteX, double whiteY) {
    const TagSpan tag = *findTag(bytes, {'w', 't', 'p', 't'});
    writeXyzTagBody(bytes, tag.dataOffset, whiteX / whiteY, 1.0, (1.0 - whiteX - whiteY) / whiteY);
  }

  void patchColorantTo(std::vector<uint8_t>& bytes, const std::array<uint8_t, 4>& signature, double x, double y) {
    const TagSpan tag = *findTag(bytes, signature);
    writeXyzTagBody(bytes, tag.dataOffset, x / y, 1.0, (1.0 - x - y) / y);
  }

  void patchVersionMajor(std::vector<uint8_t>& bytes, uint8_t major) { bytes[8] = major; }

  // The measured colorimetry fixtures are restored to: P3-style primaries on
  // a D55 white, values independent of any parser math.
  constexpr double kMeasuredRedX = 0.68;
  constexpr double kMeasuredRedY = 0.32;
  constexpr double kMeasuredGreenX = 0.265;
  constexpr double kMeasuredGreenY = 0.69;
  constexpr double kMeasuredBlueX = 0.15;
  constexpr double kMeasuredBlueY = 0.06;
  constexpr double kMeasuredWhiteX = 0.3324;
  constexpr double kMeasuredWhiteY = 0.3474;

  std::array<float, 3> applyMatrix(const float matrix[9], const std::array<float, 3>& in) {
    return {
        matrix[0] * in[0] + matrix[1] * in[1] + matrix[2] * in[2],
        matrix[3] * in[0] + matrix[4] * in[1] + matrix[5] * in[2],
        matrix[6] * in[0] + matrix[7] * in[1] + matrix[8] * in[2],
    };
  }

  // Sampled encode tables follow the documented contract in colorimetry.h:
  // value i sits at the linear input (i / (size - 1))².
  template <typename Encode> std::vector<float> sampledCurve(size_t size, Encode encode) {
    std::vector<float> values(size);
    for (size_t index = 0; index < size; ++index) {
      const float t = static_cast<float>(index) / static_cast<float>(size - 1);
      values[index] = encode(t * t);
    }
    return values;
  }

  // The sampled encode contract pins the table to the generalized inverse of
  // the monotone envelope of the TRC: the first encoded position whose
  // envelope reaches the target. Where the probe's own input cell straddles a
  // flat run of the envelope the inverse jumps inside the cell and the jump's
  // position is not recoverable from the two table samples that bracket it, so
  // a linear lookup can only blend across it. The pin therefore has three
  // parts: no undershoot when decoded back, the answer stays inside the
  // cell's image under the inverse (never leaves the two branch crossings),
  // and the exact first crossing wherever that image is narrow enough for the
  // exactness tolerance to mean anything.
  void expectGeneralizedInverse(const umbriel::PanelTransfer& transfer, const std::vector<float>& envelope) {
    const auto envelopeScale = static_cast<float>(envelope.size() - 1);
    const auto envelopeAt = [&](float encoded) {
      const float position = std::clamp(encoded, 0.0F, 1.0F) * envelopeScale;
      const auto lower = static_cast<size_t>(position);
      const size_t upper = std::min(lower + 1, envelope.size() - 1);
      const float fraction = position - static_cast<float>(lower);
      return envelope[lower] + (envelope[upper] - envelope[lower]) * fraction;
    };
    const auto crossingOf = [&](float target) {
      return static_cast<float>(std::ranges::lower_bound(envelope, target) - envelope.begin()) / envelopeScale;
    };
    const auto tableScale = static_cast<float>(transfer.encode.size() - 1);
    // A curve whose decode does not reach zero makes targets below its floor
    // unrenderable: the generalized inverse is the clamped position zero and
    // decoding back lands on the floor, above the probe cell.
    const float decodeFloor = envelope.front();
    for (int probe = 1; probe < 256; ++probe) {
      const float target = static_cast<float>(probe) / 256.0F;
      if (target > envelope.back()) {
        break;
      }
      const float encoded = umbriel::panelEncodeSample(transfer, target);
      const float position = std::sqrt(target) * tableScale;
      const float cellLow = std::pow(std::floor(position) / tableScale, 2.0F);
      const float cellHigh = std::pow(std::ceil(position) / tableScale, 2.0F);
      const float imageLow = crossingOf(cellLow);
      const float imageHigh = crossingOf(cellHigh);
      const float decoded = envelopeAt(encoded);
      if (decoded + 1e-3F < target || decoded > std::max(cellHigh, decodeFloor) + 1e-3F) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format(
                "encode({}) = {}, decoding back to {} leaves the probe cell [{}, {}]", target, encoded, decoded,
                cellLow, cellHigh
            )
        );
      }
      if (encoded < std::min(imageLow, imageHigh) - 0.008F || encoded > std::max(imageLow, imageHigh) + 0.008F) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format(
                "encode({}) = {}, the inverse image of [{}, {}] is [{}, {}]", target, encoded, cellLow, cellHigh,
                imageLow, imageHigh
            )
        );
      }
      if (imageHigh - imageLow <= 0.016F && std::abs(encoded - crossingOf(target)) > 0.008F) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format("encode({}) = {}, envelope reaches the target at {}", target, encoded, crossingOf(target))
        );
      }
    }
  }

} // namespace

UMBRIEL_TEST(sdrGamutMatrixPinsSrgbToDisplayP3AndBt2020) {
  // Published constant matrices (both sets are D65, so absolute = relative here).
  constexpr std::array<float, 9> expectP3{
      0.8225F, 0.1775F, 0.0000F, 0.0332F, 0.9668F, 0.0000F, 0.0171F, 0.0724F, 0.9105F,
  };
  constexpr std::array<float, 9> expectBt2020{
      0.6274F, 0.3293F, 0.0433F, 0.0691F, 0.9195F, 0.0114F, 0.0164F, 0.0880F, 0.8956F,
  };
  float matrix[9];
  umbriel::sdrGamutMatrix(displayP3Primaries(), 0.0F, matrix);
  for (size_t index = 0; index < 9; ++index) {
    if (!closeTo(matrix[index], expectP3[index])) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__,
          std::format("sRGB->P3 matrix[{}] = {}, expected {}", index, matrix[index], expectP3[index])
      );
    }
  }
  umbriel::sdrGamutMatrix(bt2020Primaries(), 0.0F, matrix);
  for (size_t index = 0; index < 9; ++index) {
    if (!closeTo(matrix[index], expectBt2020[index])) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__,
          std::format("sRGB->BT.2020 matrix[{}] = {}, expected {}", index, matrix[index], expectBt2020[index])
      );
    }
  }
}

UMBRIEL_TEST(sdrGamutMatrixNativeWidenessIsIdentity) {
  float matrix[9];
  umbriel::sdrGamutMatrix(displayP3Primaries(), 1.0F, matrix);
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 3; ++column) {
      const float expected = row == column ? 1.0F : 0.0F;
      if (!closeTo(matrix[row * 3 + column], expected, 1e-4)) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format("native wideness matrix[{}][{}] = {}", row, column, matrix[row * 3 + column])
        );
      }
    }
  }
}

UMBRIEL_TEST(trcEncodeBakingSeparatesGamma22FromSrgb) {
  const umbriel::PanelTransfer gamma22{.kind = umbriel::PanelTransfer::Kind::Gamma22, .encode = {}};
  const umbriel::PanelTransfer srgb{.kind = umbriel::PanelTransfer::Kind::Srgb, .encode = {}};

  CHECK_EQ(umbriel::panelEncodeSample(gamma22, 0.0F), 0.0F);
  CHECK_EQ(umbriel::panelEncodeSample(gamma22, 1.0F), 1.0F);
  CHECK(closeTo(umbriel::panelEncodeSample(gamma22, 0.5F), std::pow(0.5, 1.0 / 2.2), 1e-4));

  CHECK_EQ(umbriel::panelEncodeSample(srgb, 0.0F), 0.0F);
  CHECK_EQ(umbriel::panelEncodeSample(srgb, 1.0F), 1.0F);
  CHECK(closeTo(umbriel::panelEncodeSample(srgb, 0.5F), 1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055, 1e-4));

  // The two curves must differ where the sRGB toe lives.
  CHECK(std::abs(umbriel::panelEncodeSample(gamma22, 0.02F) - umbriel::panelEncodeSample(srgb, 0.02F)) > 1e-3);

  // Endpoints stay anchored through the transform pipeline so baked ramps
  // begin at 0 and end at 1 for every encode kind.
  for (const umbriel::PanelTransfer* transfer : {&gamma22, &srgb}) {
    umbriel::PanelColorimetry panel{
        .primaries = srgbPrimaries(),
        .white = {0.3127F, 0.329F},
        .transfer = *transfer,
    };
    wlr_color_transform* transform = umbriel::buildSdrGamutTransform(panel);
    CHECK(transform != nullptr);
    if (transform == nullptr) {
      continue;
    }
    float black[3]{};
    float white[3]{};
    const float inBlack[3]{0.0F, 0.0F, 0.0F};
    const float inWhite[3]{1.0F, 1.0F, 1.0F};
    wlr_color_transform_eval(transform, black, inBlack);
    wlr_color_transform_eval(transform, white, inWhite);
    CHECK(closeTo(black[0], 0.0, 1e-4));
    CHECK(closeTo(white[0], 1.0, 1e-4));
    wlr_color_transform_unref(transform);
  }
}

UMBRIEL_TEST(sdrGamutStagingDecisionPreservesNativePixels) {
  using umbriel::SdrGamut;
  // No characterization: nothing to map.
  CHECK(!umbriel::sdrGamutStaged(false, SdrGamut::Srgb, false));
  // "native" stages NO transform at all: pixels stay bit-identical.
  CHECK(!umbriel::sdrGamutStaged(true, SdrGamut::Native, false));
  CHECK(umbriel::sdrGamutStaged(true, SdrGamut::Srgb, false));
  // An active HDR image description owns the output transform.
  CHECK(!umbriel::sdrGamutStaged(true, SdrGamut::Srgb, true));
}

UMBRIEL_TEST(sdrGamutTransformBakesPanelEncodeCurve) {
  // A sampled encode curve runs through the LUT stage, which clamps negatives
  // the float matrix can leave behind, so the eval pins are exact here.
  umbriel::PanelColorimetry panel{
      .primaries = srgbPrimaries(),
      .white = {0.3127F, 0.329F},
      .transfer = {.kind = umbriel::PanelTransfer::Kind::Curve, .encode = {}},
  };
  // A gamma 2.2 encode curve, sampled on the documented encode grid the way
  // the ICC parser bakes one (colorimetry.h: value i sits at input (i/(n-1))²).
  panel.transfer.encode = sampledCurve(1024, [](float linear) { return std::pow(linear, 1.0F / 2.2F); });
  wlr_color_transform* transform = umbriel::buildSdrGamutTransform(panel);
  CHECK(transform != nullptr);
  if (transform == nullptr) {
    return;
  }
  float output[3]{};
  const float input[3]{0.5F, 0.0F, 0.0F};
  wlr_color_transform_eval(transform, output, input);
  CHECK(closeTo(output[0], std::pow(0.5, 1.0 / 2.2), 1e-3));
  CHECK(closeTo(output[1], 0.0, 1e-4));
  CHECK(closeTo(output[2], 0.0, 1e-4));
  wlr_color_transform_unref(transform);
}

UMBRIEL_TEST(edidColorimetryUsesColorantsGammaAndWhite) {
  const auto edid = edidWithD65AndGamma22();
  const wlr_color_primaries panel = displayP3Primaries();
  const std::optional<umbriel::PanelColorimetry> parsed = umbriel::panelColorimetryFromEdid(edid, &panel);
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }
  CHECK(closeTo(parsed->primaries.red.x, 0.68));
  CHECK(closeTo(parsed->primaries.green.y, 0.69));
  CHECK(closeTo(parsed->white[0], 0.3127, 0.005));
  CHECK(closeTo(parsed->white[1], 0.329, 0.005));
  CHECK(parsed->transfer.kind == umbriel::PanelTransfer::Kind::Gamma22);
}

UMBRIEL_TEST(edidColorimetryStaysInertWithoutEdidOrPrimaries) {
  CHECK(!umbriel::panelColorimetryFromEdid({}, nullptr).has_value());
  const auto edid = edidWithD65AndGamma22();
  const wlr_color_primaries srgb = srgbPrimaries();
  CHECK(!umbriel::panelColorimetryFromEdid({}, &srgb).has_value());
  CHECK(!umbriel::panelColorimetryFromEdid(edid, nullptr).has_value());
}

UMBRIEL_TEST(iccMatrixShaperRoundTripsColorantsWhiteAndTrc) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.error.empty());
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelColorimetry& panel = *parsed.colorimetry;
  CHECK(closeTo(panel.primaries.red.x, 0.64));
  CHECK(closeTo(panel.primaries.green.y, 0.60));
  CHECK(closeTo(panel.primaries.blue.x, 0.15));
  CHECK(closeTo(panel.white[0], 0.3127, 0.005));
  CHECK(closeTo(panel.white[1], 0.329, 0.005));
  CHECK(panel.transfer.kind == umbriel::PanelTransfer::Kind::Gamma22);
#endif
}

UMBRIEL_TEST(iccChadTagRecoversMeasuredWhiteAndColorants) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // A non-D50 white forces lcms2 to adapt the colorants and record chad; the
  // parser must undo that adaptation to recover the measured colorimetry.
  std::array<cmsToneCurve*, 3> curves{srgbDecodeCurve(), srgbDecodeCurve(), srgbDecodeCurve()};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3324, 0.3474), // D55
      {.Red = xyY(0.68, 0.32), .Green = xyY(0.265, 0.69), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelColorimetry& panel = *parsed.colorimetry;
  CHECK(closeTo(panel.primaries.red.x, 0.68, 0.005));
  CHECK(closeTo(panel.primaries.green.y, 0.69, 0.005));
  CHECK(closeTo(panel.primaries.blue.y, 0.06, 0.005));
  CHECK(closeTo(panel.white[0], 0.3324, 0.005));
  CHECK(closeTo(panel.white[1], 0.3474, 0.005));
  CHECK(panel.transfer.kind == umbriel::PanelTransfer::Kind::Srgb);
#endif
}

UMBRIEL_TEST(iccTabulatedTrcBakesEncodeCurve) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{tabulatedGamma24Curve(), tabulatedGamma24Curve(), tabulatedGamma24Curve()};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelTransfer& transfer = parsed.colorimetry->transfer;
  CHECK(transfer.kind == umbriel::PanelTransfer::Kind::Curve);
  if (transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
    return;
  }
  CHECK(transfer.encode.size() > 2);
  CHECK(closeTo(transfer.encode.front(), 0.0, 1e-3));
  CHECK(closeTo(transfer.encode.back(), 1.0, 1e-3));
  // The tabulated TRC is gamma 2.4 evaluated; the baked encode must invert it.
  CHECK(closeTo(umbriel::panelEncodeSample(transfer, 0.5F), std::pow(0.5, 1.0 / 2.4), 0.005));
#endif
}

UMBRIEL_TEST(iccLutProfilesAreRejected) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const cmsCIExyY white = xyY(0.3127, 0.329);
  const cmsCIExyYTRIPLE primaries{
      .Red = xyY(0.64, 0.33),
      .Green = xyY(0.30, 0.60),
      .Blue = xyY(0.15, 0.06),
  };
  cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves.data());
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(profile != nullptr);
  if (profile == nullptr) {
    return;
  }
  // A presence-only probe: lcms2 refuses to serialize ad-hoc pipelines as
  // mAB/mBA, but the parser rejects on the tag's presence alone, so a raw
  // A2B0 tag is exactly what the rejection must key on.
  const std::array<uint8_t, 4> lutTag{0x6D, 0x66, 0x74, 0x32}; // 'mft2'
  CHECK(cmsWriteRawTag(profile, cmsSigAToB0Tag, lutTag.data(), static_cast<cmsUInt32Number>(lutTag.size())));
  const std::vector<uint8_t> bytes = saveProfile(profile);
  cmsCloseProfile(profile);

  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(!parsed.colorimetry.has_value());
  CHECK(parsed.error.contains("LUT"));
#endif
}

UMBRIEL_TEST(iccVcgtIsIgnoredOnMatrixShaperProfiles) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const cmsCIExyY white = xyY(0.3127, 0.329);
  const cmsCIExyYTRIPLE primaries{
      .Red = xyY(0.64, 0.33),
      .Green = xyY(0.30, 0.60),
      .Blue = xyY(0.15, 0.06),
  };
  cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves.data());
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(profile != nullptr);
  if (profile == nullptr) {
    return;
  }
  // vcgt is a pre-dispatch gamma ramp, not content colorimetry: parse must warn and ignore it.
  const std::array<uint8_t, 4> ramp{0x00, 0x00, 0xFF, 0xFF};
  CHECK(cmsWriteRawTag(profile, cmsSigVcgtTag, ramp.data(), static_cast<cmsUInt32Number>(ramp.size())));
  const std::vector<uint8_t> bytes = saveProfile(profile);
  cmsCloseProfile(profile);

  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  CHECK(std::ranges::any_of(parsed.warnings, [](const std::string& warning) { return warning.contains("vcgt"); }));
#endif
}

UMBRIEL_TEST(iccVcgtOnlyProfilesAreRejected) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  cmsHPROFILE profile = cmsCreateProfilePlaceholder(nullptr);
  CHECK(profile != nullptr);
  if (profile == nullptr) {
    return;
  }
  cmsSetDeviceClass(profile, cmsSigDisplayClass);
  cmsSetColorSpace(profile, cmsSigRgbData);
  cmsSetPCS(profile, cmsSigXYZData);
  const std::array<uint8_t, 4> ramp{0x00, 0x00, 0xFF, 0xFF};
  cmsWriteRawTag(profile, cmsSigVcgtTag, ramp.data(), static_cast<cmsUInt32Number>(ramp.size()));
  const std::vector<uint8_t> bytes = saveProfile(profile);
  cmsCloseProfile(profile);

  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(!parsed.colorimetry.has_value());
  CHECK(!parsed.error.empty());
#endif
}

UMBRIEL_TEST(iccTruncatedProfilesAreRejected) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
  CHECK(!umbriel::parseIccProfile(truncated).colorimetry.has_value());
  CHECK(!umbriel::parseIccProfile({}).colorimetry.has_value());
#endif
}

UMBRIEL_TEST(iccStoreForFileProfileKeepsExactBytes) {
  // The store is the file as written on disk, never a reserialization of what
  // the parser understood: odd padding and trailing garbage survive verbatim.
  const std::vector<uint8_t> bytes{0x00, 0x11, 0x22, 0x33, 0xFF, 0x00, 0x7F, 0x42};
  const umbriel::IccProfileStore store = umbriel::iccStoreForFileProfile(bytes);
  CHECK(store != nullptr);
  if (store == nullptr) {
    return;
  }
  CHECK(*store == bytes);
  CHECK(umbriel::iccStoreForFileProfile({}) == nullptr);
}

UMBRIEL_TEST(inertCharacterizationStoresNoIccProfile) {
  CHECK(umbriel::iccStoreForPanel(nullptr) == nullptr);
#if !UMBRIEL_HAS_LCMS2
  // EDID-without-lcms2 cannot synthesize a profile and stores nothing.
  const umbriel::PanelColorimetry panel{
      .primaries = displayP3Primaries(),
      .white = {0.3127F, 0.329F},
      .transfer = {.kind = umbriel::PanelTransfer::Kind::Gamma22, .encode = {}},
  };
  CHECK(umbriel::iccStoreForPanel(&panel) == nullptr);
#endif
}

UMBRIEL_TEST(synthesizedEdidProfileRoundTripsThroughTheIccParser) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // The advertised bytes must describe the same panel the parser recovered
  // from the EDID: feed the synthesis back through the ICC parser.
  const auto edid = edidWithD65AndGamma22();
  const wlr_color_primaries defaults = displayP3Primaries();
  const std::optional<umbriel::PanelColorimetry> source = umbriel::panelColorimetryFromEdid(edid, &defaults);
  CHECK(source.has_value());
  if (!source) {
    return;
  }
  const umbriel::IccProfileStore store = umbriel::iccStoreForPanel(&*source);
  CHECK(store != nullptr);
  if (store == nullptr || store->empty()) {
    return;
  }
  const umbriel::IccParse parsed = umbriel::parseIccProfile(*store);
  CHECK(parsed.error.empty());
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelColorimetry& recovered = *parsed.colorimetry;
  CHECK(closeTo(recovered.primaries.red.x, source->primaries.red.x, 0.005));
  CHECK(closeTo(recovered.primaries.red.y, source->primaries.red.y, 0.005));
  CHECK(closeTo(recovered.primaries.green.x, source->primaries.green.x, 0.005));
  CHECK(closeTo(recovered.primaries.green.y, source->primaries.green.y, 0.005));
  CHECK(closeTo(recovered.primaries.blue.x, source->primaries.blue.x, 0.005));
  CHECK(closeTo(recovered.primaries.blue.y, source->primaries.blue.y, 0.005));
  CHECK(closeTo(recovered.white[0], source->white[0], 0.005));
  CHECK(closeTo(recovered.white[1], source->white[1], 0.005));
  CHECK(recovered.transfer.kind == source->transfer.kind);
#endif
}

UMBRIEL_TEST(synthesizedCurveTrcRoundTripsThroughTheIccParser) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Gamma 2.4 is not a named fast path: the synthesized profile must carry the
  // sampled decode curve so the parser bakes the same encode back.
  umbriel::PanelColorimetry source{
      .primaries = bt2020Primaries(),
      .white = {0.3127F, 0.329F},
      .transfer = {.kind = umbriel::PanelTransfer::Kind::Curve, .encode = {}},
  };
  source.transfer.encode.resize(1024);
  for (size_t index = 0; index < source.transfer.encode.size(); ++index) {
    const float linear = static_cast<float>(index) / static_cast<float>(source.transfer.encode.size() - 1);
    source.transfer.encode[index] = std::pow(linear, 1.0F / 2.4F);
  }
  const umbriel::IccProfileStore store = umbriel::iccStoreForPanel(&source);
  CHECK(store != nullptr);
  if (store == nullptr || store->empty()) {
    return;
  }
  const umbriel::IccParse parsed = umbriel::parseIccProfile(*store);
  CHECK(parsed.error.empty());
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelColorimetry& recovered = *parsed.colorimetry;
  CHECK(closeTo(recovered.primaries.red.x, source.primaries.red.x, 0.005));
  CHECK(closeTo(recovered.primaries.green.y, source.primaries.green.y, 0.005));
  CHECK(recovered.transfer.kind == umbriel::PanelTransfer::Kind::Curve);
  if (recovered.transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
    return;
  }
  for (const float at : {0.25F, 0.5F, 0.75F}) {
    CHECK(closeTo(
        umbriel::panelEncodeSample(recovered.transfer, at), umbriel::panelEncodeSample(source.transfer, at), 0.01
    ));
  }
#endif
}

UMBRIEL_TEST(fitTransferFitsNamedFastPathsAndPowerCurves) {
  // Exact named shapes stay named; only genuinely non-representable TRCs fall
  // back to the parametric power fit (fitTransfer's approximation contract).
  // Sampled fixtures follow the documented encode-table contract: value i sits
  // at the linear input (i / (size - 1))².
  auto srgbEncode = [](float linear) -> float {
    return linear >= 1.0F ? 1.0F
                          : (linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F);
  };

  CHECK(
      umbriel::fitTransfer({.kind = umbriel::PanelTransfer::Kind::Srgb, .encode = {}}).kind
      == umbriel::TransferFit::Kind::Srgb
  );
  CHECK(
      umbriel::fitTransfer({.kind = umbriel::PanelTransfer::Kind::Gamma22, .encode = {}}).kind
      == umbriel::TransferFit::Kind::Gamma22
  );

  // Sampled curves re-detect the named shapes before fitting anything.
  const umbriel::TransferFit sampledSrgb =
      umbriel::fitTransfer({.kind = umbriel::PanelTransfer::Kind::Curve, .encode = sampledCurve(1024, srgbEncode)});
  CHECK(sampledSrgb.kind == umbriel::TransferFit::Kind::Srgb);
  const umbriel::TransferFit sampledGamma22 = umbriel::fitTransfer(
      {.kind = umbriel::PanelTransfer::Kind::Curve,
       .encode = sampledCurve(1024, [](float x) -> float { return std::pow(x, 1.0F / 2.2F); })}
  );
  CHECK(sampledGamma22.kind == umbriel::TransferFit::Kind::Gamma22);

  // Odd curves fit to their power exponent within tolerance.
  for (const float exponent : {1.8F, 2.4F, 2.8F}) {
    const float shape = exponent;
    const umbriel::TransferFit fit = umbriel::fitTransfer(
        {.kind = umbriel::PanelTransfer::Kind::Curve,
         .encode = sampledCurve(1024, [shape](float x) -> float { return std::pow(x, 1.0F / shape); })}
    );
    CHECK(fit.kind == umbriel::TransferFit::Kind::Power);
    CHECK(closeTo(fit.exponent, exponent, 0.01));
  }

  // An empty sampled table is the identity encode, i.e. linear.
  const umbriel::TransferFit linear = umbriel::fitTransfer({.kind = umbriel::PanelTransfer::Kind::Curve, .encode = {}});
  CHECK(linear.kind == umbriel::TransferFit::Kind::Power);
  CHECK(closeTo(linear.exponent, 1.0));
}

UMBRIEL_TEST(iccClassScopeAdmitsColorSpaceProfilesForClientIcc) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // set_icc_file admits Display *and* ColorSpace class profiles; output
  // characterization keeps its display-only vocabulary.
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  // Header bytes 12-15 carry the ICC device class signature.
  const auto setClass = [&bytes](char first, char second, char third, char fourth) {
    bytes[12] = static_cast<uint8_t>(first);
    bytes[13] = static_cast<uint8_t>(second);
    bytes[14] = static_cast<uint8_t>(third);
    bytes[15] = static_cast<uint8_t>(fourth);
  };

  setClass('s', 'p', 'a', 'c'); // ColorSpace class
  CHECK(!umbriel::parseIccProfile(bytes).colorimetry.has_value());
  CHECK(umbriel::parseIccProfile(bytes, umbriel::IccClassScope::DisplayOrColorSpace).colorimetry.has_value());

  setClass('p', 'r', 't', 'r'); // Printer class: out of scope everywhere
  CHECK(!umbriel::parseIccProfile(bytes).colorimetry.has_value());
  CHECK(!umbriel::parseIccProfile(bytes, umbriel::IccClassScope::DisplayOrColorSpace).colorimetry.has_value());
#endif
}

UMBRIEL_TEST(wlrMatrixStageConsumesRowMajorCoefficients) {
  // The matrix contract must match how the wlroots matrix stage evaluates:
  // published sRGB->BT.2020 row-major coefficients map each basis vector to
  // its published column through wlr_color_transform_init_matrix.
  float matrix[9];
  umbriel::sdrGamutMatrix(bt2020Primaries(), 0.0F, matrix);
  wlr_color_transform* stage = wlr_color_transform_init_matrix(matrix);
  CHECK(stage != nullptr);
  if (stage == nullptr) {
    return;
  }
  const std::array<std::pair<std::array<float, 3>, std::array<float, 3>>, 3> basis{{
      {{1.0F, 0.0F, 0.0F}, {0.6274F, 0.0691F, 0.0164F}},
      {{0.0F, 1.0F, 0.0F}, {0.3293F, 0.9195F, 0.0880F}},
      {{0.0F, 0.0F, 1.0F}, {0.0433F, 0.0114F, 0.8956F}},
  }};
  for (const auto& [input, expected] : basis) {
    float output[3]{};
    wlr_color_transform_eval(stage, output, input.data());
    for (size_t channel = 0; channel < 3; ++channel) {
      if (!closeTo(output[channel], expected[channel])) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format("matrix stage channel {} = {}, expected {}", channel, output[channel], expected[channel])
        );
      }
    }
  }
  wlr_color_transform_unref(stage);
}

UMBRIEL_TEST(sdrGamutMatrixPreservesSourceWhiteOnNonD65Panels) {
  // White-preserving mapping: (1,1,1) is the source white and must land on the
  // panel white exactly, whatever chromaticity the panel declares. Only chroma
  // is confined to the sRGB gamut.
  wlr_color_primaries panel = displayP3Primaries();
  panel.white = {.x = 0.3457F, .y = 0.3585F}; // D50-declaring panel
  const std::array<float, 3> white{1.0F, 1.0F, 1.0F};
  float matrix[9];
  for (const float amount : {0.25F, 0.5F, 0.75F}) {
    umbriel::sdrGamutMatrix(panel, amount, matrix);
    const std::array<float, 3> mapped = applyMatrix(matrix, white);
    for (size_t channel = 0; channel < 3; ++channel) {
      if (std::abs(mapped[channel] - 1.0F) > 1e-4F) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format("wideness {} maps source white channel {} to {}, expected 1", amount, channel, mapped[channel])
        );
      }
    }
  }
  umbriel::sdrGamutMatrix(panel, 0.0F, matrix);
  const std::array<float, 3> mapped = applyMatrix(matrix, white);
  for (size_t channel = 0; channel < 3; ++channel) {
    if (std::abs(mapped[channel] - 1.0F) > 1e-4F) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__,
          std::format("strict srgb maps source white channel {} to {}, expected 1", channel, mapped[channel])
      );
    }
  }
  // White must not be clipped either: no channel exceeds 1 on a non-D65 panel.
  CHECK(mapped[0] <= 1.0001F && mapped[1] <= 1.0001F && mapped[2] <= 1.0001F);
}

UMBRIEL_TEST(sdrGamutTransformKeepsWhiteIdenticalToNativePixels) {
  // Switching native <-> srgb must never change how white renders: the
  // transform maps (1,1,1) to (1,1,1) for every panel and every encode kind.
  const std::array<float, 3> white{1.0F, 1.0F, 1.0F};
  wlr_color_primaries panel = displayP3Primaries();
  panel.white = {.x = 0.3127F, .y = 0.329F};
  const umbriel::PanelTransfer transfers[]{
      {.kind = umbriel::PanelTransfer::Kind::Srgb, .encode = {}},
      {.kind = umbriel::PanelTransfer::Kind::Gamma22, .encode = {}},
      {.kind = umbriel::PanelTransfer::Kind::Curve, .encode = {0.0F, 1.0F}},
  };
  for (const auto& whitePoint :
       {std::pair{0.3127F, 0.329F}, std::pair{0.3457F, 0.3585F}, std::pair{0.3324F, 0.3474F}}) {
    panel.white = {.x = whitePoint.first, .y = whitePoint.second};
    for (const umbriel::PanelTransfer& transfer : transfers) {
      umbriel::PanelColorimetry colorimetry{
          .primaries = panel, .white = {panel.white.x, panel.white.y}, .transfer = transfer
      };
      wlr_color_transform* transform = umbriel::buildSdrGamutTransform(colorimetry);
      CHECK(transform != nullptr);
      if (transform == nullptr) {
        continue;
      }
      float output[3]{};
      wlr_color_transform_eval(transform, output, white.data());
      for (size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(output[channel] - 1.0F) > 1e-3F) {
          umbriel::test::reportFailure(
              __FILE__, __LINE__,
              std::format(
                  "white at ({},{}) kind {} maps channel {} to {}, native passes 1", whitePoint.first,
                  whitePoint.second, static_cast<int>(transfer.kind), channel, output[channel]
              )
          );
        }
      }
      wlr_color_transform_unref(transform);
    }
  }
}

#if UMBRIEL_HAS_LCMS2

// A v2 profile with no chad tag and a measured (non-D50) white point, the
// layout v2 measurement suites emit: colorants D50-relative, wtpt measured.
std::vector<uint8_t> v2NoChadMeasuredWhiteProfile() {
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(kMeasuredWhiteX, kMeasuredWhiteY),
      {
          .Red = xyY(kMeasuredRedX, kMeasuredRedY),
          .Green = xyY(kMeasuredGreenX, kMeasuredGreenY),
          .Blue = xyY(kMeasuredBlueX, kMeasuredBlueY),
      },
      curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  if (bytes.empty()) {
    return bytes;
  }
  stripTag(bytes, {'c', 'h', 'a', 'd'});
  patchVersionMajor(bytes, 2);
  patchWtptTo(bytes, kMeasuredWhiteX, kMeasuredWhiteY);
  return bytes;
}

std::vector<uint8_t> chadProfileWithWtpt(double wtptX, double wtptY) {
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(kMeasuredWhiteX, kMeasuredWhiteY),
      {
          .Red = xyY(kMeasuredRedX, kMeasuredRedY),
          .Green = xyY(kMeasuredGreenX, kMeasuredGreenY),
          .Blue = xyY(kMeasuredBlueX, kMeasuredBlueY),
      },
      curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  if (!bytes.empty()) {
    patchWtptTo(bytes, wtptX, wtptY);
  }
  return bytes;
}

void checkMeasuredColorimetry(const char* what, const umbriel::PanelColorimetry& panel) {
  const auto expect = [&](double actual, double expected, const char* field) {
    if (std::abs(actual - expected) > 0.005) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("{}: {} = {}, expected {}", what, field, actual, expected)
      );
    }
  };
  expect(panel.primaries.red.x, kMeasuredRedX, "red.x");
  expect(panel.primaries.red.y, kMeasuredRedY, "red.y");
  expect(panel.primaries.green.x, kMeasuredGreenX, "green.x");
  expect(panel.primaries.green.y, kMeasuredGreenY, "green.y");
  expect(panel.primaries.blue.x, kMeasuredBlueX, "blue.x");
  expect(panel.primaries.blue.y, kMeasuredBlueY, "blue.y");
  expect(panel.white[0], kMeasuredWhiteX, "white.x");
  expect(panel.white[1], kMeasuredWhiteY, "white.y");
}

#endif

UMBRIEL_TEST(iccV2NoChadMeasuredWhiteRecoversMeasuredColorimetry) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Without a chad tag the colorants are D50-relative; the profile's measured
  // white declares the real frame. White and colorants must end up in one
  // frame so the recovered values are the measured ones.
  const std::vector<uint8_t> bytes = v2NoChadMeasuredWhiteProfile();
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.error.empty());
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  checkMeasuredColorimetry("v2 no-chad", *parsed.colorimetry);
  CHECK(parsed.colorimetry->transfer.kind == umbriel::PanelTransfer::Kind::Gamma22);

  // The measured white renders as source white: (1,1,1) maps to (1,1,1).
  float matrix[9];
  umbriel::sdrGamutMatrix(parsed.colorimetry->primaries, 0.0F, matrix);
  const std::array<float, 3> mapped = applyMatrix(matrix, {1.0F, 1.0F, 1.0F});
  for (size_t channel = 0; channel < 3; ++channel) {
    if (std::abs(mapped[channel] - 1.0F) > 1e-3F) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("v2 no-chad: source white channel {} maps to {}", channel, mapped[channel])
      );
    }
  }
#endif
}

UMBRIEL_TEST(iccChadWithMeasuredWhiteIsNotAdaptedTwice) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // chad records the adaptation applied on the way to the D50 PCS; when wtpt
  // already carries the measured white, un-adapting it again must not move it.
  const std::vector<uint8_t> bytes = chadProfileWithWtpt(kMeasuredWhiteX, kMeasuredWhiteY);
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.error.empty());
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  checkMeasuredColorimetry("chad + measured wtpt", *parsed.colorimetry);
#endif
}

UMBRIEL_TEST(iccNoChadD50WhiteKeepsTheD50Frame) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // A D50 white with no chad is self-consistently D50: nothing to adapt, and
  // the tagged colorants pass through unchanged.
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3457, 0.3585), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  stripTag(bytes, {'c', 'h', 'a', 'd'});
  patchVersionMajor(bytes, 2);
  patchWtptTo(bytes, 0.3457, 0.3585);
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelColorimetry& panel = *parsed.colorimetry;
  CHECK(closeTo(panel.primaries.red.x, 0.64, 0.005));
  CHECK(closeTo(panel.primaries.green.y, 0.60, 0.005));
  CHECK(closeTo(panel.primaries.blue.y, 0.06, 0.005));
  CHECK(closeTo(panel.white[0], 0.3457, 0.005));
  CHECK(closeTo(panel.white[1], 0.3585, 0.005));
#endif
}

UMBRIEL_TEST(iccTabulatedCurveKeepsShadowAccuracyForGamma24) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // The sampled encode curve must track x^(1/2.4) to well under one 8-bit
  // code everywhere, especially in the near-vertical shadow region.
  std::array<cmsToneCurve*, 3> curves{tabulatedGamma24Curve(), tabulatedGamma24Curve(), tabulatedGamma24Curve()};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry || parsed.colorimetry->transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
    return;
  }
  const umbriel::PanelTransfer& transfer = parsed.colorimetry->transfer;
  double worst = 0.0;
  for (int probe = 0; probe <= 40000; ++probe) {
    // Geometric probes reach the shadow region; uniform probes cover the rest.
    const double linear = probe <= 20000 ? std::pow(2.0, -probe / 2000.0) : (probe - 20000) / 20000.0;
    const double error =
        std::abs(umbriel::panelEncodeSample(transfer, static_cast<float>(linear)) - std::pow(linear, 1.0 / 2.4))
        * 255.0;
    worst = std::max(worst, error);
  }
  std::println("gamma2.4 sampled encode worst error: {:.3f} 8-bit codes", worst);
  CHECK(worst < 0.6);

  // End to end through the staged transform the renderer consumes.
  umbriel::PanelColorimetry panel{
      .primaries = srgbPrimaries(),
      .white = {0.3127F, 0.329F},
      .transfer = transfer,
  };
  wlr_color_transform* transform = umbriel::buildSdrGamutTransform(panel);
  CHECK(transform != nullptr);
  if (transform == nullptr) {
    return;
  }
  double worstEndToEnd = 0.0;
  for (int probe = 0; probe <= 40000; ++probe) {
    const double linear = probe <= 20000 ? std::pow(2.0, -probe / 2000.0) : (probe - 20000) / 20000.0;
    const float input[3]{static_cast<float>(linear), 0.0F, 0.0F};
    float output[3]{};
    wlr_color_transform_eval(transform, output, input);
    const double error = std::abs(output[0] - std::pow(linear, 1.0 / 2.4)) * 255.0;
    worstEndToEnd = std::max(worstEndToEnd, error);
  }
  wlr_color_transform_unref(transform);
  std::println("gamma2.4 end-to-end worst error: {:.3f} 8-bit codes", worstEndToEnd);
  CHECK(worstEndToEnd < 0.6);
#endif
}

UMBRIEL_TEST(edidGamma26CurveKeepsShadowAccuracy) {
  // EDID gamma != 2.2 rides the same sampled-curve path.
  auto edid = edidWithD65AndGamma22();
  edid[23] = 160; // gamma = (160 + 100) / 100 = 2.6
  uint8_t checksum = 0;
  for (size_t index = 0; index < 127; ++index) {
    checksum = static_cast<uint8_t>(checksum + edid[index]);
  }
  edid[127] = static_cast<uint8_t>(0 - checksum);
  const wlr_color_primaries defaults = displayP3Primaries();
  const std::optional<umbriel::PanelColorimetry> parsed = umbriel::panelColorimetryFromEdid(edid, &defaults);
  CHECK(parsed.has_value());
  if (!parsed || parsed->transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
    return;
  }
  double worst = 0.0;
  for (int probe = 0; probe <= 40000; ++probe) {
    const double linear = probe <= 20000 ? std::pow(2.0, -probe / 2000.0) : (probe - 20000) / 20000.0;
    const double error =
        std::abs(umbriel::panelEncodeSample(parsed->transfer, static_cast<float>(linear)) - std::pow(linear, 1.0 / 2.6))
        * 255.0;
    worst = std::max(worst, error);
  }
  std::println("EDID gamma 2.6 sampled encode worst error: {:.3f} 8-bit codes", worst);
  CHECK(worst < 0.6);
}

UMBRIEL_TEST(iccNonMonotonicTrcStaysInvertibleAndBounded) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Untrusted TRCs need not increase at all; the inverted encode curve must
  // still be a monotone, bounded, finite table.
  std::vector<uint16_t> dip{0, 30000, 10000, 40000, 20000, 55000, 50000, 65535};
  cmsToneCurve* curve = cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(dip.size()), dip.data());
  CHECK(curve != nullptr);
  if (curve == nullptr) {
    return;
  }
  std::array<cmsToneCurve*, 3> curves{curve, curve, curve};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  // The monotone envelope of the raw TRC is what the encode curve must invert.
  constexpr size_t kEnvelopeSamples = 1024;
  std::vector<float> envelope(kEnvelopeSamples);
  float runningMax = 0.0F;
  for (size_t index = 0; index < kEnvelopeSamples; ++index) {
    const float encoded = static_cast<float>(index) / static_cast<float>(kEnvelopeSamples - 1);
    runningMax = std::max(runningMax, cmsEvalToneCurveFloat(curve, encoded));
    envelope[index] = runningMax;
  }
  cmsFreeToneCurve(curve);
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelTransfer& transfer = parsed.colorimetry->transfer;
  CHECK(transfer.kind == umbriel::PanelTransfer::Kind::Curve);
  if (transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
    return;
  }
  CHECK(transfer.encode.size() > 1);
  float previous = 0.0F;
  for (size_t index = 0; index < transfer.encode.size(); ++index) {
    const float value = transfer.encode[index];
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
      umbriel::test::reportFailure(__FILE__, __LINE__, std::format("encode[{}] = {} leaves [0,1]", index, value));
    }
    if (value + 1e-6F < previous) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("encode[{}] = {} decreases from {}", index, value, previous)
      );
    }
    previous = value;
  }

  // The encode must reproduce the generalized inverse of the monotone
  // envelope of the TRC (see expectGeneralizedInverse). Inverting raw
  // unordered samples drifts away from that crossing.
  expectGeneralizedInverse(transfer, envelope);
#endif
}

UMBRIEL_TEST(iccOddParametricCurvesStayFiniteMonotoneAndBounded) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Parametric curves with awkward coefficients must not leak non-finite or
  // decreasing samples into the inversion and the fit.
  const std::array<std::array<cmsFloat64Number, 5>, 2> parametric{{
      {1.5, -1.0, 0.5, 1.0, 0.0},
      {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 8.0, 0.005},
  }};
  for (const auto& params : parametric) {
    cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params.data());
    CHECK(curve != nullptr);
    if (curve == nullptr) {
      continue;
    }
    std::array<cmsToneCurve*, 3> curves{curve, curve, curve};
    const std::vector<uint8_t> bytes = matrixShaperProfile(
        xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
    );
    constexpr size_t kEnvelopeSamples = 1024;
    std::vector<float> envelope(kEnvelopeSamples);
    float runningMax = 0.0F;
    for (size_t index = 0; index < kEnvelopeSamples; ++index) {
      const float encoded = static_cast<float>(index) / static_cast<float>(kEnvelopeSamples - 1);
      runningMax = std::max(runningMax, cmsEvalToneCurveFloat(curve, encoded));
      envelope[index] = runningMax;
    }
    cmsFreeToneCurve(curve);
    const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
    CHECK(parsed.colorimetry.has_value());
    if (!parsed.colorimetry || parsed.colorimetry->transfer.kind != umbriel::PanelTransfer::Kind::Curve) {
      continue;
    }
    float previous = 0.0F;
    for (const float value : parsed.colorimetry->transfer.encode) {
      if (!std::isfinite(value) || value < 0.0F || value > 1.0F || value + 1e-6F < previous) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__, std::format("parametric sample {} invalid (previous {})", value, previous)
        );
      }
      previous = value;
    }
    // Same inverse contract as for tabulated TRCs: the encode curve must land
    // on the envelope's generalized-inverse crossing, including when the raw
    // parametric curve decreases.
    expectGeneralizedInverse(parsed.colorimetry->transfer, envelope);
  }
#endif
}

UMBRIEL_TEST(iccPerChannelTrcDifferencesWarnAndUseTheRedCurve) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{cmsBuildGamma(nullptr, 2.2), tabulatedGamma24Curve(), srgbDecodeCurve()};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  CHECK(std::ranges::any_of(parsed.warnings, [](const std::string& warning) { return warning.contains("differ"); }));
  if (parsed.colorimetry) {
    CHECK(parsed.colorimetry->transfer.kind == umbriel::PanelTransfer::Kind::Gamma22);
  }
#endif
}

UMBRIEL_TEST(iccGreenBlueTrcRepairsAreReported) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // A repaired green/blue TRC must surface its warning; it used to be
  // collected into a per-channel vector that was never merged anywhere.
  const cmsUInt16Number values[5] = {0, 20000, 10000, 30000, 65535};
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildTabulatedToneCurve16(nullptr, 5, values), cmsBuildGamma(nullptr, 2.2)
  };
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  CHECK(std::ranges::any_of(parsed.warnings, [](const std::string& warning) { return warning.contains("flattened"); }));
#endif
}

UMBRIEL_TEST(iccDegeneratePrimariesAreRejected) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!bytes.empty());

  // A collapsed colorant triangle has no gamut to map into.
  std::vector<uint8_t> collapsed = bytes;
  patchColorantTo(collapsed, {'g', 'X', 'Y', 'Z'}, 0.64, 0.33);
  patchColorantTo(collapsed, {'b', 'X', 'Y', 'Z'}, 0.64, 0.33);
  const umbriel::IccParse collapsedParse = umbriel::parseIccProfile(collapsed);
  CHECK(!collapsedParse.colorimetry.has_value());
  CHECK(!collapsedParse.error.empty());

  // A zero-sum colorant yields a (0,0) chromaticity and a singular matrix.
  std::vector<uint8_t> zeroed = bytes;
  writeXyzTagBody(zeroed, findTag(zeroed, {'b', 'X', 'Y', 'Z'})->dataOffset, 0.0, 0.0, 0.0);
  const umbriel::IccParse zeroedParse = umbriel::parseIccProfile(zeroed);
  CHECK(!zeroedParse.colorimetry.has_value());
  CHECK(!zeroedParse.error.empty());
#endif
}

UMBRIEL_TEST(fitTransferFitsSrgbLikeCurveWithBoundedError) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // sRGB-like: the sRGB power part with a linear toe, deliberately far enough
  // from every named decode (the toe differs from sRGB's by ~1.5x the named
  // match tolerance) to fall through to the power fit. The fit must stay close
  // to the curve in linear light despite the toe's shadow leverage, and the
  // fitted exponent must stay near the power part.
  const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 5.0, 0.012};
  cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params);
  CHECK(curve != nullptr);
  if (curve == nullptr) {
    return;
  }
  std::array<cmsToneCurve*, 3> curves{curve, curve, curve};
  const std::vector<uint8_t> bytes = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  cmsFreeToneCurve(curve);
  const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
  CHECK(parsed.colorimetry.has_value());
  if (!parsed.colorimetry) {
    return;
  }
  const umbriel::PanelTransfer& transfer = parsed.colorimetry->transfer;
  CHECK(transfer.kind == umbriel::PanelTransfer::Kind::Curve);
  const umbriel::TransferFit fit = umbriel::fitTransfer(transfer);
  CHECK(fit.kind == umbriel::TransferFit::Kind::Power);
  if (fit.kind != umbriel::TransferFit::Kind::Power) {
    return;
  }
  double worst = 0.0;
  for (int probe = 0; probe <= 20000; ++probe) {
    const double linear = probe == 0 ? 1e-6 : probe / 20000.0;
    const double approx = std::pow(linear, 1.0 / fit.exponent);
    const double curveValue = umbriel::panelEncodeSample(transfer, static_cast<float>(linear));
    worst = std::max(worst, std::abs(approx - curveValue) * 255.0);
  }
  std::println("sRGB-like power fit exponent {:.3f}, worst error {:.3f} 8-bit codes", fit.exponent, worst);
  CHECK(fit.exponent > 1.9F && fit.exponent < 2.6F);
  CHECK(worst < 10.5);
#endif
}

UMBRIEL_TEST(iccMalformedStructuresAreRejectedDeterministically) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Untrusted bytes never reach the curve math with a broken shape: every
  // entry point rejects with an error rather than parsing garbage.
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const std::vector<uint8_t> base = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!base.empty());

  const auto rejectsWithError = [&](const std::vector<uint8_t>& bytes, const char* what) {
    const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
    if (parsed.colorimetry.has_value() || parsed.error.empty()) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("parseIccProfile accepted malformed input: {}", what)
      );
    }
    const umbriel::ClientIccDescription client = umbriel::clientIccDescription(bytes);
    if (client.description.has_value() || client.error.empty()) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("clientIccDescription accepted malformed input: {}", what)
      );
    }
  };

  for (const size_t cut :
       {size_t{0}, size_t{8}, size_t{64}, size_t{127}, size_t{131}, size_t{140}, base.size() / 2, base.size() - 1}) {
    rejectsWithError(std::vector<uint8_t>(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(cut)), "truncated");
  }
  {
    std::vector<uint8_t> bytes = base;
    writeU32Be(bytes, 0, static_cast<uint32_t>(bytes.size() + 4096));
    rejectsWithError(bytes, "declared size beyond file");
    writeU32Be(bytes, 0, 10);
    rejectsWithError(bytes, "declared size under header");
  }
  {
    std::vector<uint8_t> bytes = base;
    std::fill(bytes.begin() + 36, bytes.begin() + 40, 0);
    rejectsWithError(bytes, "zeroed magic");
  }
  for (const uint8_t version : {uint8_t{1}, uint8_t{3}, uint8_t{5}, uint8_t{0}}) {
    std::vector<uint8_t> bytes = base;
    patchVersionMajor(bytes, version);
    rejectsWithError(bytes, "unsupported version");
  }
  {
    std::vector<uint8_t> bytes = base;
    writeU32Be(bytes, 128, 0xFFFFFFFF);
    rejectsWithError(bytes, "tag count overflow");
    writeU32Be(bytes, 128, 0x10000);
    rejectsWithError(bytes, "oversized tag count");
  }
  {
    std::vector<uint8_t> bytes = base;
    const TagSpan wtpt = *findTag(bytes, {'w', 't', 'p', 't'});
    writeU32Be(bytes, wtpt.entryOffset + 4, 0xFFFFFF00);
    rejectsWithError(bytes, "tag offset beyond file");
    std::vector<uint8_t> overflow = base;
    const TagSpan overflowed = *findTag(overflow, {'w', 't', 'p', 't'});
    writeU32Be(overflow, overflowed.entryOffset + 4, 0xFFFFFFF0);
    writeU32Be(overflow, overflowed.entryOffset + 8, 0x200);
    rejectsWithError(overflow, "tag offset+size overflow");
  }
  {
    std::vector<uint8_t> bytes = base;
    const TagSpan trc = *findTag(bytes, {'r', 'T', 'R', 'C'});
    writeU32Be(bytes, trc.dataOffset + 8, 0x0FFFFFFF);
    rejectsWithError(bytes, "oversized curv count");
    std::vector<uint8_t> para = base;
    const TagSpan paraSpan = *findTag(para, {'r', 'T', 'R', 'C'});
    writeU16Be(para, paraSpan.dataOffset + 8, 9);
    rejectsWithError(para, "invalid para function type");
  }
  {
    std::vector<uint8_t> bytes = base;
    std::fill(bytes.begin() + 132, bytes.begin() + 132 + 12 * 4, 0);
    rejectsWithError(bytes, "NUL-filled tag table entries");
    std::vector<uint8_t> run = base;
    std::fill(run.begin() + 200, run.begin() + 240, 0);
    rejectsWithError(run, "embedded NUL run");
    rejectsWithError(std::vector<uint8_t>(256, 0), "all-zero profile");
  }
#endif
}

UMBRIEL_TEST(iccMutationFuzzRejectsOrParsesSanely) {
#if !UMBRIEL_HAS_LCMS2
  std::println("skip: built without lcms2");
#else
  // Bounded mutation fuzz: every mutant must either reject with an error or
  // parse into a well-formed panel. The iteration count is env-tunable for
  // one-off deep runs; the seeded sequence is a stable prefix either way.
  size_t iterations = 2000;
  if (const char* env = std::getenv("UMBRIEL_ICC_FUZZ_ITERATIONS")) {
    iterations = std::strtoul(env, nullptr, 10);
  }
  std::array<cmsToneCurve*, 3> curves{
      cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2), cmsBuildGamma(nullptr, 2.2)
  };
  const std::vector<uint8_t> base = matrixShaperProfile(
      xyY(0.3127, 0.329), {.Red = xyY(0.64, 0.33), .Green = xyY(0.30, 0.60), .Blue = xyY(0.15, 0.06)}, curves
  );
  for (cmsToneCurve* curve : curves) {
    cmsFreeToneCurve(curve);
  }
  CHECK(!base.empty());

  uint64_t state = 0x9E3779B97F4A7C15ULL;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  size_t accepted = 0;
  for (size_t iteration = 0; iteration < iterations; ++iteration) {
    std::vector<uint8_t> bytes = base;
    const size_t mutations = 1 + next() % 6;
    for (size_t mutation = 0; mutation < mutations && !bytes.empty(); ++mutation) {
      const size_t position = next() % bytes.size();
      switch (next() % 5) {
      case 0:
        bytes[position] = static_cast<uint8_t>(next());
        break;
      case 1:
        bytes[position] ^= static_cast<uint8_t>(1U << (next() % 8));
        break;
      case 2: {
        const size_t run = std::min<size_t>(1 + next() % 64, bytes.size() - position);
        std::fill(
            bytes.begin() + static_cast<std::ptrdiff_t>(position),
            bytes.begin() + static_cast<std::ptrdiff_t>(position + run), 0
        );
        break;
      }
      case 3:
        if (position + 4 <= bytes.size()) {
          writeU32Be(bytes, position, static_cast<uint32_t>(next()));
        }
        break;
      default:
        bytes.resize(position);
        break;
      }
      if (bytes.empty()) {
        break;
      }
    }
    const umbriel::IccParse parsed = umbriel::parseIccProfile(bytes);
    if (parsed.colorimetry.has_value()) {
      ++accepted;
      const umbriel::PanelColorimetry& panel = *parsed.colorimetry;
      if (!umbriel::validDisplayPrimaries(panel.primaries)
          || !std::isfinite(panel.white[0])
          || !std::isfinite(panel.white[1])) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__, std::format("fuzz iteration {} parsed invalid colorimetry", iteration)
        );
        continue;
      }
      if (panel.transfer.kind == umbriel::PanelTransfer::Kind::Curve) {
        float previous = 0.0F;
        for (const float value : panel.transfer.encode) {
          if (!std::isfinite(value) || value < 0.0F || value > 1.0F || value + 1e-6F < previous) {
            umbriel::test::reportFailure(
                __FILE__, __LINE__, std::format("fuzz iteration {} leaked an invalid encode curve", iteration)
            );
            break;
          }
          previous = value;
        }
      }
    } else if (parsed.error.empty()) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("fuzz iteration {} rejected without an error", iteration)
      );
    }
    const umbriel::ClientIccDescription client = umbriel::clientIccDescription(bytes);
    if (client.description.has_value()) {
      const umbriel::ColorDescription& description = *client.description;
      const bool finite = std::isfinite(description.customPrimaries.red.x)
          && std::isfinite(description.customPrimaries.green.y)
          && std::isfinite(description.customPrimaries.blue.x)
          && std::isfinite(description.tfPower);
      if (!finite) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__, std::format("fuzz iteration {} client description has non-finite fields", iteration)
        );
      }
    } else if (client.error.empty()) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("fuzz iteration {} client rejected without an error", iteration)
      );
    }
  }
  std::println(
      "icc mutation fuzz: {} iterations, {} accepted, {} rejected", iterations, accepted, iterations - accepted
  );
#endif
}

int main() { return RUN_TESTS(); }
