#pragma once

#include <cstdint>
#include <memory>
#include <wayland-server-core.h>

struct wlr_xwayland_surface;

namespace umbriel {

  class Server;
  class View;
  class Xwayland;
  class XwaylandUnmanaged;

  // A utility window transient for another X11 window that declares it never takes keyboard input is placed by its
  // client, like a context menu without override-redirect. WeChat's search suggestions are one.
  bool isPassiveUtilityPopup(const wlr_xwayland_surface* xsurface);

  // One X11 window for its whole lifetime. While it has a wl_surface, an override-redirect window, one typed as a
  // context menu, notification, tooltip, dropdown, or drag icon, or a passive utility popup is an XwaylandUnmanaged,
  // and any other window is a View; a window whose type or override-redirect flag changes swaps wrappers without
  // changing the Wayland surface's mapped state.
  class XwaylandWindow {
  public:
    XwaylandWindow(Server& server, Xwayland& owner, wlr_xwayland_surface* xsurface);
    // Detaches without touching the view: it only runs at server teardown, after the registry deleted every view.
    ~XwaylandWindow();

    XwaylandWindow(const XwaylandWindow&) = delete;
    XwaylandWindow& operator=(const XwaylandWindow&) = delete;

    // The X screen moved relative to the layout: re-place the wrapper's window.
    void syncGeometry();
    // An event-stream barrier completed for one map of this window.
    void handleXwmSync(uint64_t serial);
    void refreshCaptureProtection();
    [[nodiscard]] wlr_xwayland_surface* xsurface() const { return m_xsurface; }
    // X pixels per layout unit in the window's surface.
    [[nodiscard]] double scale() const;

  private:
    static void onAssociate(wl_listener* listener, void* data);
    static void onDissociate(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onSetOverrideRedirect(wl_listener* listener, void* data);
    static void onSetWindowType(wl_listener* listener, void* data);
    // Before associate there is no wrapper to decide, so the client's geometry request is granted.
    static void onRequestConfigure(wl_listener* listener, void* data);
    void handleAssociate();
    void handleDissociate();
    void handleDestroy();
    void updateClassification();
    void createWrapper();
    void destroyWrapper();
    void removeListeners();

    Server& m_server;
    Xwayland& m_owner;
    wlr_xwayland_surface* m_xsurface = nullptr;
    // Client geometry is in X11 pixels. Managed layout configures must not overwrite the rectangle a late popup owns.
    struct ClientGeometry {
      int16_t x;
      int16_t y;
      uint16_t width;
      uint16_t height;
    } m_clientGeometry;
    View* m_view = nullptr;
    bool m_captureBlockedBeforePopup = false;
    std::unique_ptr<XwaylandUnmanaged> m_unmanaged;
    wl_listener m_associate{};
    wl_listener m_dissociate{};
    wl_listener m_destroy{};
    wl_listener m_setOverrideRedirect{};
    wl_listener m_setWindowType{};
    wl_listener m_requestConfigure{};
  };

} // namespace umbriel
