#include "check.h"
#include "config/value_parse.h"
#include "output/format_sequence.h"

#include <algorithm>
#include <drm_fourcc.h>
#include <vector>

// Pulls the C++ math headers in before `static` is defined away below.
#include <cmath> // IWYU pragma: keep

extern "C" {
// wlroots uses C99 array parameter syntax in headers included by wlr_output.h.
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

using umbriel::FormatSequenceOps;
using umbriel::FormatSequenceParams;
using umbriel::FormatSequenceResult;
using umbriel::FormatTier;
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
        .configuredModeSpec = nullptr,
        .preferredMode = nullptr,
        .modeFallbackAlreadyWarned = false,
        .vrrDroppedTier = std::nullopt,
        .earlyHdrFail = {},
    };
  }

  // Scripted backend. Test and commit results are consumed in call order, and the
  // last entry repeats once the list is exhausted.
  struct MockOps {
    enum class Kind : uint8_t { StageSdr, StageHdr, Test, Commit };

    struct Call {
      Kind kind;
      uint32_t format;
      bool vrr;
    };

    std::vector<Call> calls;
    std::vector<bool> testResults;
    std::vector<bool> commitResults;
    std::vector<uint32_t> nonPrimaryFormats;
    size_t testIndex = 0;
    size_t commitIndex = 0;
    uint32_t stagedFormat = 0;
    bool stagedVrr = false;
    bool imageDescCleared = false;
    bool modeFallbackWarned = false;
    std::vector<FormatTier> vrrWarnings;
    wlr_output_mode* stagedMode = nullptr;

    static bool next(const std::vector<bool>& script, size_t& index, bool fallback) {
      if (script.empty()) {
        return fallback;
      }
      const bool value = script[std::min(index, script.size() - 1)];
      ++index;
      return value;
    }

    bool stage(Kind kind, uint32_t fmt, bool vrr) {
      calls.push_back({kind, fmt, vrr});
      if (std::ranges::find(nonPrimaryFormats, fmt) != nonPrimaryFormats.end()) {
        return false;
      }
      stagedFormat = fmt;
      stagedVrr = vrr;
      return true;
    }

    [[nodiscard]] size_t count(Kind kind) const {
      return static_cast<size_t>(std::ranges::count_if(calls, [&](const Call& c) { return c.kind == kind; }));
    }

    // The staged format and VRR state of every commit attempt, in order.
    [[nodiscard]] std::vector<Call> commits() const {
      std::vector<Call> out;
      for (const Call& c : calls) {
        if (c.kind == Kind::Commit) {
          out.push_back(c);
        }
      }
      return out;
    }

    FormatSequenceOps ops() {
      return FormatSequenceOps{
          .stageSdr = [this](uint32_t fmt, bool vrr) { return stage(Kind::StageSdr, fmt, vrr); },
          .stageHdr = [this](uint32_t fmt, bool vrr) { return stage(Kind::StageHdr, fmt, vrr); },
          .test =
              [this] {
                calls.push_back({Kind::Test, stagedFormat, stagedVrr});
                return next(testResults, testIndex, false);
              },
          .commit =
              [this] {
                calls.push_back({Kind::Commit, stagedFormat, stagedVrr});
                return next(commitResults, commitIndex, true);
              },
          .clearImageDescription = [this] { imageDescCleared = true; },
          .stageMode = [this](wlr_output_mode* m) { stagedMode = m; },
          .warnModeFallback = [this](std::string_view, const wlr_output_mode&) { modeFallbackWarned = true; },
          .warnVrrDropped = [this](FormatTier tier) { vrrWarnings.push_back(tier); },
      };
    }
  };

  using Kind = MockOps::Kind;

} // namespace

// SDR8 baseline: XR24 is committed directly, without a test.
UMBRIEL_TEST(sdr8CommitsXr24) {
  MockOps m;

  const FormatSequenceResult r = runFormatSequence(sdr8Params(), m.ops());

  CHECK(r.committed);
  CHECK(!r.usedModeFallback);
  CHECK(r.committedTier == FormatTier::Sdr8);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB8888});
  CHECK_EQ(m.count(Kind::Test), size_t{0});
  CHECK_EQ(m.count(Kind::Commit), size_t{1});
}

// SDR10: XR30 fails its test, XB30 passes and is the only commit.
UMBRIEL_TEST(sdr10XR30TestFailedFallsBackToXB30) {
  MockOps m;
  m.testResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.count(Kind::Test), size_t{2});
  CHECK_EQ(m.count(Kind::Commit), size_t{1});
}

