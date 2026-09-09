#include "check.h"
#include "config/value_parse.h"
#include "output/format_sequence.h"

#include <cmath>
#include <drm_fourcc.h>
#include <vector>

extern "C" {
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

using umbriel::FormatSequenceOps;
using umbriel::FormatSequenceParams;
using umbriel::FormatSequenceResult;
using umbriel::ProbeOutcome;
using umbriel::runFormatSequence;

namespace {

  // Minimal params. SDR8 only, no HDR, no mode override.
  FormatSequenceParams sdr8Params() {
    return FormatSequenceParams{
        .hdrRequested = false,
        .imageDescAvailable = false,
        .hdrWasActive = false,
        .currentRenderFormat = DRM_FORMAT_XRGB8888,
        .bitDepth = 8,
        .tryVrrOn = false,
        .stagedMode = nullptr,
        .configuredModeSpec = nullptr,
        .preferredMode = nullptr,
        .modeFallbackAlreadyWarned = false,
        .earlyHdrFail = {},
    };
  }

  // Scripted probe dispatcher. SDR and HDR probes consume outcomes in call order.
  // The last entry repeats if the list is exhausted.
  struct MockOps {
    struct Call {
      uint32_t format;
      bool vrr;
      bool hdr;
    };

    std::vector<Call> calls;
    std::vector<ProbeOutcome> sdrOutcomes;
    std::vector<ProbeOutcome> hdrOutcomes;
    bool imageDescCleared = false;
    bool modeFallbackWarned = false;
    bool hdrVrrWarned = false;
    wlr_output_mode* stagedMode = nullptr;

    ProbeOutcome dispatchSdr(uint32_t fmt, bool vrr) {
      calls.push_back({fmt, vrr, false});
      size_t idx = 0;
      for (const auto& c : calls) {
        if (!c.hdr) {
          ++idx;
        }
      }
      --idx;
      if (sdrOutcomes.empty()) {
        return ProbeOutcome::TestFailed;
      }
      return sdrOutcomes[std::min(idx, sdrOutcomes.size() - 1)];
    }

    ProbeOutcome dispatchHdr(uint32_t fmt, bool vrr) {
      calls.push_back({fmt, vrr, true});
      size_t idx = 0;
      for (const auto& c : calls) {
        if (c.hdr) {
          ++idx;
        }
      }
      --idx;
      if (hdrOutcomes.empty()) {
        return ProbeOutcome::TestFailed;
      }
      return hdrOutcomes[std::min(idx, hdrOutcomes.size() - 1)];
    }

    FormatSequenceOps ops() {
      return FormatSequenceOps{
          .probeSdr = [this](uint32_t fmt, bool vrr) { return dispatchSdr(fmt, vrr); },
          .probeHdr = [this](uint32_t fmt, bool vrr) { return dispatchHdr(fmt, vrr); },
          .clearImageDescription = [this] { imageDescCleared = true; },
          .stageMode = [this](wlr_output_mode* m) { stagedMode = m; },
          .warnModeFallback = [this](std::string_view, const wlr_output_mode&) { modeFallbackWarned = true; },
          .warnHdrVrrIncompatible = [this] { hdrVrrWarned = true; },
      };
    }
  };

} // namespace

// SDR8 baseline: Single XR24 probe committed.
UMBRIEL_TEST(sdr8CommitsXr24) {
  MockOps m;
  m.sdrOutcomes = {ProbeOutcome::Committed};

  const FormatSequenceResult r = runFormatSequence(sdr8Params(), m.ops());

  CHECK(r.committed);
  CHECK(!r.usedModeFallback);
  CHECK_EQ(m.calls.size(), size_t{1});
  CHECK_EQ(m.calls[0].format, uint32_t{DRM_FORMAT_XRGB8888});
}

// SDR10: XR30 test-fails, XB30 commits.
UMBRIEL_TEST(sdr10XR30TestFailedFallsBackToXB30) {
  MockOps m;
  m.sdrOutcomes = {ProbeOutcome::TestFailed, ProbeOutcome::Committed};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.sdr10Fail.empty());
  // Two SDR probes: XR30 then XB30.
  size_t sdrCalls = 0;
  for (const auto& c : m.calls) {
    if (!c.hdr) {
      ++sdrCalls;
    }
  }
  CHECK_EQ(sdrCalls, size_t{2});
}

