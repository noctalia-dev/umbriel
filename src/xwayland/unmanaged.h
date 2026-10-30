#pragma once

#include <cstdint>
#include <wayland-server-core.h>

struct wlr_scene_tree;
struct wlr_xwayland_surface;
struct wlr_xwayland_surface_configure_event;

namespace umbriel {

  class Server;

  // An X11 context menu, notification, tooltip, combo dropdown, drag icon, or passive utility popup, override-redirect
  // or typed as one. The X client positions it in layout coordinates and the compositor only shows it there, above
  // every window. Its scene node carries no view tag, so pointer hit tests find the surface without a view.
  class XwaylandUnmanaged {
  public:
    // `xsurface` must be associated with a wl_surface.
    XwaylandUnmanaged(Server& server, wlr_xwayland_surface* xsurface, bool captureBlocked);
    ~XwaylandUnmanaged();

    XwaylandUnmanaged(const XwaylandUnmanaged&) = delete;
    XwaylandUnmanaged& operator=(const XwaylandUnmanaged&) = delete;

    // Places the window where its X geometry says, in layout coordinates, and draws it at the scale of the output
    // there.
    void syncPosition();
    // Updated metadata can withdraw keyboard focus without remapping or raising the popup.
    void refreshFocusPolicy();
    // Reveal only the map whose earlier X geometry events have passed through the window manager.
    void handleXwmSync(uint64_t serial);
    void refreshCaptureProtection();
    // X pixels per layout unit in the window's surface.
    [[nodiscard]] double scale() const { return m_scale; }

  private:
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onSetParent(wl_listener* listener, void* data);
    static void onSetGeometry(wl_listener* listener, void* data);
    static void onRequestConfigure(wl_listener* listener, void* data);
    static void onCommit(wl_listener* listener, void* data);
    static void onShapeChanged(wl_listener* listener, void* data);
    void handleMap();
    void handleUnmap();
    void handleSetGeometry();
    void handleRequestConfigure(const wlr_xwayland_surface_configure_event* event);
    void updateShape();
    // Hand the keyboard back to the activated view when this window holds it.
    void releaseKeyboard();

    Server& m_server;
    wlr_xwayland_surface* m_xsurface = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    double m_scale = 1.0;
    uint64_t m_pendingXwmSync = 0;
    bool m_captureBlockedBeforePopup = false;
    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_setGeometry{};
    wl_listener m_setParent{};
    wl_listener m_requestConfigure{};
    wl_listener m_commit{};
    wl_listener m_shapeChanged{};
  };

} // namespace umbriel
