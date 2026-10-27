#pragma once

#include <wayland-server-core.h>

struct wlr_scene_tree;
struct wlr_xwayland_surface;
struct wlr_xwayland_surface_configure_event;

namespace umbriel {

  class Server;

  // An X11 menu, tooltip, combo dropdown, or drag icon, override-redirect or typed as one. The X client positions it in
  // layout coordinates and the compositor only shows it there, above every window. Its scene node carries no view tag,
  // so pointer hit tests find the surface without a view.
  class XwaylandUnmanaged {
  public:
    // `xsurface` must be associated with a wl_surface.
    XwaylandUnmanaged(Server& server, wlr_xwayland_surface* xsurface);
    ~XwaylandUnmanaged();

    XwaylandUnmanaged(const XwaylandUnmanaged&) = delete;
    XwaylandUnmanaged& operator=(const XwaylandUnmanaged&) = delete;

    // Places the window where its X geometry says, in layout coordinates, and draws it at the scale of the output
    // there.
    void syncPosition();
    // X pixels per layout unit in the window's surface.
    [[nodiscard]] double scale() const { return m_scale; }

  private:
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
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
    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_setGeometry{};
    wl_listener m_requestConfigure{};
    wl_listener m_commit{};
    wl_listener m_shapeChanged{};
  };

} // namespace umbriel
