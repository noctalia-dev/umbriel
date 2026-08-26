#include "check.h"
#include "output/frame_schedule.h"

using umbriel::OutputFrameFollowup;
using umbriel::outputFrameFollowup;
using umbriel::TearingCommitRecovery;

UMBRIEL_TEST(nestedBackendCanScheduleFrames) {
  CHECK_EQ(outputFrameFollowup(false, nullptr, false, true), OutputFrameFollowup::Schedule);
}

UMBRIEL_TEST(activeNativeSessionCanScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::Schedule);
}

UMBRIEL_TEST(inactiveNativeSessionCannotScheduleFrames) {
  wlr_session session{};
  session.active = false;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::None);
}

UMBRIEL_TEST(stoppingCompositorCannotScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(true, &session, false, true), OutputFrameFollowup::None);
}

UMBRIEL_TEST(failedCommitUsesDelayedRetry) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, false), OutputFrameFollowup::RetryDelayed);
}

UMBRIEL_TEST(failedAnimatedFrameDoesNotScheduleImmediately) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, true), OutputFrameFollowup::RetryDelayed);
}

UMBRIEL_TEST(idleSuccessfulFrameNeedsNoFollowup) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, false), OutputFrameFollowup::None);
}

UMBRIEL_TEST(failedTearingCommitForcesARegularRecoveryCommit) {
  TearingCommitRecovery recovery;

  CHECK(recovery.requestTearing(true));
  recovery.recordCommit(true, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));
}

UMBRIEL_TEST(failedRegularRecoveryStaysRegularUntilItSucceeds) {
  TearingCommitRecovery recovery;
  recovery.recordCommit(true, false);
  recovery.recordCommit(false, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));

  recovery.recordCommit(false, true);
  CHECK(!recovery.regularCommitPending());
  CHECK(recovery.requestTearing(true));
}

UMBRIEL_TEST(rejectedTearingTestKeepsFailedRegularFallbackOnTheRegularPath) {
  TearingCommitRecovery recovery;
  recovery.forceRegularCommit();
  recovery.recordCommit(false, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));

  recovery.recordCommit(false, true);
  CHECK(recovery.requestTearing(true));
}

UMBRIEL_TEST(ineligibleFramesNeverRequestTearing) {
  TearingCommitRecovery recovery;
  CHECK(!recovery.requestTearing(false));
  recovery.recordCommit(false, true);
  CHECK(!recovery.regularCommitPending());
}

int main() { return RUN_TESTS(); }
