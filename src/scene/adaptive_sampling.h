#pragma once

#include <cstdint>

namespace umbriel {

  // When the adaptive title bar may read the window's top edge back. A readback finishes the frame, so a burst of
  // commits pays for at most one read per kSampleEveryCommits commits and per kSampleMinIntervalMsec, and an
  // acquire fence that has not signalled is waited on only until kFenceGiveUpMsec, then read anyway: a stalled client
  // cannot strand the bar on a stale color. Pure, so the cadence and the bound are testable without a compositor.
  class AdaptiveSampling {
  public:
    enum class Action : std::uint8_t {
      // The cadence held this read back: the caller owes the edge a read once the interval allows it.
      Skip,
      // The fence has not signalled yet. Retry on the next update, or when the fallback timer fires.
      Wait,
      // This fence was already read anyway; keep what the bar shows until the client commits anew.
      GiveUp,
      Read,
      // Read even though the fence has not signalled, because the wait ran out.
      ReadAnyway,
    };

    struct Input {
      // A client commit, the only path the cadence throttles.
      bool fromCommit = false;
      bool fenceSignalled = true;
      // Bumped by the caller on every commit, so an abandoned fence is abandoned for that commit alone.
      std::uint64_t generation = 0;
      std::uint64_t nowMsec = 0;
    };

    [[nodiscard]] Action decide(const Input& input) const;
    void record(Action action, const Input& input);
    // A commit that left the edge alone ends the burst, so the next repaint of it is the first of a new one and reads.
    void noteQuietCommit();
    void reset();

    // One read per two commits, and never sooner than the interval while commits stay dense, so a pathological commit
    // rate cannot become one readback per commit.
    static constexpr int kSampleEveryCommits = 2;
    static constexpr std::uint64_t kSampleMinIntervalMsec = 8;
    // How long a commit's acquire fence may stay unsignalled before its edge is read anyway.
    static constexpr std::uint64_t kFenceGiveUpMsec = 33;

  private:
    int m_commitsSinceRead = 0;
    std::uint64_t m_lastReadMsec = 0;
    bool m_haveRead = false;
    bool m_fenceWaiting = false;
    std::uint64_t m_fenceSinceMsec = 0;
    bool m_gaveUp = false;
    std::uint64_t m_gaveUpGeneration = 0;
  };

  inline AdaptiveSampling::Action AdaptiveSampling::decide(const Input& input) const {
    if (!input.fenceSignalled) {
      // A commit brings a new generation, so this covers the redraws and timers that follow an abandoned fence.
      if (m_gaveUp && m_gaveUpGeneration == input.generation) {
        return Action::GiveUp;
      }
      if (m_fenceWaiting && input.nowMsec - m_fenceSinceMsec >= kFenceGiveUpMsec) {
        return Action::ReadAnyway;
      }
      return Action::Wait;
    }
    // A mode, surface, config or title change is not a commit and is never worth delaying.
    if (!input.fromCommit || !m_haveRead) {
      return Action::Read;
    }
    const bool commitsDue = m_commitsSinceRead + 1 >= kSampleEveryCommits;
    const bool intervalDue = input.nowMsec - m_lastReadMsec >= kSampleMinIntervalMsec;
    return commitsDue && intervalDue ? Action::Read : Action::Skip;
  }

  inline void AdaptiveSampling::record(Action action, const Input& input) {
    if (input.fenceSignalled) {
      m_fenceWaiting = false;
      m_gaveUp = false;
    }
    switch (action) {
    case Action::Skip:
      ++m_commitsSinceRead;
      return;
    case Action::Wait:
      if (!m_fenceWaiting) {
        m_fenceSinceMsec = input.nowMsec;
        m_fenceWaiting = true;
      }
      return;
    case Action::GiveUp:
      return;
    case Action::Read:
    case Action::ReadAnyway:
      m_commitsSinceRead = 0;
      m_lastReadMsec = input.nowMsec;
      // The read consumed the wait, signalled or not; the next unsignalled commit starts its own.
      m_fenceWaiting = false;
      // A read that answered a state change starts a new burst, whose first commit reads.
      m_haveRead = input.fromCommit;
      m_gaveUp = action == Action::ReadAnyway;
      m_gaveUpGeneration = input.generation;
      return;
    }
  }

  inline void AdaptiveSampling::noteQuietCommit() {
    m_haveRead = false;
    m_commitsSinceRead = 0;
  }

  inline void AdaptiveSampling::reset() {
    m_commitsSinceRead = 0;
    m_lastReadMsec = 0;
    m_haveRead = false;
    m_fenceWaiting = false;
    m_fenceSinceMsec = 0;
    m_gaveUp = false;
    m_gaveUpGeneration = 0;
  }

} // namespace umbriel
