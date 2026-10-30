#include "xwayland/window.h"

#include "core/log.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "xwayland/unmanaged.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <array>

namespace umbriel {

  namespace {

    constexpr Logger kLog("xwayland");

    // Client-placed context menus, notifications, tooltips, dropdowns, and drag icons can omit override-redirect.
    // EWMH MENU describes a persistent torn-off menu, so it stays managed unless it requests override-redirect.
    constexpr std::array kPopupWindowTypes{
        WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
        WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,       WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
        WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,           WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
    };

    bool isPopup(const wlr_xwayland_surface* xsurface) {
      return xsurface->override_redirect
          || isPassiveUtilityPopup(xsurface)
          || std::ranges::any_of(kPopupWindowTypes, [xsurface](auto type) {
               return wlr_xwayland_surface_has_window_type(xsurface, type);
             });
    }

  } // namespace

  bool isPassiveUtilityPopup(const wlr_xwayland_surface* xsurface) {
    return !xsurface->override_redirect
        && xsurface->parent != nullptr
        && wlr_xwayland_surface_has_window_type(xsurface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY)
        && xsurface->hints != nullptr
        && (xsurface->hints->flags & XCB_ICCCM_WM_HINT_INPUT) != 0
        && xsurface->hints->input == 0;
  }

  XwaylandWindow::XwaylandWindow(Server& server, Xwayland& owner, wlr_xwayland_surface* xsurface)
      : m_server(server), m_owner(owner), m_xsurface(xsurface),
        m_clientGeometry{xsurface->x, xsurface->y, xsurface->width, xsurface->height} {
    // No wrapper until associate: the wl_surface does not exist before it.
    m_associate.notify = onAssociate;
    wl_signal_add(&xsurface->events.associate, &m_associate);
    m_dissociate.notify = onDissociate;
    wl_signal_add(&xsurface->events.dissociate, &m_dissociate);
    m_destroy.notify = onDestroy;
    wl_signal_add(&xsurface->events.destroy, &m_destroy);
    m_setOverrideRedirect.notify = onSetOverrideRedirect;
    wl_signal_add(&xsurface->events.set_override_redirect, &m_setOverrideRedirect);
    m_setWindowType.notify = onSetWindowType;
    wl_signal_add(&xsurface->events.set_window_type, &m_setWindowType);
    m_requestConfigure.notify = onRequestConfigure;
    wl_signal_add(&xsurface->events.request_configure, &m_requestConfigure);
  }

  XwaylandWindow::~XwaylandWindow() {
    removeListeners();
    m_unmanaged.reset();
  }

  void XwaylandWindow::removeListeners() {
    for (wl_listener* listener :
         {&m_associate, &m_dissociate, &m_destroy, &m_setOverrideRedirect, &m_setWindowType, &m_requestConfigure}) {
      if (listener->link.next != nullptr) {
        wl_list_remove(&listener->link);
        listener->link = {};
      }
    }
  }

  void XwaylandWindow::onAssociate(wl_listener* listener, void* /*data*/) {
    XwaylandWindow* self = wl_container_of(listener, self, m_associate); // NOLINT(modernize-use-auto)
    self->handleAssociate();
  }

  void XwaylandWindow::onDissociate(wl_listener* listener, void* /*data*/) {
    XwaylandWindow* self = wl_container_of(listener, self, m_dissociate); // NOLINT(modernize-use-auto)
    self->handleDissociate();
  }

  void XwaylandWindow::onDestroy(wl_listener* listener, void* /*data*/) {
    XwaylandWindow* self = wl_container_of(listener, self, m_destroy); // NOLINT(modernize-use-auto)
    self->handleDestroy();
  }

  void XwaylandWindow::onSetOverrideRedirect(wl_listener* listener, void* /*data*/) {
    XwaylandWindow* self = wl_container_of(listener, self, m_setOverrideRedirect); // NOLINT(modernize-use-auto)
    self->updateClassification();
  }

  void XwaylandWindow::onSetWindowType(wl_listener* listener, void* /*data*/) {
    XwaylandWindow* self = wl_container_of(listener, self, m_setWindowType); // NOLINT(modernize-use-auto)
    self->updateClassification();
  }

  void XwaylandWindow::onRequestConfigure(wl_listener* listener, void* data) {
    XwaylandWindow* self = wl_container_of(listener, self, m_requestConfigure); // NOLINT(modernize-use-auto)
    const auto* event = static_cast<const wlr_xwayland_surface_configure_event*>(data);
    if (self->m_unmanaged == nullptr) {
      if ((event->mask & XCB_CONFIG_WINDOW_X) != 0) {
        self->m_clientGeometry.x = event->x;
      }
      if ((event->mask & XCB_CONFIG_WINDOW_Y) != 0) {
        self->m_clientGeometry.y = event->y;
      }
      if ((event->mask & XCB_CONFIG_WINDOW_WIDTH) != 0) {
        self->m_clientGeometry.width = event->width;
      }
      if ((event->mask & XCB_CONFIG_WINDOW_HEIGHT) != 0) {
        self->m_clientGeometry.height = event->height;
      }
    }
    // A view or unmanaged window answers for itself once it exists.
    if (self->m_view == nullptr && self->m_unmanaged == nullptr) {
      grantConfigureRequest(self->m_xsurface, event);
    }
  }

