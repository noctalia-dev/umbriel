#pragma once

#include <memory>
#include <unordered_map>
#include <wayland-server-core.h>

struct wlr_surface;

namespace umbriel {

  // Manual passthrough belongs to an exact keyboard-focusable surface. The
  // surface's destruction removes its state, even when it is not focused.
  class ManualShortcutsInhibit {
  public:
    ManualShortcutsInhibit() = default;
    ~ManualShortcutsInhibit();

    ManualShortcutsInhibit(const ManualShortcutsInhibit&) = delete;
    ManualShortcutsInhibit& operator=(const ManualShortcutsInhibit&) = delete;

    bool enabled(wlr_surface* surface) const;
    void enable(wlr_surface* surface);
    void disable(wlr_surface* surface);
    void clear();

  private:
    struct Entry {
      ManualShortcutsInhibit* owner = nullptr;
      wlr_surface* surface = nullptr;
      wl_listener destroy{};
    };

    static void onSurfaceDestroy(wl_listener* listener, void* data);

    std::unordered_map<wlr_surface*, std::unique_ptr<Entry>> m_entries;
  };

} // namespace umbriel
