#include "scene/adaptive_sampling.h"

#include "check.h"

#include <cstdint>

namespace {
  using umbriel::AdaptiveSampling;

  AdaptiveSampling::Input commit(std::uint64_t nowMsec, bool fenceSignalled = true, std::uint64_t generation = 1) {
    return {.fromCommit = true, .fenceSignalled = fenceSignalled, .generation = generation, .nowMsec = nowMsec};
  }

  AdaptiveSampling::Input stateChange(std::uint64_t nowMsec) {
    return {.fromCommit = false, .fenceSignalled = true, .generation = 1, .nowMsec = nowMsec};
  }

  AdaptiveSampling::Input redraw(std::uint64_t nowMsec, bool fenceSignalled = true, std::uint64_t generation = 1) {
    return {.fromCommit = false, .fenceSignalled = fenceSignalled, .generation = generation, .nowMsec = nowMsec};
  }
} // namespace

UMBRIEL_TEST(firstCommitOfABurstReads) {
  AdaptiveSampling sampling;
  CHECK(sampling.decide(commit(100)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(secondCommitOfABurstIsSkipped) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(100));
  CHECK(sampling.decide(commit(116)) == AdaptiveSampling::Action::Skip);
  sampling.record(AdaptiveSampling::Action::Skip, commit(116));
  // Two commits past the read, and the interval with them: the skipped frame's edge is read now.
  CHECK(sampling.decide(commit(133)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(denseCommitsWaitForTheIntervalAfterTheCommitCount) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(0));
  CHECK(sampling.decide(commit(3)) == AdaptiveSampling::Action::Skip);
  sampling.record(AdaptiveSampling::Action::Skip, commit(3));
  // The second commit is due by count, but the floor holds it back: a client committing hundreds of times a second
  // cannot turn this into one readback per commit.
  CHECK(sampling.decide(commit(4)) == AdaptiveSampling::Action::Skip);
  sampling.record(AdaptiveSampling::Action::Skip, commit(4));
  CHECK(sampling.decide(commit(9)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(aQuietCommitEndsTheBurst) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(0));
  // A commit that does not touch the edge is not part of the burst: the next repaint of it reads, so a single repaint
  // after the application went quiet still lands on the bar.
  sampling.noteQuietCommit();
  CHECK(sampling.decide(commit(2)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(stateChangesAlwaysReadAndRestartTheBurst) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(0));
  CHECK(sampling.decide(stateChange(1)) == AdaptiveSampling::Action::Read);
  sampling.record(AdaptiveSampling::Action::Read, stateChange(1));
  // The next commit is the first of a new burst, so an idle window that repaints once follows immediately.
  CHECK(sampling.decide(commit(2)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(anUnsignalledFenceWaitsUntilItsBound) {
  AdaptiveSampling sampling;
  CHECK(sampling.decide(commit(0, false)) == AdaptiveSampling::Action::Wait);
  sampling.record(AdaptiveSampling::Action::Wait, commit(0, false));
  CHECK(sampling.decide(commit(16, false)) == AdaptiveSampling::Action::Wait);
  sampling.record(AdaptiveSampling::Action::Wait, commit(16, false));
  CHECK(sampling.decide(commit(AdaptiveSampling::kFenceGiveUpMsec, false)) == AdaptiveSampling::Action::ReadAnyway);
}

UMBRIEL_TEST(anAbandonedFenceIsNotReadAgainForTheSameCommit) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Wait, commit(0, false));
  const AdaptiveSampling::Input bound = commit(AdaptiveSampling::kFenceGiveUpMsec, false);
  CHECK(sampling.decide(bound) == AdaptiveSampling::Action::ReadAnyway);
  sampling.record(AdaptiveSampling::Action::ReadAnyway, bound);
  // A redraw must not read the same unfinished commit over and over; only a commit starts a new generation.
  CHECK(sampling.decide(redraw(AdaptiveSampling::kFenceGiveUpMsec + 40, false)) == AdaptiveSampling::Action::GiveUp);
  // The client's next commit brings a new fence, which gets its own wait.
  CHECK(sampling.decide(commit(AdaptiveSampling::kFenceGiveUpMsec + 50, false, 2)) == AdaptiveSampling::Action::Wait);
}

UMBRIEL_TEST(aSkippedCommitIsReadWhenTheBurstEnds) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(0));
  CHECK(sampling.decide(commit(4)) == AdaptiveSampling::Action::Skip);
  sampling.record(AdaptiveSampling::Action::Skip, commit(4));
  // The client stops after the skipped commit: the fallback timer's update has to read the frame the cadence owed.
  CHECK(sampling.decide(redraw(AdaptiveSampling::kSampleMinIntervalMsec)) == AdaptiveSampling::Action::Read);
}

UMBRIEL_TEST(aFenceThatSignalsEndsTheWait) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Wait, commit(0, false));
  CHECK(sampling.decide(commit(10)) == AdaptiveSampling::Action::Read);
  sampling.record(AdaptiveSampling::Action::Read, commit(10));
  // With the fence signalled the give-up is gone, so a later unsignalled commit waits from its own start.
  CHECK(sampling.decide(commit(20, false, 2)) == AdaptiveSampling::Action::Wait);
}

UMBRIEL_TEST(resetForgetsTheBurstAndTheAbandonedFence) {
  AdaptiveSampling sampling;
  sampling.record(AdaptiveSampling::Action::Read, commit(100));
  CHECK(sampling.decide(commit(101)) == AdaptiveSampling::Action::Skip);
  sampling.reset();
  CHECK(sampling.decide(commit(102)) == AdaptiveSampling::Action::Read);
  CHECK(sampling.decide(commit(103, false)) == AdaptiveSampling::Action::Wait);
}

int main() { return RUN_TESTS(); }
