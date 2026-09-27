#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

struct wlr_output_mode;

namespace umbriel {
  struct OutputMode;

  enum class FormatTier : uint8_t {
    Hdr,
    Sdr10,
    Sdr8,
  };

  struct FormatSequenceParams {
    bool hdrRequested;
    bool imageDescAvailable;
    bool hdrWasActive;
    uint32_t currentRenderFormat;
    int bitDepth;
    bool tryVrrOn;
    const OutputMode* configuredModeSpec; // nullptr = No mode configured
    wlr_output_mode* preferredMode;       // nullptr = No fallback available
    bool modeFallbackAlreadyWarned;
    std::optional<FormatTier> vrrDroppedTier;
    std::string_view earlyHdrFail;
  };

  struct FormatSequenceOps {
    std::function<bool(uint32_t format, bool vrr)> stageSdr;
    std::function<bool(uint32_t format, bool vrr)> stageHdr;
    std::function<bool()> test;
    std::function<bool()> commit;
    std::function<void()> clearImageDescription;
    std::function<void(wlr_output_mode*)> stageMode;
    std::function<void(std::string_view requested, const wlr_output_mode& fallback)> warnModeFallback;
    std::function<void(FormatTier tier)> warnVrrDropped;
  };

  struct FormatSequenceResult {
    bool committed = false;
    bool usedModeFallback = false;
    bool modeFallbackWarnedNow = false;
    FormatTier committedTier = FormatTier::Sdr8;
    uint32_t committedFormat = 0;
    bool vrrDropped = false;
    std::string_view hdrFail;
    std::string_view sdr10Fail;
  };

  [[nodiscard]] FormatSequenceResult
  runFormatSequence(const FormatSequenceParams& params, const FormatSequenceOps& ops);
} // namespace umbriel
