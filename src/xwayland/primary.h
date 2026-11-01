#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <wayland-server-core.h>
#include <xcb/xcb.h>

struct wlr_xwayland;

namespace umbriel {

  // Publishes a stable RandR primary on the XWM connection, without keeping the lazy Xwayland alive or waiting for
  // X11 replies in the Wayland event loop.
  class XwaylandPrimary {
  public:
    explicit XwaylandPrimary(wl_display* display);
    ~XwaylandPrimary();

    XwaylandPrimary(const XwaylandPrimary&) = delete;
    XwaylandPrimary& operator=(const XwaylandPrimary&) = delete;

    void start(wlr_xwayland* xwayland);
    void stop();
    // nullopt leaves RandR unmanaged; an empty name manages it with no primary while the chosen monitor is absent.
    void setOutput(std::optional<std::string> name);
    [[nodiscard]] bool handleEvent(xcb_generic_event_t* event);

  private:
    struct Output {
      uint32_t id;
      uint32_t cookie;
    };

    static int onTimer(void* data);
    void poll();
    void refresh();
    void request();
    void discard();
    void wakeWindowManager();
    [[nodiscard]] bool pending() const;

    wlr_xwayland* m_xwayland = nullptr;
    xcb_connection_t* m_connection = nullptr;
    wl_event_source* m_timer = nullptr;
    std::optional<std::string> m_output;
    bool m_clear = false;
    bool m_dirty = false;
    bool m_available = false;
    uint8_t m_firstEvent = 0;
    uint32_t m_root = 0;
    uint32_t m_wakeupWindow = 0;
    uint32_t m_extensionCookie = 0;
    uint32_t m_versionCookie = 0;
    uint32_t m_resourcesCookie = 0;
    uint32_t m_primaryCookie = 0;
    uint32_t m_current = 0;
    uint32_t m_candidate = 0;
    std::vector<Output> m_outputs;
    std::chrono::steady_clock::time_point m_requested;
  };

} // namespace umbriel
