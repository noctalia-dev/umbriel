#pragma once

#include <chrono>

namespace umbriel {

  enum class FullscreenRequestDisposition {
    Acknowledge,
    Park,
    Apply,
  };

  class DeferredUnfullscreen {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto kGrace = std::chrono::milliseconds(750);

    [[nodiscard]] FullscreenRequestDisposition
    observeClientRequest(bool requested, bool activated, bool scheduledFullscreen, TimePoint now = Clock::now()) {
      if (requested == scheduledFullscreen) {
        clear();
        return FullscreenRequestDisposition::Acknowledge;
      }
      if (!requested && !activated) {
        m_pending = true;
        m_parkedAt = now;
        return FullscreenRequestDisposition::Park;
      }
      clear();
      return FullscreenRequestDisposition::Apply;
    }

    [[nodiscard]] bool takeOnActivation(TimePoint now = Clock::now()) {
      if (!m_pending) {
        return false;
      }
      m_pending = false;
      return now - m_parkedAt <= kGrace;
    }

    void clear() { m_pending = false; }
    [[nodiscard]] bool pending() const { return m_pending; }

  private:
    bool m_pending = false;
    TimePoint m_parkedAt;
  };

} // namespace umbriel
