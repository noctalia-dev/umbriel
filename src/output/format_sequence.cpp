#include "output/format_sequence.h"

#include "config/value_parse.h"
#include "output/hdr_format.h"
#include "output/sdr_format.h"

#include <drm_fourcc.h>
#include <format>
#include <span>

namespace umbriel {

  namespace {

    std::span<const bool> vrrPasses(bool tryVrrOn) {
      static constexpr bool kBothPasses[] = {true, false};
      static constexpr bool kNoVrrPass[] = {false};
      return tryVrrOn ? std::span<const bool>(kBothPasses) : std::span<const bool>(kNoVrrPass);
    }

    // Runs the HDR -> SDR10 -> SDR8 x VRR sequence for whatever mode is currently staged.
    // Returns true if a commit succeeded. Sets hdrFail/sdr10Fail accordingly.
    bool runSequenceForCurrentMode(
        const FormatSequenceParams& params, const FormatSequenceOps& ops, std::string_view& hdrFail,
        std::string_view& sdr10Fail
    ) {
      // HDR.
      const auto tryHdrFormats = [&]() -> bool {
        if (!(params.hdrRequested && params.imageDescAvailable)) {
          if (params.imageDescAvailable || params.hdrWasActive) {
            ops.clearImageDescription();
          }
          return false;
        }

        const auto hdrFormats = hdrRenderFormatCandidates(params.currentRenderFormat);
        bool anyTestPassed = false;
        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          for (const uint32_t fmt : hdrFormats) {
            const ProbeOutcome outcome = ops.probeHdr(fmt, vrr);
            if (outcome == ProbeOutcome::TestFailed) {
              continue;
            }
            anyTestPassed = true;
            if (outcome == ProbeOutcome::Committed) {
              if (params.tryVrrOn && !vrr) {
                ops.warnHdrVrrIncompatible();
              }
              return true;
            }
          }
        }

        hdrFail = anyTestPassed ? "HDR commit rejected by backend" : "backend rejected all 10-bit HDR render formats";
        ops.clearImageDescription();
        return false;
      };

      // SDR10.
      const auto trySdr10Formats = [&]() -> bool {
        if (params.bitDepth != 10) {
          return false;
        }

        bool anyTestPassed = false;
        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          const auto accepted = selectSdr10RenderFormat([&](uint32_t fmt) -> bool {
            const ProbeOutcome outcome = ops.probeSdr(fmt, vrr);
            if (outcome == ProbeOutcome::TestFailed) {
              return false;
            }
            anyTestPassed = true;
            if (outcome == ProbeOutcome::Committed) {
              return true;
            }
            return false;
          });
          if (accepted) {
            return true;
          }
        }
        sdr10Fail =
            anyTestPassed ? "10-bit SDR commit rejected by backend" : "backend rejected all 10-bit SDR render formats";
        return false;
      };

      // SDR8.
      const auto trySdr8Formats = [&]() -> bool {
        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          if (ops.probeSdr(DRM_FORMAT_XRGB8888, vrr) == ProbeOutcome::Committed) {
            return true;
          }
        }
        return false;
      };

      return tryHdrFormats() || trySdr10Formats() || trySdr8Formats();
    }

  } // namespace

  FormatSequenceResult runFormatSequence(const FormatSequenceParams& params, const FormatSequenceOps& ops) {
    FormatSequenceResult result;
    std::string_view hdrFail = params.earlyHdrFail;
    std::string_view sdr10Fail;

    result.committed = runSequenceForCurrentMode(params, ops, hdrFail, sdr10Fail);

    if (!result.committed && params.configuredModeSpec != nullptr && params.preferredMode != nullptr) {
      result.usedModeFallback = true;
      if (!params.modeFallbackAlreadyWarned) {
        result.modeFallbackWarnedNow = true;
        const std::string requested = params.configuredModeSpec->refreshMHz != 0
            ? std::format(
                  "{}x{}@{}mHz", params.configuredModeSpec->width, params.configuredModeSpec->height,
                  params.configuredModeSpec->refreshMHz
              )
            : std::format("{}x{}", params.configuredModeSpec->width, params.configuredModeSpec->height);
        ops.warnModeFallback(requested, *params.preferredMode);
      }

      ops.stageMode(params.preferredMode);

      hdrFail = params.earlyHdrFail;
      sdr10Fail = {};
      result.committed = runSequenceForCurrentMode(params, ops, hdrFail, sdr10Fail);
    }

    result.hdrFail = hdrFail;
    result.sdr10Fail = sdr10Fail;
    return result;
  }

} // namespace umbriel
