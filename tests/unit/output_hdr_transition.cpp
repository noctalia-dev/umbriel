#include "check.h"
#include "output/hdr_transition.h"
#include "output/page_flip_retry.h"
#include "output/rendered_state_commit.h"

#include <cstdint>
#include <optional>
#include <vector>

extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
}

using umbriel::commitRenderedState;
using umbriel::HdrTransition;
using umbriel::PageFlipRetry;
using umbriel::RenderedStateCommitResult;

namespace {

  void destroyMockBuffer(wlr_buffer* buffer) { wlr_buffer_finish(buffer); }

  constexpr wlr_buffer_impl kMockBufferImpl{
      .destroy = destroyMockBuffer,
      .get_dmabuf = nullptr,
      .get_shm = nullptr,
      .begin_data_ptr_access = nullptr,
      .end_data_ptr_access = nullptr,
  };

} // namespace

UMBRIEL_TEST(changedRequestStaysPendingUntilCommitted) {
  HdrTransition transition;

  CHECK(transition.pending(true, true));
  CHECK(!transition.committedRequest());

  transition.recordCommit(true, true, true);

  CHECK(!transition.pending(true, true));
  CHECK(transition.committedRequest());
}

UMBRIEL_TEST(failedAttemptRemainsPendingForRetry) {
  HdrTransition transition;

  CHECK(transition.pending(true, true));
  transition.recordCommit(true, true, false);

  CHECK(transition.pending(true, true));
  CHECK(!transition.committedRequest());
}

UMBRIEL_TEST(requestReturningToCommittedStateCancelsTransition) {
  HdrTransition transition;

  CHECK(transition.pending(true, true));
  CHECK(!transition.pending(true, false));
}

UMBRIEL_TEST(disabledOutputDefersTransitionUntilEnabled) {
  HdrTransition transition;

  CHECK(!transition.pending(false, true));
  CHECK(transition.pending(true, true));
}

UMBRIEL_TEST(disabledCommitDoesNotAdvanceTransition) {
  HdrTransition transition;

  transition.recordCommit(false, true, true);

  CHECK(transition.pending(true, true));
  CHECK(!transition.committedRequest());
}

UMBRIEL_TEST(pageFlipRetryDefersOnlyNonBlockingCommitsWhileFlipPending) {
  const PageFlipRetry retry;

  CHECK(retry.shouldDefer(true, false));
  CHECK(!retry.shouldDefer(false, false));
  CHECK(!retry.shouldDefer(true, true));
}

UMBRIEL_TEST(pageFlipRetryKeepsGeometryAcrossMergedDeferrals) {
  PageFlipRetry retry;

  CHECK(!retry.take().has_value());
  retry.defer(true);
  retry.defer(false);
  CHECK(retry.take() == std::optional(true));
  CHECK(!retry.take().has_value());
}

UMBRIEL_TEST(pageFlipRetryStopsDeferringAfterMaxAttemptsUntilReset) {
  PageFlipRetry retry;

  for (uint8_t attempt = 0; attempt < PageFlipRetry::kMaxFrameAttempts; ++attempt) {
    CHECK(retry.shouldDefer(true, false));
    retry.defer(false);
    CHECK(retry.take().has_value());
  }
  CHECK(!retry.shouldDefer(true, false));

  retry.reset();
  CHECK(retry.shouldDefer(true, false));
}

UMBRIEL_TEST(renderedStateBuildsBeforeCommitAndCarriesBuffer) {
  wlr_output_state state{};
  wlr_output_state_init(&state);
  wlr_buffer buffer{};
  wlr_buffer_init(&buffer, &kMockBufferImpl, 1, 1);
  state.allow_reconfiguration = true;
  std::vector<int> order;

  const RenderedStateCommitResult result = commitRenderedState(
      state,
      [&](wlr_output_state& pending) {
        order.push_back(1);
        wlr_output_state_set_buffer(&pending, &buffer);
        return true;
      },
      [&](const wlr_output_state& pending) {
        order.push_back(2);
        CHECK((pending.committed & WLR_OUTPUT_STATE_BUFFER) != 0);
        CHECK(pending.buffer != nullptr);
        CHECK(pending.allow_reconfiguration);
        return true;
      }
  );

  CHECK_EQ(result, RenderedStateCommitResult::Committed);
  CHECK_EQ(order.size(), size_t{2});
  CHECK_EQ(order[0], 1);
  CHECK_EQ(order[1], 2);
  wlr_output_state_finish(&state);
  wlr_buffer_drop(&buffer);
}

UMBRIEL_TEST(renderedStateRefusesCommitWithoutSceneBuffer) {
  wlr_output_state state{};
  wlr_output_state_init(&state);
  bool commitCalled = false;

  const RenderedStateCommitResult result = commitRenderedState(
      state, [](wlr_output_state&) { return true; },
      [&](const wlr_output_state&) {
        commitCalled = true;
        return true;
      }
  );

  CHECK_EQ(result, RenderedStateCommitResult::MissingBuffer);
  CHECK(!commitCalled);
  wlr_output_state_finish(&state);
}

UMBRIEL_TEST(renderedStateRefusesNullBufferBehindCommittedFlag) {
  wlr_output_state state{};
  wlr_output_state_init(&state);
  bool commitCalled = false;

  const RenderedStateCommitResult result = commitRenderedState(
      state,
      [](wlr_output_state& pending) {
        pending.committed |= WLR_OUTPUT_STATE_BUFFER;
        return true;
      },
      [&](const wlr_output_state&) {
        commitCalled = true;
        return true;
      }
  );

  CHECK_EQ(result, RenderedStateCommitResult::MissingBuffer);
  CHECK(!commitCalled);
  state.committed &= ~WLR_OUTPUT_STATE_BUFFER;
  wlr_output_state_finish(&state);
}

int main() { return RUN_TESTS(); }