// Keep XB30 when it is already active instead of switching to XR30.
UMBRIEL_TEST(sdr10PrefersActiveXB30) {
  MockOps m;
  m.testResults = {true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.currentRenderFormat = DRM_FORMAT_XBGR2101010;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{1});
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XBGR2101010});
}

// SDR10: Both 10-bit formats fail their tests. Nothing is committed for them,
// the reason is set, and XR24 commits.
UMBRIEL_TEST(sdr10BothRejectedSetsFallbackReasonAndCommitsSdr8) {
  MockOps m;

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.sdr10Fail, std::string_view{"backend rejected all 10-bit SDR render formats"});
  CHECK(r.committedTier == FormatTier::Sdr8);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{1});
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XRGB8888});
}

// A format outside the primary plane set is skipped without a test.
UMBRIEL_TEST(nonPrimaryFormatIsNotTested) {
  MockOps m;
  m.nonPrimaryFormats = {DRM_FORMAT_XRGB2101010};
  m.testResults = {true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
}

// VRR retry: XR24 commit fails with VRR and succeeds without it.
UMBRIEL_TEST(vrrRetryCommitsWithoutVrr) {
  MockOps m;
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.vrrDropped);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{2});
  CHECK(commits[0].vrr == true);
  CHECK(commits[1].vrr == false);
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Sdr8);
}

// SDR10 VRR retry: Both formats fail with VRR. The active XB30 passes
// without VRR and is the only commit.
UMBRIEL_TEST(sdr10VrrRetryWarnsWhenVrrDropped) {
  MockOps m;
  m.testResults = {false, false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.currentRenderFormat = DRM_FORMAT_XBGR2101010;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK(r.vrrDropped);
  CHECK_EQ(m.count(Kind::Test), size_t{3});
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{1});
  CHECK(commits[0].vrr == false);
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Sdr10);
}

// HDR VRR retry: HDR only passes its test without VRR.
UMBRIEL_TEST(hdrVrrRetryWarnsWhenVrrDropped) {
  MockOps m;
  m.testResults = {false, false, true};

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Hdr);
  CHECK_EQ(m.count(Kind::Commit), size_t{1});
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Hdr);
}

// No warning when VRR is kept.
UMBRIEL_TEST(sdr10WithVrrDoesNotWarn) {
  MockOps m;
  m.testResults = {true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(!r.vrrDropped);
  CHECK(m.commits()[0].vrr == true);
  CHECK(m.vrrWarnings.empty());
}

// The VRR warning is not repeated when the same tier dropped VRR last time.
UMBRIEL_TEST(vrrWarningNotRepeatedForSameTier) {
  MockOps m;
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.tryVrrOn = true;
  p.vrrDroppedTier = FormatTier::Sdr8;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.vrrDropped);
  CHECK(m.vrrWarnings.empty());
}

// The VRR warning comes back when a different tier drops VRR.
UMBRIEL_TEST(vrrWarningRepeatsWhenTierChanges) {
  MockOps m;
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.tryVrrOn = true;
  p.vrrDroppedTier = FormatTier::Hdr;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Sdr8);
}

// A failed XR30 commit ends the tier; XB30 is only considered when XR30
// fails its pre-commit test.
UMBRIEL_TEST(sdr10CommitFailureFallsThroughToSdr8) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.sdr10Fail, std::string_view{"10-bit SDR commit rejected by backend"});
  CHECK(r.committedTier == FormatTier::Sdr8);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{2});
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[1].format, uint32_t{DRM_FORMAT_XRGB8888});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
}

// HDR commit failure drops to SDR10 without committing the other HDR format.
UMBRIEL_TEST(hdrCommitFailureFallsThroughToSdr10) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.hdrFail, std::string_view{"HDR commit rejected by backend"});
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK(m.imageDescCleared);
  CHECK_EQ(m.count(Kind::StageHdr), size_t{1});
  CHECK_EQ(m.count(Kind::Commit), size_t{2});
}

// A failed HDR+VRR commit retries the same format without VRR
// directly. It neither tests the retry nor considers the alternate format.
UMBRIEL_TEST(hdrCommitFailureRetriesSameFormatWithoutVrr) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Hdr);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK(r.hdrFail.empty());
  CHECK(r.vrrDropped);
  CHECK(!m.imageDescCleared);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{2});
  CHECK(commits[0].vrr);
  CHECK(!commits[1].vrr);
  CHECK_EQ(commits[1].format, commits[0].format);
  CHECK_EQ(m.count(Kind::Test), size_t{1});
  CHECK_EQ(m.count(Kind::StageHdr), size_t{2});
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Hdr);
}

