#include "config/config.h"
#include "output/output.h"
#include "server/server.h"
#include "view/floating.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "wlr.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Every branch between the xdg-shell and the X11 role of a View lives here.

namespace umbriel {

  wlr_surface* View::rootSurface() const {
    return m_toplevel != nullptr ? m_toplevel->base->surface : m_xsurface->surface;
  }

  const char* View::title() const { return m_toplevel != nullptr ? m_toplevel->title : m_xsurface->title; }

  const char* View::appId() const { return m_toplevel != nullptr ? m_toplevel->app_id : m_xsurface->class_; }

  bool View::scheduledFullscreen() const {
    return m_toplevel != nullptr ? m_toplevel->scheduled.fullscreen : m_xState.fullscreen;
  }

  bool View::currentFullscreen() const {
    return m_toplevel != nullptr ? m_toplevel->current.fullscreen : m_xState.fullscreen;
  }

  bool View::scheduledMaximized() const {
    return m_toplevel != nullptr ? m_toplevel->scheduled.maximized : m_xState.maximized;
  }

  bool View::currentMaximized() const {
    return m_toplevel != nullptr ? m_toplevel->current.maximized : m_xState.maximized;
  }

  bool View::scheduledActivated() const {
    return m_toplevel != nullptr ? m_toplevel->scheduled.activated : m_xState.activated;
  }

  void View::setActivatedState(bool activated) {
    if (m_toplevel != nullptr) {
      wlr_xdg_toplevel_set_activated(m_toplevel, activated);
      return;
    }
    m_xState.activated = activated;
    if (activated) {
      // An ignored WM_CHANGE_STATE request can leave Wine waiting for a WM_STATE reply even though wlroots never
      // marked the surface minimized. Reassert NormalState on activation so the client can complete its restore.
      wlr_xwayland_surface_set_minimized(m_xsurface, false);
    }
    wlr_xwayland_surface_activate(m_xsurface, activated);
  }

  void View::reclaimXwaylandFocus() {
    if (m_xsurface != nullptr) {
      wlr_xwayland_surface_activate(m_xsurface, true);
    }
  }

  void View::setSuspendedState(bool suspended) {
    // X11 has no such state: minimizing would tell the client it was iconified, which is not what a hidden tab is.
    if (m_toplevel == nullptr || !m_toplevel->base->initialized || m_toplevel->scheduled.suspended == suspended) {
      return;
    }
    wlr_xdg_toplevel_set_suspended(m_toplevel, suspended);
  }

  void View::setFullscreenState(bool fullscreen) {
    // The IPC window listing reports the scheduled state.
    m_server->scheduleIpcWindowsEvent();
    if (m_toplevel != nullptr) {
      wlr_xdg_toplevel_set_fullscreen(m_toplevel, fullscreen);
      return;
    }
    m_xState.fullscreen = fullscreen;
    m_xFullscreenSize.reset();
    wlr_xwayland_surface_set_fullscreen(m_xsurface, fullscreen);
  }

  void View::setMaximizedState(bool maximized) {
    m_server->scheduleIpcWindowsEvent();
    if (m_toplevel != nullptr) {
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
      return;
    }
    m_xState.maximized = maximized;
    wlr_xwayland_surface_set_maximized(m_xsurface, maximized, maximized);
  }

  void View::requestClose() {
    if (m_toplevel != nullptr) {
      wlr_xdg_toplevel_send_close(m_toplevel);
    } else {
      wlr_xwayland_surface_close(m_xsurface);
    }
  }

  wlr_box View::geometryBox() const {
    if (m_toplevel != nullptr) {
      return m_toplevel->base->geometry;
    }
    const wlr_surface* surface = m_xsurface->surface;
    if (surface == nullptr) {
      return {};
    }
    return {
        .x = 0,
        .y = 0,
        .width = xToLayoutLength(surface->current.width),
        .height = xToLayoutLength(surface->current.height)
    };
  }

  SizeHints View::sizeHints() const {
    if (m_toplevel != nullptr) {
      return xdgSizeHints(m_toplevel);
    }
    SizeHints hints = x11SizeHints(m_xsurface->size_hints);
    hints.minWidth = std::max(1, xToLayoutLength(hints.minWidth));
    hints.minHeight = std::max(1, xToLayoutLength(hints.minHeight));
    hints.maxWidth = xToLayoutLength(hints.maxWidth);
    hints.maxHeight = xToLayoutLength(hints.maxHeight);
    return hints;
  }