  void XwaylandWindow::handleAssociate() {
    createWrapper();
    wlr_surface* surface = m_xsurface->surface;
    // X11 association and Wayland commits travel over separate sockets. If the first buffer arrived already,
    // wlroots' newly installed commit listener missed it. Map it after all wrappers are ready so an idle client
    // cannot wait forever for a frame callback from an unmapped surface.
    if (!surface->mapped && wlr_surface_has_buffer(surface)) {
      kLog.debug("mapping buffered X11 window={} on association", m_xsurface->window_id);
      wlr_surface_map(surface);
    }
  }

  void XwaylandWindow::handleDissociate() {
    destroyWrapper();
    // The XWM focus record outlives a Wayland association. Retire it so a reused X window can take real X focus again.
    wlr_xwayland_surface_activate(m_xsurface, false);
    // Retiring that record can restore PointerRoot after a native client already received its keyboard enter.
    m_owner.scheduleFocusClear();
  }

  void XwaylandWindow::handleDestroy() {
    removeListeners();
    destroyWrapper();
    m_owner.forget(this); // deletes this
  }

  void XwaylandWindow::updateClassification() {
    if (m_view == nullptr && m_unmanaged == nullptr) {
      return; // associate reads the current metadata
    }
    const bool popup = isPopup(m_xsurface);
    if (popup == (m_unmanaged != nullptr)) {
      if (m_unmanaged != nullptr) {
        m_unmanaged->refreshFocusPolicy();
      }
      return;
    }
    if (m_view != nullptr && m_xsurface->override_redirect) {
      // A client can configure directly after enabling override-redirect. ConfigureNotify already applied those
      // coordinates before the flag event, so retain axes that differ from the retiring view's last configure.
      const auto& configured = m_view->m_xConfigured;
      if (m_xsurface->x != configured.x) {
        m_clientGeometry.x = m_xsurface->x;
      }
      if (m_xsurface->y != configured.y) {
        m_clientGeometry.y = m_xsurface->y;
      }
      if (m_xsurface->width != configured.width) {
        m_clientGeometry.width = m_xsurface->width;
      }
      if (m_xsurface->height != configured.height) {
        m_clientGeometry.height = m_xsurface->height;
      }
    }
    if (m_unmanaged != nullptr) {
      // Unmanaged geometry belongs to the client, including override-redirect configures that bypass request_configure.
      m_clientGeometry = {m_xsurface->x, m_xsurface->y, m_xsurface->width, m_xsurface->height};
    }
    if (m_view != nullptr) {
      m_captureBlockedBeforePopup = m_view->captureBlocked();
    }
    kLog.debug("reclassifying X11 window={} as {}", m_xsurface->window_id, popup ? "popup" : "managed");
    destroyWrapper();
    if (popup) {
      wlr_xwayland_surface_set_maximized(m_xsurface, false, false);
      wlr_xwayland_surface_set_fullscreen(m_xsurface, false);
      wlr_xwayland_surface_configure(
          m_xsurface, m_clientGeometry.x, m_clientGeometry.y, m_clientGeometry.width, m_clientGeometry.height
      );
    }
    createWrapper();
  }

  void XwaylandWindow::createWrapper() {
    if (isPopup(m_xsurface)) {
      m_unmanaged = std::make_unique<XwaylandUnmanaged>(m_server, m_xsurface, m_captureBlockedBeforePopup);
    } else {
      m_view = &m_server.adoptView(std::make_unique<View>(m_server, m_xsurface));
      if (m_xsurface->surface->mapped && !m_view->mapped()) {
        m_view->handleMap();
      }
    }
  }

  void XwaylandWindow::refreshCaptureProtection() {
    if (m_unmanaged != nullptr) {
      m_unmanaged->refreshCaptureProtection();
    }
  }

  void XwaylandWindow::syncGeometry() {
    if (m_view != nullptr) {
      m_view->resyncXwaylandGeometry();
    }
    if (m_unmanaged != nullptr) {
      m_unmanaged->syncPosition();
    }
  }

  double XwaylandWindow::scale() const {
    if (m_view != nullptr) {
      return m_view->xwaylandScale();
    }
    return m_unmanaged != nullptr ? m_unmanaged->scale() : 1.0;
  }

  void XwaylandWindow::handleXwmSync(uint64_t serial) {
    if (m_unmanaged != nullptr) {
      m_unmanaged->handleXwmSync(serial);
    }
  }

  void XwaylandWindow::destroyWrapper() {
    m_unmanaged.reset();
    if (View* view = m_view) {
      m_view = nullptr;
      if (view->mapped()) {
        view->handleUnmap(false);
      }
      view->roleDestroyed();
    }
  }

} // namespace umbriel
