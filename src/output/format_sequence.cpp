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
        const FormatSequenceParams& params, const FormatSequenceOps& ops, FormatSequenceResult& result,
        std::string_view& hdrFail, std::string_view& sdr10Fail
    ) {
      const auto commitStaged = [&](FormatTier tier, uint32_t fmt, bool vrr) -> bool {
        if (!ops.commit()) {
          return false;
        }
        result.committedTier = tier;
        result.committedFormat = fmt;
        result.vrrDropped = params.tryVrrOn && !vrr;
        if (result.vrrDropped && params.vrrDroppedTier != tier) {
          ops.warnVrrDropped(tier);
        }
        return true;
      };

      const auto commitTenBit = [&](FormatTier tier, uint32_t fmt, bool vrr,
                                    const std::function<bool(uint32_t, bool)>& stage) -> bool {
        if (commitStaged(tier, fmt, vrr)) {
          return true;
        }
        return vrr && stage(fmt, false) && commitStaged(tier, fmt, false);
      };

      // HDR.
      const auto tryHdrFormats = [&]() -> bool {
        if (!(params.hdrRequested && params.imageDescAvailable)) {
          if (params.hdrWasActive) {
            ops.clearImageDescription();
          }
          return false;
        }

        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          const auto accepted = selectHdrRenderFormat(params.currentRenderFormat, [&](uint32_t fmt) {
            return ops.stageHdr(fmt, vrr) && ops.test();
          });
          if (!accepted) {
            continue;
          }
          if (commitTenBit(FormatTier::Hdr, *accepted, vrr, ops.stageHdr)) {
            return true;
          }
          hdrFail = "HDR commit rejected by backend";
          ops.clearImageDescription();
          return false;
        }

        hdrFail = "backend rejected all 10-bit HDR render formats";
        ops.clearImageDescription();
        return false;
      };

      // SDR10.
      const auto trySdr10Formats = [&]() -> bool {
        if (params.bitDepth != 10) {
          return false;
        }

        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          const auto accepted = selectSdr10RenderFormat(params.currentRenderFormat, [&](uint32_t fmt) {
            return ops.stageSdr(fmt, vrr) && ops.test();
          });
          if (!accepted) {
            continue;
          }
          if (commitTenBit(FormatTier::Sdr10, *accepted, vrr, ops.stageSdr)) {
            return true;
          }
          sdr10Fail = "10-bit SDR commit rejected by backend";
          return false;
        }

        sdr10Fail = "backend rejected all 10-bit SDR render formats";
        return false;
      };

      // SDR8. The last resort, so it is committed without a test, retrying once without VRR.
      const auto trySdr8Formats = [&]() -> bool {
        for (const bool vrr : vrrPasses(params.tryVrrOn)) {
          ops.stageSdr(DRM_FORMAT_XRGB8888, vrr);
          if (commitStaged(FormatTier::Sdr8, DRM_FORMAT_XRGB8888, vrr)) {
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

    result.committed = runSequenceForCurrentMode(params, ops, result, hdrFail, sdr10Fail);

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
      result.committed = runSequenceForCurrentMode(params, ops, result, hdrFail, sdr10Fail);
    }

    result.hdrFail = hdrFail;
    result.sdr10Fail = sdr10Fail;
    return result;
  }

} // namespace umbriel