  View::ClientSize View::scheduledSize() const {
    if (m_toplevel != nullptr) {
      return {.width = m_toplevel->scheduled.width, .height = m_toplevel->scheduled.height};
    }
    return m_xScheduled;
  }

  View::ClientSize View::currentSize() const {
    if (m_toplevel != nullptr) {
      return {.width = m_toplevel->current.width, .height = m_toplevel->current.height};
    }
    const wlr_surface* surface = m_xsurface->surface;
    if (surface == nullptr) {
      return {};
    }
    return {.width = xToLayoutLength(surface->current.width), .height = xToLayoutLength(surface->current.height)};
  }

  void View::forEachSurface(SurfaceIterator iterator, void* data) const {
    if (m_toplevel != nullptr) {
      wlr_xdg_surface_for_each_surface(m_toplevel->base, iterator, data);
    } else if (m_xsurface->surface != nullptr) {
      wlr_surface_for_each_surface(m_xsurface->surface, iterator, data);
    }
  }

  void View::forEachPopupSurface(SurfaceIterator iterator, void* data) const {
    if (m_toplevel != nullptr) {
      wlr_xdg_surface_for_each_popup_surface(m_toplevel->base, iterator, data);
    }
  }

  bool View::requestedFullscreen() const {
    return m_toplevel != nullptr ? m_toplevel->requested.fullscreen : m_xsurface->fullscreen;
  }

  bool View::requestedMaximized() const {
    if (m_toplevel != nullptr) {
      return m_toplevel->requested.maximized;
    }
    return m_xsurface->maximized_vert && m_xsurface->maximized_horz;
  }

  uint32_t View::configureSize(int width, int height) {
    if (m_toplevel != nullptr) {
      return wlr_xdg_toplevel_set_size(m_toplevel, width, height);
    }
    sendXwaylandConfigure(width, height);
    return 0;
  }

  void View::adoptScheduledSize(int width, int height) {
    if (m_toplevel != nullptr) {
      m_toplevel->scheduled.width = width;
      m_toplevel->scheduled.height = height;
      return;
    }
    // The X window already has this size, so there is nothing to send.
    m_xScheduled = {.width = width, .height = height};
    m_xConfigured.width = m_xsurface->width;
    m_xConfigured.height = m_xsurface->height;
  }

  bool View::configureSettled(uint32_t serial) const {
    return m_toplevel == nullptr || serialSettled(m_toplevel->base->current.configure_serial, serial);
  }

  bool View::pendingConfigureSettled(uint32_t serial) const {
    return m_toplevel == nullptr || serialSettled(m_toplevel->base->pending.configure_serial, serial);
  }

  bool View::retireFloatingSizeRequest() {
    if (m_toplevel != nullptr) {
      return m_floating.retireSizeRequestIfSettled(m_toplevel->base->current.configure_serial);
    }
    // X11 configures carry no serial: the request is answered once the window commits the requested size, or any size
    // other than the one it had when the request went out.
    if (const auto& pending = m_floating.pendingSize()) {
      const ClientSize size = currentSize();
      const bool exact = size.width == (*pending)[0] && size.height == (*pending)[1];
      const bool answered = size.width != m_xFloatingRequestBase.width || size.height != m_xFloatingRequestBase.height;
      if (!exact && !answered) {
        return false;
      }
    }
    return m_floating.retireSizeRequestIfSettled(0);
  }

  bool View::roleInitialized() const { return m_toplevel == nullptr || m_toplevel->base->initialized; }

  bool View::openingConfigurePending() const { return m_toplevel != nullptr && m_toplevel->base->initial_commit; }

  void View::reassertRoleState() {
    if (m_toplevel != nullptr) {
      wlr_xdg_surface_schedule_configure(m_toplevel->base);
      return;
    }
    wlr_xwayland_surface_set_fullscreen(m_xsurface, m_xState.fullscreen);
    wlr_xwayland_surface_set_maximized(m_xsurface, m_xState.maximized, m_xState.maximized);
  }

  void View::setTiledState(uint32_t edges) {
    if (m_toplevel != nullptr) {
      wlr_xdg_toplevel_set_tiled(m_toplevel, edges);
    }
  }

  bool View::shellParentRequested() const {
    return m_toplevel != nullptr ? m_toplevel->parent != nullptr : m_xsurface->parent != nullptr;
  }

