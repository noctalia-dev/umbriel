#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

struct wlr_output_mode;

namespace umbriel {
  struct OutputMode;

  enum class ProbeOutcome : uint8_t {
    TestFailed,   // wlr_output_test_state rejected the state
    CommitFailed, // Test passed but wlr_output_commit_state failed
    Committed,    // Test passed and commit succeeded
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
    std::string_view earlyHdrFail;
  };

  struct FormatSequenceOps {
    std::function<ProbeOutcome(uint32_t format, bool vrr)> probeSdr;
    std::function<ProbeOutcome(uint32_t format, bool vrr)> probeHdr;
    std::function<void()> clearImageDescription;
    std::function<void(wlr_output_mode*)> stageMode;
    std::function<void(std::string_view requested, const wlr_output_mode& fallback)> warnModeFallback;
    std::function<void()> warnHdrVrrIncompatible;
  };

  struct FormatSequenceResult {
    bool committed = false;
    bool usedModeFallback = false;
    bool modeFallbackWarnedNow = false;
    std::string_view hdrFail;
    std::string_view sdr10Fail;
  };

  [[nodiscard]] FormatSequenceResult
  runFormatSequence(const FormatSequenceParams& params, const FormatSequenceOps& ops);
} // namespace umbriel
