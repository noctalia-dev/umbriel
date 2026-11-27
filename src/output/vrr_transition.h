#pragma once

#include <optional>

namespace umbriel {

  // Adaptive sync changes ride a frame commit, which cannot modeset. Some drivers need a full modeset to switch VRR
  // (an i915 eDP panel changes its vblank start and context latency), so the backend test rejects the change. A
  // rejected change is remembered until the mode, the output configuration, or the HDR state changes; otherwise every
  // focus, title, or fullscreen change would test and log it again.
  class VrrTransition {
  public:
    bool shouldTry(bool wanted, bool live) const { return wanted != live && m_rejected != wanted; }

    void reject(bool wanted) { m_rejected = wanted; }

    void reset() { m_rejected.reset(); }

  private:
    std::optional<bool> m_rejected;
  };

} // namespace umbriel
