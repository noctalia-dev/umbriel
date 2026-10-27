#pragma once

#include "xwayland/outputs.h"
#include "xwayland/xsettings.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <pixman.h>
#include <string>
#include <sys/types.h>
#include <vector>
#include <wayland-server-core.h>
#include <xcb/xcb.h>

struct wl_client;
struct wl_global;
struct wlr_surface;
struct wlr_xcursor_manager;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_xwayland_surface_configure_event;
struct xcb_connection_t;

namespace umbriel {

  class Server;
  class XwaylandWindow;
  class XwaylandShapes;

  // Configure the window as the client asked; fields the request leaves out keep their current value.
  void grantConfigureRequest(wlr_xwayland_surface* xsurface, const wlr_xwayland_surface_configure_event* event);

  // The wlroots Xwayland server. It starts lazily: the X display socket exists from construction, and the Xwayland
  // process spawns when the first X client connects. Every X11 window it announces gets an XwaylandWindow.
  class Xwayland {
  public:
    explicit Xwayland(Server& server);
    ~Xwayland();

    Xwayland(const Xwayland&) = delete;
    Xwayland& operator=(const Xwayland&) = delete;

    // False when wlroots could not create the server; the caller drops the object.
    [[nodiscard]] bool available() const { return m_wlr != nullptr; }
    // The value children get as DISPLAY.
    [[nodiscard]] const char* displayName() const;
    [[nodiscard]] bool setInputSocket(int fd);
    [[nodiscard]] bool authenticateInputPeer(pid_t peer) const;
    // The cursor X clients see over their windows until they set their own. The buffer belongs to `manager`, so this
    // must run again before a replaced manager is destroyed. Also republishes the cursor theme in XSETTINGS.
    void applyCursor(wlr_xcursor_manager* manager);
    // Called by a window whose X11 surface was destroyed; deletes it.
    void forget(XwaylandWindow* window);
    // Mapping between layout and X coordinates.
    [[nodiscard]] const XwaylandOutputs& outputs() const { return *m_outputs; }
    // X pixels per layout unit in `surface`: the scale its X11 window is drawn at. 1 for every other surface.
    [[nodiscard]] double surfaceScale(wlr_surface* surface) const;
    // Bounding shape in X surface-local coordinates. NULL means unshaped; an empty region cuts all pixels away.
    // A new surface remains empty until its initial asynchronous query completes.
    [[nodiscard]] const pixman_region32_t* surfaceShape(wlr_surface* surface) const;
    // Emits wlr_surface* after a bounding-shape query or removal changes its rendering mask.
    [[nodiscard]] wl_signal* shapeChanged() const;
    // Cursor surfaces have no wlr_xwayland_surface role. Identify them by the
    // Wayland client owned by this Xwayland server instead.
    [[nodiscard]] bool ownsSurface(const wlr_surface* surface) const;
    // Whether an xdg-output manager global may be advertised to `client`.
    [[nodiscard]] bool advertiseOutputManager(const wl_client* client, const wl_global* global) const;
    // Republishes the X screen and moves every X11 window to match it.
    void handleOutputLayoutChange();
    // An Xwayland surface just yielded the keyboard to a native Wayland client. Clear both X focus channels after the
    // Wayland enter has reached the client, so the native client holds keyboard focus before X11 loses it.
    void scheduleFocusClear();

  private:
    static void onServerStart(wl_listener* listener, void* data);
    static void onClientDestroy(wl_listener* listener, void* data);
    static bool onXEvent(wlr_xwayland* xwayland, xcb_generic_event_t* event);
    static void onReady(wl_listener* listener, void* data);
    static void onNewSurface(wl_listener* listener, void* data);
    static void onKeyboardFocusChange(wl_listener* listener, void* data);
    static int onSelectionRefreshTimer(void* data);
    static int onFocusClearTimer(void* data);
    void handleReady();
    void handleNewSurface(wlr_xwayland_surface* xsurface);
    void handleKeyboardFocusChange(void* data);
    void clearFocus();
    // Owns the XSETTINGS selection of the running Xwayland and publishes the current settings in it.
    void startXsettings(uint32_t selectionAtom, uint32_t managerAtom);
    // Rewrites the settings property when the values announced to X11 clients changed.
    void publishXsettings();

    Server& m_server;
    wlr_xwayland* m_wlr = nullptr;
    bool m_nativeResolution = false;
    bool m_xsettingsEnabled = true;
    std::optional<std::string> m_inheritedEiSocket;
    bool m_ownsEiSocketEnvironment = false;
    uint64_t m_inputSocketDevice = 0;
    uint64_t m_inputSocketInode = 0;
    std::unique_ptr<XwaylandOutputs> m_outputs;
    std::unique_ptr<XwaylandShapes> m_shapes;
    std::vector<std::unique_ptr<XwaylandWindow>> m_windows;
    uint32_t m_netActiveWindow = 0;
    // The XSETTINGS manager lives on the window manager's connection, so it neither keeps the lazy Xwayland running
    // nor outlives it. `m_xsettingsConnection` tells whether the window belongs to the current Xwayland.
    xcb_connection_t* m_xsettingsConnection = nullptr;
    uint32_t m_xsettingsWindow = 0;
    uint32_t m_xsettingsSettingsAtom = 0;
    uint32_t m_xsettingsSerial = 0;
    XsettingsValues m_xsettings;
    wl_event_source* m_focusClearTimer = nullptr;
    wl_event_source* m_selectionRefreshTimer = nullptr;
    wl_listener m_serverStart{};
    wl_listener m_clientDestroy{};
    wl_listener m_ready{};
    wl_listener m_newSurface{};
    wl_listener m_keyboardFocusChange{};
  };

} // namespace umbriel
