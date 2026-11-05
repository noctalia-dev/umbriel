#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace umbriel {

  enum class LidState : uint8_t {
    Open,
    Closed,
  };

  // Coalesces lid sources into one state transition. Sources disappear while
  // libinput is suspended, so the last published state intentionally survives
  // an interval with no sources.
  class LidStateCoordinator {
  public:
    using SourceId = uint64_t;

    [[nodiscard]] SourceId addSource(LidState initial);
    void updateSource(SourceId source, LidState state);
    void removeSource(SourceId source);
    void setReady() { m_ready = true; }

    // Returns a state only when its command should run. Call this after a
    // backend event batch has settled so an inferred initial Open can be
    // replaced by libinput's queued initial Closed event first.
    [[nodiscard]] std::optional<LidState> takeTransition();

  private:
    struct Source {
      SourceId id = 0;
      LidState state = LidState::Open;
    };

    std::optional<LidState> aggregate() const;

    SourceId m_nextSource = 1;
    std::vector<Source> m_sources;
    std::optional<LidState> m_published;
    bool m_ready = false;
  };

} // namespace umbriel
