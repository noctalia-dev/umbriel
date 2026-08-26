#pragma once

extern "C" {
#include <wlr/backend/session.h>
}

namespace umbriel {

  enum class OutputFrameFollowup {
    None,
    Schedule,
    RetryDelayed,
  };

  [[nodiscard]] inline bool outputFrameAllowed(bool stopping, const wlr_session* session) {
    return !stopping && (session == nullptr || session->active);
  }

  [[nodiscard]] inline OutputFrameFollowup
  outputFrameFollowup(bool stopping, const wlr_session* session, bool commitFailed, bool animationsActive) {
    if (!outputFrameAllowed(stopping, session)) {
      return OutputFrameFollowup::None;
    }
    if (commitFailed) {
      return OutputFrameFollowup::RetryDelayed;
    }
    return animationsActive ? OutputFrameFollowup::Schedule : OutputFrameFollowup::None;
  }

  // An asynchronous page flip can pass the backend test and still fail at commit time. The generic frame retry must
  // submit one fresh regular state before another asynchronous attempt, otherwise a backend rejection can become a
  // self-sustaining retry loop.
  class TearingCommitRecovery {
  public:
    [[nodiscard]] bool requestTearing(bool eligible) const { return eligible && !m_regularCommitPending; }
    [[nodiscard]] bool regularCommitPending() const { return m_regularCommitPending; }

    void recordCommit(bool requestedTearing, bool succeeded) {
      if (requestedTearing && !succeeded) {
        m_regularCommitPending = true;
      } else if (!requestedTearing && succeeded) {
        m_regularCommitPending = false;
      }
    }

    void forceRegularCommit() { m_regularCommitPending = true; }

    void reset() { m_regularCommitPending = false; }

  private:
    bool m_regularCommitPending = false;
  };

} // namespace umbriel
