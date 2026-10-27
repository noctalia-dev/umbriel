#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <pixman.h>
#include <unordered_map>
#include <wayland-server-core.h>
#include <xcb/xcb.h>

struct wlr_surface;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct xcb_connection_t;

namespace umbriel {

  // Reads SHAPE on the existing window-manager connection. Requests and replies stay asynchronous so an X client
  // cannot stall the Wayland event loop, and no extra X connection keeps the lazy server alive.
  class XwaylandShapes {
  public:
    explicit XwaylandShapes(wl_display* display);
    ~XwaylandShapes();

    XwaylandShapes(const XwaylandShapes&) = delete;
    XwaylandShapes& operator=(const XwaylandShapes&) = delete;

    void start(wlr_xwayland* xwayland);
    void stop();
    void add(wlr_xwayland_surface* surface);
    void remove(wlr_xwayland_surface* surface);
    [[nodiscard]] bool handleEvent(xcb_generic_event_t* event);
    // X surface-local coordinates. NULL means unshaped; an empty region also holds an initial pending query back.
    [[nodiscard]] const pixman_region32_t* shape(wlr_surface* surface) const;
    [[nodiscard]] wl_signal* changed() { return &m_changed; }

  private:
    struct Surface;
    static int onTimer(void* data);
    void poll();
    void request(Surface& surface);
    void discard(Surface& surface);
    void resolve(Surface& surface, bool shaped);
    void notify(uint32_t window);
    void arm();
    void wakeWindowManager();

    wlr_xwayland* m_xwayland = nullptr;
    xcb_connection_t* m_connection = nullptr;
    wl_event_source* m_timer = nullptr;
    uint32_t m_extensionCookie = 0;
    uint32_t m_wakeupWindow = 0;
    uint8_t m_firstEvent = 0;
    bool m_available = false;
    std::chrono::steady_clock::time_point m_extensionStarted;
    std::unordered_map<uint32_t, std::unique_ptr<Surface>> m_surfaces;
    wl_signal m_changed{};
  };

} // namespace umbriel