  bool View::looksTiled() const {
    if (openingParented()) {
      return false;
    }
    if (m_xsurface != nullptr) {
      if (m_xsurface->modal) {
        return false;
      }
      for (const wlr_xwayland_net_wm_window_type type :
           {WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
            WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY}) {
        if (wlr_xwayland_surface_has_window_type(m_xsurface, type)) {
          return false;
        }
      }
    }
    // A fullscreen window fills the output whatever its size hints say. Wine pins a game window that does not match
    // the X monitor to a fixed size, which must not make it a floating dialog.
    if (requestedFullscreen()) {
      return true;
    }
    if (m_toplevel != nullptr) {
      const auto& state = m_toplevel->current;
      const bool fixedWidth = state.max_width > 0 && state.min_width == state.max_width;
      const bool fixedHeight = state.max_height > 0 && state.min_height == state.max_height;
      return !fixedWidth && !fixedHeight;
    }
    const xcb_size_hints_t* hints = m_xsurface->size_hints;
    if (hints == nullptr
        || (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) == 0
        || (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) == 0) {
      return true;
    }
    const bool fixedWidth = hints->max_width > 0 && hints->min_width == hints->max_width;
    const bool fixedHeight = hints->max_height > 0 && hints->min_height == hints->max_height;
    return !fixedWidth && !fixedHeight;
  }

  void View::sendXwaylandConfigure(int width, int height) {
    m_xScheduled = {.width = width, .height = height};
    syncXwaylandConfigure();
  }

  void View::syncXwaylandConfigure() {
    if (m_xsurface == nullptr || m_xsurface->surface == nullptr || m_contentTree == nullptr) {
      return;
    }
    const XwaylandRegion region = xwaylandRegion();
    applyXwaylandScale(region.scale);
    // The frame's position is its place in layout coordinates: a workspace slide moves the workspace tree around it
    // and does not change it. Its own animated motion is not published frame by frame either, because X clients, games
    // in particular, react to every move; the last tick of the animation publishes where the window came to rest.
    int lx = region.toX(m_sceneTree->node.x);
    int ly = region.toY(m_sceneTree->node.y);
    // X11 geometry is 16-bit: coordinates are signed, sizes unsigned and nonzero.
    constexpr int kMinCoordinate = std::numeric_limits<int16_t>::min();
    constexpr int kMaxCoordinate = std::numeric_limits<int16_t>::max();
    constexpr int kMaxSize = std::numeric_limits<uint16_t>::max();
    lx = std::clamp(lx, kMinCoordinate, kMaxCoordinate);
    ly = std::clamp(ly, kMinCoordinate, kMaxCoordinate);
    const bool moving = m_posX.animating() || m_posY.animating() || m_layoutMotion || openingInsetActive();
    if (moving && m_xConfigured.x != INT_MIN) {
      lx = m_xConfigured.x;
      ly = m_xConfigured.y;
    }
    // An axis the compositor leaves to the client keeps the size the X window has. A fullscreen window that switched
    // to an emulated mode keeps that mode's size.
    ClientSize size{.width = m_xsurface->width, .height = m_xsurface->height};
    if (scheduledFullscreen() && m_xFullscreenSize) {
      size = *m_xFullscreenSize;
    } else {
      if (m_xScheduled.width > 0) {
        size.width = region.toXWidth(m_xScheduled.width);
      }
      if (m_xScheduled.height > 0) {
        size.height = region.toXHeight(m_xScheduled.height);
      }
    }
    const int width = std::clamp(size.width, 1, kMaxSize);
    const int height = std::clamp(size.height, 1, kMaxSize);
    if (lx == m_xConfigured.x
        && ly == m_xConfigured.y
        && width == m_xConfigured.width
        && height == m_xConfigured.height) {
      return;
    }
    m_xConfigured = {.x = lx, .y = ly, .width = width, .height = height};
    wlr_xwayland_surface_configure(
        m_xsurface, static_cast<int16_t>(lx), static_cast<int16_t>(ly), static_cast<uint16_t>(width),
        static_cast<uint16_t>(height)
    );
  }

  XwaylandRegion View::xwaylandRegion() const {
    const Output* output = currentOutput();
    return m_server->xwayland()->outputs().region(
        output != nullptr ? output->wlr() : nullptr, m_sceneTree->node.x, m_sceneTree->node.y
    );
  }

