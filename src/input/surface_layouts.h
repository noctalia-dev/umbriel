#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <wayland-server-core.h>

struct wlr_surface;

namespace umbriel {

  // The named XKB layout last used by each keyboard-focusable surface when
  // input.keyboard.track_layout is "window".
  //
  // Keyed on the surface rather than on a View so layer-shell and lock-screen
  // clients receive the same behavior as toplevels. Entries remove themselves
  // through the surface destroy signal.
  class SurfaceLayoutMemory {
  public:
    SurfaceLayoutMemory() = default;
    ~SurfaceLayoutMemory();

    SurfaceLayoutMemory(const SurfaceLayoutMemory&) = delete;
    SurfaceLayoutMemory& operator=(const SurfaceLayoutMemory&) = delete;

    void remember(wlr_surface* surface, std::string_view layout);
    std::optional<std::string_view> recall(wlr_surface* surface) const;
    void forget(wlr_surface* surface);
    void clear();
    size_t size() const { return m_entries.size(); }

  private:
    struct Entry {
      SurfaceLayoutMemory* owner = nullptr;
      wlr_surface* surface = nullptr;
      std::string layout;
      wl_listener destroy{};
    };

    static void onSurfaceDestroy(wl_listener* listener, void* data);
    static void destroyEntry(Entry* entry);

    std::unordered_map<wlr_surface*, Entry*> m_entries;
  };

} // namespace umbriel