// SDR10: Both XR30 and XB30 test-fail -> reason set, falls through to XR24.
UMBRIEL_TEST(sdr10BothRejectedSetsFallbackReasonAndCommitsSdr8) {
  MockOps m;
  m.sdrOutcomes = {
      ProbeOutcome::TestFailed, // XR30
      ProbeOutcome::TestFailed, // XB30
      ProbeOutcome::Committed,  // XR24 SDR8 fallback
  };

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.sdr10Fail, std::string_view{"backend rejected all 10-bit SDR render formats"});
}

// VRR retry: XR24 commit-fails with vrr=true, succeeds with vrr=false.
UMBRIEL_TEST(vrrRetryCommitsWithoutVrr) {
  MockOps m;
  m.sdrOutcomes = {ProbeOutcome::CommitFailed, ProbeOutcome::Committed};

  FormatSequenceParams p = sdr8Params();
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(m.calls.size(), size_t{2});
  CHECK(m.calls[0].vrr == true);
  CHECK(m.calls[1].vrr == false);
}

// Commit failure: XR30 and XB30 test-pass but commit-fail, XR24 succeeds.
UMBRIEL_TEST(sdr10CommitFailureFallsThroughToSdr8) {
  MockOps m;
  m.sdrOutcomes = {
      ProbeOutcome::CommitFailed, // XR30
      ProbeOutcome::CommitFailed, // XB30
      ProbeOutcome::Committed,    // XR24
  };

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.sdr10Fail, std::string_view{"10-bit SDR commit rejected by backend"});
}

// HDR fail -> SDR10: All HDR formats test-fail, XB30 SDR10 commits.
UMBRIEL_TEST(hdrFailFollowedBySdr10Success) {
  MockOps m;
  m.hdrOutcomes = {ProbeOutcome::TestFailed, ProbeOutcome::TestFailed}; // XR30, XB30 HDR
  m.sdrOutcomes = {ProbeOutcome::TestFailed, ProbeOutcome::Committed};  // XR30 test-fail, XB30 ok

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(!r.hdrFail.empty());
  CHECK(r.sdr10Fail.empty());
  CHECK(m.imageDescCleared);
}

// Mode fallback: All formats fail for the staged mode, preferred mode retry succeeds.
UMBRIEL_TEST(modeFallbackRetriesOnPreferredMode) {
  static wlr_output_mode preferred{};

  MockOps m;
  m.sdrOutcomes = {ProbeOutcome::TestFailed, ProbeOutcome::Committed};

  static const umbriel::OutputMode spec{1280, 720, 60000};
  FormatSequenceParams p = sdr8Params();
  p.configuredModeSpec = &spec;
  p.preferredMode = &preferred;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.usedModeFallback);
  CHECK(r.modeFallbackWarnedNow);
  CHECK(m.modeFallbackWarned);
  CHECK(m.stagedMode == &preferred);
}

// No preferred mode (headless): All formats fail, no fallback, uncommitted.
UMBRIEL_TEST(noPreferredModeYieldsUncommitted) {
  MockOps m;
  m.sdrOutcomes = {ProbeOutcome::TestFailed};

  static const umbriel::OutputMode spec{1280, 720, 60000};
  FormatSequenceParams p = sdr8Params();
  p.configuredModeSpec = &spec;
  p.preferredMode = nullptr;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(!r.committed);
  CHECK(!r.usedModeFallback);
}

int main() { return RUN_TESTS(); }