  void View::applyXwaylandScale(double scale) {
    if (scale == m_xScale) {
      return;
    }
    m_xScale = scale;
    wlr_scene_subsurface_tree_set_scale(&m_contentTree->node, scale);
    if (m_captureScene != nullptr) {
      wlr_scene_subsurface_tree_set_scale(&m_captureScene->tree.node, scale);
    }
  }

  int View::xToLayoutLength(int length) const {
    return m_xScale == 1.0 ? length : static_cast<int>(std::lround(length / m_xScale));
  }

  void View::resyncXwaylandGeometry() {
    if (!m_mapped || m_xsurface == nullptr) {
      return;
    }
    m_xConfigured.x = INT_MIN;
    syncXwaylandConfigure();
  }

  void View::onXwaylandRequestConfigure(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_xRequestConfigure);
    self->handleXwaylandRequestConfigure(static_cast<const wlr_xwayland_surface_configure_event*>(data));
  }

  void View::onXwaylandRequestActivate(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_xRequestActivate);
    self->handleXwaylandRequestActivate();
  }

  void View::onXwaylandSetHints(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_xSetHints);
    self->handleXwaylandSetHints();
  }

  void View::handleXwaylandRequestConfigure(const wlr_xwayland_surface_configure_event* event) {
    if (!m_mapped) {
      // Before map the client owns its geometry; the opening configure at map replaces it.
      grantConfigureRequest(m_xsurface, event);
      return;
    }
    // A request may name one axis only; the other keeps the configured size.
    const bool widthRequested = (event->mask & XCB_CONFIG_WINDOW_WIDTH) != 0;
    const bool heightRequested = (event->mask & XCB_CONFIG_WINDOW_HEIGHT) != 0;
    const int width = widthRequested ? event->width : m_xConfigured.width;
    const int height = heightRequested ? event->height : m_xConfigured.height;
    if (scheduledFullscreen() && (widthRequested || heightRequested)) {
      // A game that switches to an emulated RandR mode resizes its fullscreen window to that mode at the output origin.
      // Xwayland scales such a window up to the output through a viewport, but only while it is exactly that size.
      const XwaylandRegion region = xwaylandRegion();
      const int fullWidth = region.toXWidth(m_xScheduled.width);
      const int fullHeight = region.toXHeight(m_xScheduled.height);
      const int x = (event->mask & XCB_CONFIG_WINDOW_X) != 0 ? event->x : m_xConfigured.x;
      const int y = (event->mask & XCB_CONFIG_WINDOW_Y) != 0 ? event->y : m_xConfigured.y;
      const bool emulated = x == m_xConfigured.x
          && y == m_xConfigured.y
          && width > 0
          && height > 0
          && width <= fullWidth
          && height <= fullHeight
          && (width != fullWidth || height != fullHeight);
      m_xFullscreenSize = emulated ? std::optional<ClientSize>({.width = width, .height = height}) : std::nullopt;
    }
    if (!m_tiled
        && !scheduledFullscreen()
        && !scheduledMaximized()
        && !m_maximizedToEdges
        && !sizeGrabActive()
        && (widthRequested || heightRequested)
        && width > 0
        && height > 0
        && (width != m_xConfigured.width || height != m_xConfigured.height)) {
      // A floating window's size is the client's to choose; its position stays compositor-owned.
      requestFloatingSize(xToLayoutLength(width), xToLayoutLength(height));
    }
    // ICCCM expects a ConfigureNotify in reply even when the request is denied.
    m_xConfigured.x = INT_MIN;
    syncXwaylandConfigure();
  }

  void View::handleXwaylandRequestActivate() {
    // Client-initiated activation follows the untrusted xdg-activation policy.
    if (m_activated) {
      setUrgent(false);
      return;
    }
    if (!m_mapped) {
      deferActivation(false);
      return;
    }
    if (resolvedRules().focusOnActivate.value_or(config().general.focusOnActivate)) {
      m_server->focusView(this, FocusReason::XdgActivation);
    } else {
      setUrgent(true);
    }
  }

  void View::handleXwaylandSetHints() {
    if (m_activated) {
      return;
    }
    // Removing WM_HINTS clears urgency as well.
    setUrgent(m_xsurface->hints != nullptr && xcb_icccm_wm_hints_get_urgency(m_xsurface->hints) != 0);
  }

} // namespace umbriel
