#pragma once

#include <cstdint>
#include <string>

namespace umbriel {

  // Whether an output scans a client buffer out directly, which window owns it, or why the output composites instead.
  struct DirectScanoutState {
    bool active = false;
    // Why the output composites, as a snake_case name; empty while active.
    std::string reason = "no_candidate";
    // The scanned-out window; empty while inactive, or for a surface no window owns.
    std::string windowId;
    std::string appId;
    std::string title;

    bool operator==(const DirectScanoutState&) const = default;
  };

  // Queries follow every successful scene-buffer commit. Logs and subscription notifications settle for kSettleMs
  // so a cursor or notification passing over a fullscreen game does not produce an event every frame.
  class DirectScanoutTracker {
  public:
    static constexpr int kSettleMs = 500;

    enum class Observation {
      // Same as the previous frame: a pending wait keeps running.
      Unchanged,
      // A new state that differs from the settled one: (re)start the wait.
      Pending,
      // Back to the settled state: drop a pending wait.
      Settled,
    };

    // Only call after the backend accepts the scene buffer, including commits made during output reconfiguration.
    Observation recordCommit(const DirectScanoutState& state, uint32_t commitSeq) {
      ++m_commits;
      if (state.active) {
        ++m_scanoutCommits;
      }
      m_commitSeq = commitSeq;
      if (state == m_current) {
        return Observation::Unchanged;
      }
      m_current = state;
      return m_current != m_settled ? Observation::Pending : Observation::Settled;
    }

    // Adopts the observed state once it has held. True when the settled state changed.
    bool settle() {
      if (m_current == m_settled) {
        return false;
      }
      m_previous = m_settled;
      m_settled = m_current;
      return true;
    }

    [[nodiscard]] const DirectScanoutState& current() const { return m_current; }
    [[nodiscard]] const DirectScanoutState& settled() const { return m_settled; }
    [[nodiscard]] uint64_t commits() const { return m_commits; }
    [[nodiscard]] uint64_t scanoutCommits() const { return m_scanoutCommits; }
    [[nodiscard]] uint32_t commitSeq() const { return m_commitSeq; }

    // Whether the last settle is worth a log line: scanout started or stopped, or moved to another window. A change
    // between two reasons to composite is not.
    [[nodiscard]] bool settledChangeIsNotable() const {
      return m_previous.active != m_settled.active || (m_settled.active && m_previous.windowId != m_settled.windowId);
    }

  private:
    DirectScanoutState m_current;
    DirectScanoutState m_settled;
    DirectScanoutState m_previous;
    uint64_t m_commits = 0;
    uint64_t m_scanoutCommits = 0;
    uint32_t m_commitSeq = 0;
  };

} // namespace umbriel
