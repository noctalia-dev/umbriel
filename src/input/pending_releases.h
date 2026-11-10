#pragma once

#include "config/keybind_parse.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace umbriel {

  // Pending releases keyed by source and delivered key/button code.
  class PendingReleases {
  public:
    void armRelease(const void* source, uint32_t code, const Keybind& bind) {
      for (auto& entry : m_releases) {
        if (entry.source == source && entry.code == code) {
          entry.bind = bind;
          return;
        }
      }

      m_releases.push_back({.source = source, .code = code, .bind = bind});
    }

    const Keybind* releaseBind(const void* source, uint32_t code) const {
      for (const auto& entry : m_releases) {
        if (entry.source == source && entry.code == code) {
          return &entry.bind;
        }
      }

      return nullptr;
    }

    std::optional<Keybind> takeRelease(const void* source, uint32_t code) {
      for (auto it = m_releases.begin(); it != m_releases.end(); ++it) {
        if (it->source == source && it->code == code) {
          Keybind bind = std::move(it->bind);
          m_releases.erase(it);
          return bind;
        }
      }

      return std::nullopt;
    }

    void forgetSource(const void* source) {
      std::erase_if(m_releases, [source](const PendingRelease& entry) { return entry.source == source; });
    }

    void forgetPress(const void* source, uint32_t code) {
      std::erase_if(m_releases, [source, code](const PendingRelease& entry) {
        return entry.source == source && entry.code == code;
      });
    }

    // Cancel pending actions. Press/release consumption is tracked separately by the input handlers.
    void clear() { m_releases.clear(); }

    // Capture transitions retire redirected releases and modifier taps, whose cancelling input may be captured.
    void clearForCapture(bool captureKeyboard, bool capturePointer) {
      std::erase_if(m_releases, [captureKeyboard, capturePointer](const PendingRelease& entry) {
        if (entry.bind.modifierOnly && (captureKeyboard || capturePointer)) {
          return true;
        }

        return entry.bind.mouseButton != 0 ? capturePointer : captureKeyboard;
      });
    }

    // Modifier-only binds are release binds that any other input cancels. A cancelling key must not rearm a tap.
    bool cancelForInput() {
      return std::erase_if(m_releases, [](const PendingRelease& entry) { return entry.bind.modifierOnly; }) > 0;
    }

    // Keep only snapshots that are still present unchanged in the current configuration.
    void reconcileReleaseBinds(std::span<const Keybind> binds) {
      std::erase_if(m_releases, [binds](const PendingRelease& entry) {
        return std::ranges::find(binds, entry.bind) == binds.end();
      });
    }

  private:
    struct PendingRelease {
      const void* source;
      uint32_t code;
      Keybind bind;
    };

    std::vector<PendingRelease> m_releases;
  };

} // namespace umbriel
