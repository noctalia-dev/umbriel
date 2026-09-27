#pragma once

#include <algorithm>
#include <vector>

namespace umbriel {

  // What decides whether an effect instance asks for frames: it is drawn on an
  // output, its program reads umbriel_time, and its clock is advancing. Frozen
  // snapshots and effects with `animated = false` or `speed = 0` arrive with
  // advancing = false.
  struct EffectInstanceState {
    const void* output = nullptr;
    bool visible = false;
    bool readsTime = false;
    bool advancing = false;
  };

  // Per-output counts hot paths check before any lookup. Owners are opaque
  // identities (a View, an output effect), so this stays free of scene types.
  class EffectLedger {
  public:
    void update(const void* owner, const EffectInstanceState& state) {
      const auto entry = std::ranges::find(m_entries, owner, &Entry::owner);
      if (entry == m_entries.end()) {
        m_entries.push_back({owner, state});
      } else {
        entry->state = state;
      }
    }
    void remove(const void* owner) {
      std::erase_if(m_entries, [owner](const Entry& entry) { return entry.owner == owner; });
    }
    void removeOutput(const void* output) {
      std::erase_if(m_entries, [output](const Entry& entry) { return entry.state.output == output; });
    }
    void setSuspended(bool suspended) { m_suspended = suspended; }
    [[nodiscard]] bool suspended() const { return m_suspended; }
    // Instances on `output` that need effect-only frames right now.
    [[nodiscard]] unsigned eligible(const void* output) const {
      if (m_suspended) {
        return 0;
      }
      return static_cast<unsigned>(std::ranges::count_if(m_entries, [output](const Entry& entry) {
        return entry.state.output == output && entry.state.visible && entry.state.readsTime && entry.state.advancing;
      }));
    }
    // Owners carrying a persistent effect at all, visible or not.
    [[nodiscard]] unsigned active() const { return static_cast<unsigned>(m_entries.size()); }

  private:
    struct Entry {
      const void* owner;
      EffectInstanceState state;
    };
    std::vector<Entry> m_entries;
    bool m_suspended = false;
  };

} // namespace umbriel
