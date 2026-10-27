#include "check.h"
#include "output/direct_scanout.h"

#include <cstdint>
#include <limits>

using umbriel::DirectScanoutState;
using umbriel::DirectScanoutTracker;
using Observation = umbriel::DirectScanoutTracker::Observation;

namespace {
  DirectScanoutState scannedOut(const char* windowId) {
    DirectScanoutState state;
    state.active = true;
    state.reason.clear();
    state.windowId = windowId;
    state.appId = "steam_app_223850";
    return state;
  }

  DirectScanoutState composited(const char* reason) {
    DirectScanoutState state;
    state.reason = reason;
    return state;
  }
} // namespace

UMBRIEL_TEST(startsInactiveWithoutACandidate) {
  const DirectScanoutTracker tracker;
  CHECK_EQ(tracker.current(), tracker.settled());
  CHECK(!tracker.settled().active);
  CHECK_EQ(tracker.settled().reason, std::string("no_candidate"));
  CHECK_EQ(tracker.commits(), uint64_t{0});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{0});
  CHECK_EQ(tracker.commitSeq(), uint32_t{0});
}

UMBRIEL_TEST(aNewStateStartsTheWaitOnce) {
  DirectScanoutTracker tracker;
  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 17), Observation::Pending);
  // Every later frame in the same state leaves the running wait alone, so it can expire.
  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 19), Observation::Unchanged);
  CHECK(!tracker.settled().active);
}

UMBRIEL_TEST(aCommitUpdatesQueriesBeforeTheStateSettles) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(composited("composited"), 11);
  tracker.settle();

  tracker.recordCommit(scannedOut("a"), 12);
  CHECK(tracker.current().active);
  CHECK_EQ(tracker.current().windowId, std::string("a"));
  CHECK(tracker.current().reason.empty());
  CHECK_EQ(tracker.commitSeq(), uint32_t{12});
  CHECK(!tracker.settled().active);

  tracker.recordCommit(composited("software_cursor"), 13);
  CHECK(!tracker.current().active);
  CHECK_EQ(tracker.current().reason, std::string("software_cursor"));
  CHECK(tracker.current().windowId.empty());
  CHECK_EQ(tracker.commitSeq(), uint32_t{13});
}

UMBRIEL_TEST(aBriefScanoutLeavesEvidenceWithoutASettledChange) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(composited("composited"), 4);
  tracker.settle();
  const uint64_t initialCommits = tracker.commits();

  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 5), Observation::Pending);
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{1});
  CHECK_EQ(tracker.recordCommit(composited("composited"), 6), Observation::Settled);

  CHECK(!tracker.current().active);
  CHECK(!tracker.settle());
  CHECK_EQ(tracker.commits(), initialCommits + 2);
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{1});
  CHECK_EQ(tracker.commitSeq(), uint32_t{6});
}

UMBRIEL_TEST(unchangedCommitsAdvanceCountersWithoutRestartingTheWait) {
  DirectScanoutTracker tracker;
  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 20), Observation::Pending);
  CHECK_EQ(tracker.commits(), uint64_t{1});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{1});

  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 21), Observation::Unchanged);
  CHECK_EQ(tracker.commits(), uint64_t{2});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{2});
  CHECK_EQ(tracker.commitSeq(), uint32_t{21});
  CHECK(tracker.settle());
  CHECK_EQ(tracker.commits(), uint64_t{2});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{2});

  CHECK_EQ(tracker.recordCommit(composited("capture"), 22), Observation::Pending);
  CHECK_EQ(tracker.commits(), uint64_t{3});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{2});
}

UMBRIEL_TEST(outputCommitSequenceCanWrapWithoutResettingCounters) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(scannedOut("a"), std::numeric_limits<uint32_t>::max());
  CHECK_EQ(tracker.commitSeq(), std::numeric_limits<uint32_t>::max());

  tracker.recordCommit(composited("composited"), 0);
  CHECK_EQ(tracker.commitSeq(), uint32_t{0});
  CHECK_EQ(tracker.commits(), uint64_t{2});
  CHECK_EQ(tracker.scanoutCommits(), uint64_t{1});
  CHECK(!tracker.current().active);
}

UMBRIEL_TEST(settlingAdoptsTheObservedState) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(scannedOut("a"), 1);
  CHECK(tracker.settle());
  CHECK(tracker.settled().active);
  CHECK_EQ(tracker.settled().windowId, std::string("a"));
  CHECK(tracker.settledChangeIsNotable());
  // Nothing new to adopt.
  CHECK(!tracker.settle());
}

UMBRIEL_TEST(flickerBackToTheSettledStateDropsTheWait) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(scannedOut("a"), 1);
  tracker.settle();
  // A cursor shown for one frame over the game.
  CHECK_EQ(tracker.recordCommit(composited("software_cursor"), 2), Observation::Pending);
  CHECK_EQ(tracker.recordCommit(scannedOut("a"), 3), Observation::Settled);
  CHECK(!tracker.settle());
  CHECK(tracker.settled().active);
}

UMBRIEL_TEST(switchingReasonsWhileCompositingIsNotNotable) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(composited("composited"), 1);
  CHECK(tracker.settle());
  CHECK(!tracker.settledChangeIsNotable());
  tracker.recordCommit(composited("effects"), 2);
  CHECK(tracker.settle());
  CHECK(!tracker.settledChangeIsNotable());
  CHECK_EQ(tracker.settled().reason, std::string("effects"));
}

UMBRIEL_TEST(stoppingAndMovingWindowsAreNotable) {
  DirectScanoutTracker tracker;
  tracker.recordCommit(scannedOut("a"), 1);
  tracker.settle();
  tracker.recordCommit(scannedOut("b"), 2);
  CHECK(tracker.settle());
  CHECK(tracker.settledChangeIsNotable());
  tracker.recordCommit(composited("composited"), 3);
  CHECK(tracker.settle());
  CHECK(tracker.settledChangeIsNotable());
}

int main() { return RUN_TESTS(); }