// If both commits fail, HDR falls through without trying XB30.
UMBRIEL_TEST(hdrBothCommitsFailFallsThroughToSdr8) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, false, true};

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Sdr8);
  CHECK_EQ(r.hdrFail, std::string_view{"HDR commit rejected by backend"});
  CHECK(!r.vrrDropped);
  CHECK(m.imageDescCleared);
  CHECK(m.vrrWarnings.empty());
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{3});
  CHECK(commits[0].vrr);
  CHECK(!commits[1].vrr);
  CHECK(commits[2].vrr);
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[1].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[2].format, uint32_t{DRM_FORMAT_XRGB8888});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
}

// A weak pre-commit test can accept VRR even though the real commit rejects it.
// Retry the same SDR10 format without testing it again.
UMBRIEL_TEST(sdr10CommitFailureRetriesSameFormatWithoutVrr) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK(r.sdr10Fail.empty());
  CHECK(r.vrrDropped);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{2});
  CHECK(commits[0].vrr);
  CHECK(!commits[1].vrr);
  CHECK_EQ(commits[1].format, commits[0].format);
  CHECK_EQ(m.count(Kind::Test), size_t{1});
  CHECK_EQ(m.count(Kind::StageSdr), size_t{2});
  CHECK_EQ(m.vrrWarnings.size(), size_t{1});
  CHECK(m.vrrWarnings[0] == FormatTier::Sdr10);
}

// XB30 is still tried if XR30 fails its test rather than its commit.
UMBRIEL_TEST(sdr10VrrTestFailureTriesXB30WithVrr) {
  MockOps m;
  m.testResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK(!r.vrrDropped);
  CHECK_EQ(m.count(Kind::Test), size_t{2});
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{1});
  CHECK(commits[0].vrr);
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XBGR2101010});
}

// A failed XR30 VRR commit does not try XB30, even when XB30 could commit.
UMBRIEL_TEST(sdr10CommitFailureDoesNotTryXB30WithVrr) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK(r.vrrDropped);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{2});
  CHECK(commits[0].vrr);
  CHECK(!commits[1].vrr);
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[1].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
}

// Both failed commits of the selected SDR10 format fall through to SDR8.
UMBRIEL_TEST(sdr10BothCommitsFailFallsThroughToSdr8) {
  MockOps m;
  m.testResults = {true};
  m.commitResults = {false, false, true};

  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK_EQ(r.sdr10Fail, std::string_view{"10-bit SDR commit rejected by backend"});
  CHECK(r.committedTier == FormatTier::Sdr8);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB8888});
  CHECK(!r.vrrDropped);
  const auto commits = m.commits();
  CHECK_EQ(commits.size(), size_t{3});
  CHECK(commits[0].vrr);
  CHECK(!commits[1].vrr);
  CHECK(commits[2].vrr);
  CHECK_EQ(commits[0].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[1].format, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(commits[2].format, uint32_t{DRM_FORMAT_XRGB8888});
  CHECK_EQ(m.count(Kind::Test), size_t{1});
}

// HDR fail -> SDR10: All HDR formats fail their tests, XB30 SDR10 commits.
UMBRIEL_TEST(hdrFailFollowedBySdr10Success) {
  MockOps m;
  m.testResults = {false, false, false, true}; // HDR XR30, HDR XB30, SDR10 XR30, SDR10 XB30

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(!r.hdrFail.empty());
  CHECK(r.sdr10Fail.empty());
  CHECK(m.imageDescCleared);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.count(Kind::Commit), size_t{1});
}

// Tests alone pick the tier: With every HDR and SDR10 test failing, the only
// commit is the SDR8 last resort.
UMBRIEL_TEST(tierTestsDoNotCommit) {
  MockOps m;

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;
  p.tryVrrOn = true;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.committedTier == FormatTier::Sdr8);
  // 2 formats, 2 VRR passes.
  CHECK_EQ(m.count(Kind::Test), size_t{8});
  CHECK_EQ(m.count(Kind::Commit), size_t{1});
}

