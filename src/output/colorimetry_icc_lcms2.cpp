// ICC matrix-shaper parsing via lcms2. Compiled only when the
// build finds lcms2; colorimetry_icc_fallback.cpp is its inert counterpart.

#include "output/colorimetry.h"
#include "output/colorimetry_internal.h"
#include "output/hdr_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <lcms2.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace umbriel {

  namespace {

    // Named fast paths compare the evaluated curve; anything looser is baked.
    constexpr float kCurveMatchTolerance = 1e-3F;

    struct ProfileCloser {
      void operator()(void* profile) const {
        if (profile != nullptr) {
          cmsCloseProfile(profile);
        }
      }
    };
    using ProfilePtr = std::unique_ptr<void, ProfileCloser>;

    float srgbDecodeSample(float encoded) {
      return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
    }

    // Untrusted curves can evaluate to non-finite values and need not
    // increase; the inversion and the fit below require a finite monotone
    // decode, so flatten the samples onto that envelope and say so.
    void sanitizeDecodeCurve(std::vector<float>& samples, std::vector<std::string>& warnings) {
      bool repaired = false;
      float lastFinite = 0.0F;
      for (float& sample : samples) {
        if (!std::isfinite(sample)) {
          sample = lastFinite;
          repaired = true;
          continue;
        }
        lastFinite = std::clamp(sample, 0.0F, 1.0F);
        sample = lastFinite;
      }
      float runningMax = 0.0F;
      for (float& sample : samples) {
        if (sample < runningMax) {
          sample = runningMax;
          repaired = true;
        } else {
          runningMax = sample;
        }
      }
      if (repaired) {
        warnings.emplace_back("flattened a TRC that was non-finite or non-monotonic");
      }
    }

    std::vector<float> evaluateCurve(const cmsToneCurve* curve, std::vector<std::string>& warnings) {
      std::vector<float> samples(color_detail::kCurveSamples);
      for (size_t index = 0; index < color_detail::kCurveSamples; ++index) {
        const float encoded = static_cast<float>(index) / static_cast<float>(color_detail::kCurveSamples - 1);
        samples[index] = cmsEvalToneCurveFloat(curve, encoded);
      }
      sanitizeDecodeCurve(samples, warnings);
      return samples;
    }

    bool matchesDecode(const std::vector<float>& samples, float (*decode)(float)) {
      for (size_t index = 0; index < samples.size(); ++index) {
        const float encoded = static_cast<float>(index) / static_cast<float>(samples.size() - 1);
        if (std::abs(samples[index] - decode(encoded)) > kCurveMatchTolerance) {
          return false;
        }
      }
      return true;
    }

    bool sameCurve(const std::vector<float>& first, const std::vector<float>& second) {
      for (size_t index = 0; index < first.size(); ++index) {
        if (std::abs(first[index] - second[index]) > kCurveMatchTolerance) {
          return false;
        }
      }
      return true;
    }

    // Numeric inversion of the evaluated decode curve (encoded -> linear) into
    // the panel encode curve (linear -> encoded) on the sampled encode grid.
    // The forward curve is monotone (sanitized) and inverted by binary search
    // plus interpolation: an estimation, accurate to the sampling resolution.
    std::vector<float> invertDecodeCurve(const std::vector<float>& decode) {
      std::vector<float> encode(decode.size());
      for (size_t target = 0; target < encode.size(); ++target) {
        const float linear = color_detail::encodeGridLinear(target, encode.size());
        const auto upper = std::ranges::lower_bound(decode, linear);
        if (upper == decode.begin()) {
          encode[target] = 0.0F;
          continue;
        }
        if (upper == decode.end()) {
          encode[target] = 1.0F;
          continue;
        }
        const size_t high = static_cast<size_t>(upper - decode.begin());
        const size_t low = high - 1;
        const float span = decode[high] - decode[low];
        const float fraction = span > 0.0F ? (linear - decode[low]) / span : 0.0F;
        const float encoded = (static_cast<float>(low) + fraction) / static_cast<float>(decode.size() - 1);
        encode[target] = std::clamp(encoded, 0.0F, 1.0F);
      }
      return encode;
    }

    void multiplyMatrixVector(const double matrix[9], const cmsCIEXYZ& in, double out[3]) {
      out[0] = matrix[0] * in.X + matrix[1] * in.Y + matrix[2] * in.Z;
      out[1] = matrix[3] * in.X + matrix[4] * in.Y + matrix[5] * in.Z;
      out[2] = matrix[6] * in.X + matrix[7] * in.Y + matrix[8] * in.Z;
    }

    // Move a tagged PCS value into the frame the colorants end up in.
    cmsCIEXYZ applyFrame(const double frame[9], const cmsCIEXYZ& tagged) {
      double xyz[3];
      multiplyMatrixVector(frame, tagged, xyz);
      return {.X = xyz[0], .Y = xyz[1], .Z = xyz[2]};
    }

    wlr_color_cie1931_xy xyFromXyz(const cmsCIEXYZ& xyz) {
      const double sum = xyz.X + xyz.Y + xyz.Z;
      if (!std::isfinite(sum) || std::abs(sum) < 1e-9) {
        return {.x = 0.0F, .y = 0.0F};
      }
      return {
          .x = static_cast<float>(xyz.X / sum),
          .y = static_cast<float>(xyz.Y / sum),
      };
    }

    // The ICC TRC tags carry the decode curve (encoded -> linear), while a
    // PanelTransfer carries the encode curve, so sampled kinds are inverted
    // numerically here and the parser inverts back on read.
    std::vector<float> decodeFromEncode(const PanelTransfer& transfer) {
      std::vector<float> decode(color_detail::kCurveSamples);
      for (size_t index = 0; index < color_detail::kCurveSamples; ++index) {
        const float encoded = static_cast<float>(index) / static_cast<float>(color_detail::kCurveSamples - 1);
        float low = 0.0F;
        float high = 1.0F;
        for (int step = 0; step < 24; ++step) {
          const float middle = (low + high) * 0.5F;
          if (panelEncodeSample(transfer, middle) < encoded) {
            low = middle;
          } else {
            high = middle;
          }
        }
        decode[index] = (low + high) * 0.5F;
      }
      return decode;
    }

    cmsToneCurve* decodeCurve(const PanelTransfer& transfer) {
      switch (transfer.kind) {
      case PanelTransfer::Kind::Srgb: {
        // lcms2 parametric type 4 = ICC parametricCurveType format 3 (the sRGB
        // EOTF): Y = (aX + b)^g above d, else cX.
        const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
        return cmsBuildParametricToneCurve(nullptr, 4, params);
      }
      case PanelTransfer::Kind::Gamma22:
        return cmsBuildGamma(nullptr, 2.2);
      case PanelTransfer::Kind::Curve:
        break;
      }
      const std::vector<float> decode = decodeFromEncode(transfer);
      std::vector<uint16_t> samples(decode.size());
      for (size_t index = 0; index < decode.size(); ++index) {
        samples[index] = static_cast<uint16_t>(std::lround(std::clamp(decode[index], 0.0F, 1.0F) * 65535.0F));
      }
      return cmsBuildTabulatedToneCurve16(nullptr, static_cast<cmsUInt32Number>(samples.size()), samples.data());
    }

  } // namespace

  IccParse parseIccProfile(std::span<const uint8_t> data, IccClassScope scope) {
    IccParse result;
    if (data.empty()) {
      result.error = "empty profile data";
      return result;
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<cmsUInt32Number>::max())) {
      result.error = "profile is too large";
      return result;
    }
    if (const std::string structureError = color_detail::iccStructureError(data); !structureError.empty()) {
      result.error = structureError;
      return result;
    }
    ProfilePtr profile(cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size())));
    if (!profile) {
      result.error = "not a readable ICC profile";
      return result;
    }
    const cmsProfileClassSignature deviceClass = cmsGetDeviceClass(profile.get());
    const bool classInScope = deviceClass == cmsSigDisplayClass
        || (scope == IccClassScope::DisplayOrColorSpace && deviceClass == cmsSigColorSpaceClass);
    if (!classInScope) {
      result.error = scope == IccClassScope::DisplayOrColorSpace ? "not an ICC display or color space profile"
                                                                 : "not an ICC display profile";
      return result;
    }
    if (cmsGetColorSpace(profile.get()) != cmsSigRgbData) {
      result.error = "not a 3-channel RGB profile";
      return result;
    }
    if (cmsIsTag(profile.get(), cmsSigAToB0Tag)
        || cmsIsTag(profile.get(), cmsSigAToB1Tag)
        || cmsIsTag(profile.get(), cmsSigAToB2Tag)
        || cmsIsTag(profile.get(), cmsSigBToA0Tag)
        || cmsIsTag(profile.get(), cmsSigBToA1Tag)
        || cmsIsTag(profile.get(), cmsSigBToA2Tag)) {
      result.error = "LUT-based profiles (A2B0/B2A0) are unsupported; only matrix-shaper profiles are";
      return result;
    }
    if (cmsIsTag(profile.get(), cmsSigVcgtTag)) {
      result.warnings.emplace_back("ignoring vcgt tag (a pre-dispatch gamma ramp, not content colorimetry)");
    }

    const auto* red = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigRedColorantTag));
    const auto* green = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigGreenColorantTag));
    const auto* blue = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigBlueColorantTag));
    const auto* whitePoint = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigMediaWhitePointTag));
    if (red == nullptr || green == nullptr || blue == nullptr) {
      result.error = "not a matrix-shaper profile (rXYZ/gXYZ/bXYZ colorants missing)";
      return result;
    }
    if (whitePoint == nullptr) {
      result.error = "not a matrix-shaper profile (media white point missing)";
      return result;
    }

    // Colorants and white must end up in one frame. The colorant tags are D50
    // PCS-relative; chad records the adaptation applied on the way there, so
    // chad^-1 recovers the measured colorimetry. The white is derived as the
    // image of (1,1,1) under the colorant matrix in that same frame — the
    // colorant sum — which stays correct whatever wtpt claims (a measured wtpt
    // beside chad would otherwise be un-adapted twice).
    double frame[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool frameFromChad = false;
    if (const auto* chad = static_cast<const double*>(cmsReadTag(profile.get(), cmsSigChromaticAdaptationTag))) {
      double chadMatrix[9];
      std::copy_n(chad, 9, chadMatrix);
      if (color_detail::invert3x3(chadMatrix, frame)) {
        frameFromChad = true;
      } else {
        result.warnings.emplace_back("ignoring a singular chad tag");
      }
    }
    if (!frameFromChad) {
      // Without a usable chad the colorants are D50-relative by specification.
      // When wtpt declares a measured white that is not D50 (the layout v2
      // measurement suites emit), adapt the colorants to it explicitly so the
      // measured colorimetry is what gets reported.
      const double d50[3] = {0.9642, 1.0, 0.8249};
      const double wtptSum = whitePoint->X + whitePoint->Y + whitePoint->Z;
      if (std::isfinite(wtptSum) && std::abs(wtptSum) > 1e-9 && whitePoint->Y > 0.0) {
        const double wtptXyX = whitePoint->X / wtptSum;
        const double wtptXyY = whitePoint->Y / wtptSum;
        if (std::abs(wtptXyX - 0.3457) > 0.001 || std::abs(wtptXyY - 0.3585) > 0.001) {
          const double measuredWhite[3] = {
              whitePoint->X / whitePoint->Y,
              1.0,
              whitePoint->Z / whitePoint->Y,
          };
          color_detail::bradfordAdapt(d50, measuredWhite, frame);
        }
      }
    }

    const cmsCIEXYZ taggedSum{
        .X = red->X + green->X + blue->X,
        .Y = red->Y + green->Y + blue->Y,
        .Z = red->Z + green->Z + blue->Z,
    };
    const cmsCIEXYZ realRed = applyFrame(frame, *red);
    const cmsCIEXYZ realGreen = applyFrame(frame, *green);
    const cmsCIEXYZ realBlue = applyFrame(frame, *blue);
    const cmsCIEXYZ realWhite = applyFrame(frame, taggedSum);

    PanelColorimetry panel{
        .primaries =
            {
                .red = xyFromXyz(realRed),
                .green = xyFromXyz(realGreen),
                .blue = xyFromXyz(realBlue),
                .white = xyFromXyz(realWhite),
            },
        .white = {0.0F, 0.0F},
        .transfer = {},
    };
    panel.white[0] = panel.primaries.white.x;
    panel.white[1] = panel.primaries.white.y;

    // A collapsed colorant triangle cannot anchor a gamut mapping and would
    // send singular matrices into the transform builder. Degeneracy lives in
    // the XYZ colorants (a zero-sum colorant is a zero column), so span them
    // as a matrix and require it to be invertible alongside the hull check.
    // Singularity is transpose-invariant, so the storage order is irrelevant.
    const double colorants[9] = {
        realRed.X, realRed.Y, realRed.Z, realGreen.X, realGreen.Y, realGreen.Z, realBlue.X, realBlue.Y, realBlue.Z,
    };
    double colorantInverse[9];
    if (!validDisplayPrimaries(panel.primaries) || !color_detail::invert3x3(colorants, colorantInverse)) {
      result.error = "degenerate ICC primaries (the colorants do not span a gamut)";
      return result;
    }

    const auto* redTrc = static_cast<const cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigRedTRCTag));
    const auto* greenTrc = static_cast<const cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigGreenTRCTag));
    const auto* blueTrc = static_cast<const cmsToneCurve*>(cmsReadTag(profile.get(), cmsSigBlueTRCTag));
    if (redTrc == nullptr || greenTrc == nullptr || blueTrc == nullptr) {
      result.error = "not a matrix-shaper profile (rTRC/gTRC/bTRC missing)";
      return result;
    }

    const std::vector<float> redDecode = evaluateCurve(redTrc, result.warnings);
    if (!sameCurve(redDecode, evaluateCurve(greenTrc, result.warnings))
        || !sameCurve(redDecode, evaluateCurve(blueTrc, result.warnings))) {
      result.warnings.emplace_back("per-channel TRCs differ; using the red channel curve");
    }

    if (matchesDecode(redDecode, srgbDecodeSample)) {
      panel.transfer.kind = PanelTransfer::Kind::Srgb;
    } else if (matchesDecode(redDecode, [](float encoded) { return std::pow(encoded, 2.2F); })) {
      panel.transfer.kind = PanelTransfer::Kind::Gamma22;
    } else {
      panel.transfer.kind = PanelTransfer::Kind::Curve;
      panel.transfer.encode = invertDecodeCurve(redDecode);
    }

    result.colorimetry = std::move(panel);
    return result;
  }

  IccProfileStore iccStoreForPanel(const PanelColorimetry* panel) {
    if (panel == nullptr) {
      return {};
    }
    // A matrix-shaper display profile carrying the panel's colorants, white
    // point and TRC. lcms2 adapts to the D50 PCS and records chad on the way;
    // the parser undoes that adaptation on read (pinned by
    // iccChadTagRecoversMeasuredWhiteAndColorants).
    const cmsCIExyY white = {.x = panel->white[0], .y = panel->white[1], .Y = 1.0};
    const cmsCIExyYTRIPLE primaries{
        .Red = {.x = panel->primaries.red.x, .y = panel->primaries.red.y, .Y = 1.0},
        .Green = {.x = panel->primaries.green.x, .y = panel->primaries.green.y, .Y = 1.0},
        .Blue = {.x = panel->primaries.blue.x, .y = panel->primaries.blue.y, .Y = 1.0},
    };
    cmsToneCurve* curves[3]{decodeCurve(panel->transfer), decodeCurve(panel->transfer), decodeCurve(panel->transfer)};
    if (curves[0] == nullptr || curves[1] == nullptr || curves[2] == nullptr) {
      for (cmsToneCurve* curve : curves) {
        if (curve != nullptr) {
          cmsFreeToneCurve(curve);
        }
      }
      return {};
    }
    cmsHPROFILE profile = cmsCreateRGBProfileTHR(nullptr, &white, &primaries, curves);
    for (cmsToneCurve* curve : curves) {
      cmsFreeToneCurve(curve);
    }
    if (profile == nullptr) {
      return {};
    }
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0) {
      cmsCloseProfile(profile);
      return {};
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>(size);
    if (!cmsSaveProfileToMem(profile, bytes->data(), &size)) {
      cmsCloseProfile(profile);
      return {};
    }
    bytes->resize(size);
    cmsCloseProfile(profile);
    return bytes;
  }

} // namespace umbriel