// Worst case: Every test passes and every commit fails, across both modes.
// Each mode spends up to two commits on HDR, two on SDR10, and two on SDR8.
UMBRIEL_TEST(worstCaseCommitCountIsBounded) {
  static wlr_output_mode preferred{};
  static const umbriel::OutputMode spec{1280, 720, 60000};

  MockOps m;
  m.testResults = {true};
  m.commitResults = {false};

  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;
  p.tryVrrOn = true;
  p.configuredModeSpec = &spec;
  p.preferredMode = &preferred;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(!r.committed);
  CHECK(r.usedModeFallback);
  CHECK_EQ(m.count(Kind::Commit), size_t{12});
}

// Mode fallback: The staged mode cannot commit, the preferred mode retry succeeds.
UMBRIEL_TEST(modeFallbackRetriesOnPreferredMode) {
  static wlr_output_mode preferred{};

  MockOps m;
  m.commitResults = {false, true};

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

// Mode fallback reruns the complete HDR -> SDR10 -> SDR8 sequence on the
// preferred mode, and the reasons produced by the first mode do not leak into
// the result. First mode: HDR and SDR10 fail their tests, SDR8 fails to commit.
// Preferred mode: HDR fails its tests, SDR10 commits XB30, so the result
// carries a fresh HDR reason and an empty SDR10 reason.
UMBRIEL_TEST(modeFallbackRerunsFullSequenceAndResetsReasons) {
  static wlr_output_mode preferred{};

  MockOps m;
  m.testResults = {
      false, false, // Mode 1: HDR XR30, XB30
      false, false, // Mode 1: SDR10 XR30, XB30
      false, false, // Mode 2: HDR XR30, XB30
      false, true,  // Mode 2: SDR10 XR30, XB30
  };
  m.commitResults = {false, true}; // Mode 1: SDR8 XR24. Mode 2: SDR10 XB30.

  static const umbriel::OutputMode spec{1280, 720, 60000};
  FormatSequenceParams p = sdr8Params();
  p.hdrRequested = true;
  p.imageDescAvailable = true;
  p.bitDepth = 10;
  p.configuredModeSpec = &spec;
  p.preferredMode = &preferred;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.usedModeFallback);
  CHECK(r.modeFallbackWarnedNow);
  CHECK(m.modeFallbackWarned);
  CHECK(m.stagedMode == &preferred);
  CHECK(m.imageDescCleared);
  CHECK_EQ(r.hdrFail, std::string_view{"backend rejected all 10-bit HDR render formats"});
  CHECK(r.sdr10Fail.empty());
  CHECK(r.committedTier == FormatTier::Sdr10);
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(m.count(Kind::StageHdr), size_t{4});
  CHECK_EQ(m.count(Kind::Test), size_t{8});
  CHECK_EQ(m.count(Kind::Commit), size_t{2});
}

// Mode fallback clears a first-mode SDR10 reason when the preferred mode
// commits SDR10 without HDR configured.
UMBRIEL_TEST(modeFallbackClearsSdr10ReasonOnPreferredCommit) {
  static wlr_output_mode preferred{};

  MockOps m;
  m.testResults = {false, false, true}; // Mode 1: XR30, XB30. Mode 2: XR30.
  m.commitResults = {false, true};      // Mode 1: XR24. Mode 2: XR30.

  static const umbriel::OutputMode spec{1280, 720, 60000};
  FormatSequenceParams p = sdr8Params();
  p.bitDepth = 10;
  p.configuredModeSpec = &spec;
  p.preferredMode = &preferred;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(r.committed);
  CHECK(r.usedModeFallback);
  CHECK(m.stagedMode == &preferred);
  CHECK(r.hdrFail.empty());
  CHECK(r.sdr10Fail.empty());
  CHECK_EQ(r.committedFormat, uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(m.count(Kind::StageHdr), size_t{0});
  CHECK_EQ(m.count(Kind::Commit), size_t{2});
}

// No preferred mode (headless): Nothing commits, no fallback, uncommitted.
UMBRIEL_TEST(noPreferredModeYieldsUncommitted) {
  MockOps m;
  m.commitResults = {false};

  static const umbriel::OutputMode spec{1280, 720, 60000};
  FormatSequenceParams p = sdr8Params();
  p.configuredModeSpec = &spec;
  p.preferredMode = nullptr;

  const FormatSequenceResult r = runFormatSequence(p, m.ops());

  CHECK(!r.committed);
  CHECK(!r.usedModeFallback);
}

int main() { return RUN_TESTS(); }
