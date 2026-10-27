#include "xwayland/unmanaged.h"

#include "input/seat.h"
#include "scene/surface_shape.h"
#include "server/server.h"
#include "wlr.h"
#include "xwayland/xwayland.h"

namespace umbriel {

  namespace {

    // An override-redirect window that takes the keyboard says so through its window type. A managed menu or dropdown
    // is focused by the window manager, as on X11; a tooltip or drag icon never is.
    bool takesKeyboard(const wlr_xwayland_surface* xsurface) {
      if (wlr_xwayland_surface_icccm_input_model(xsurface) == WLR_ICCCM_INPUT_MODEL_NONE) {
        return false;
      }
      if (xsurface->override_redirect) {
        return wlr_xwayland_surface_override_redirect_wants_focus(xsurface);
      }
      return !wlr_xwayland_surface_has_window_type(xsurface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP)
          && !wlr_xwayland_surface_has_window_type(xsurface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND);
    }

  } // namespace

  XwaylandUnmanaged::XwaylandUnmanaged(Server& server, wlr_xwayland_surface* xsurface)
      : m_server(server), m_xsurface(xsurface) {
    m_tree = wlr_scene_tree_create(server.xwaylandUnmanagedTree());
    wlr_scene_subsurface_tree_create(m_tree, xsurface->surface);
    syncPosition();
    wlr_scene_node_set_enabled(&m_tree->node, xsurface->surface->mapped);

    m_map.notify = onMap;
    wl_signal_add(&xsurface->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&xsurface->surface->events.unmap, &m_unmap);
    m_setGeometry.notify = onSetGeometry;
    wl_signal_add(&xsurface->events.set_geometry, &m_setGeometry);
    m_requestConfigure.notify = onRequestConfigure;
    wl_signal_add(&xsurface->events.request_configure, &m_requestConfigure);
    m_commit.notify = onCommit;
    wl_signal_add(&xsurface->surface->events.commit, &m_commit);
    m_shapeChanged.notify = onShapeChanged;
    wl_signal_add(server.xwayland()->shapeChanged(), &m_shapeChanged);
    if (xsurface->surface->mapped) {
      handleMap();
    }
  }

  XwaylandUnmanaged::~XwaylandUnmanaged() {
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_setGeometry.link);
    wl_list_remove(&m_requestConfigure.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_shapeChanged.link);
    releaseKeyboard();
    wlr_scene_node_destroy(&m_tree->node);
  }

  void XwaylandUnmanaged::onMap(wl_listener* listener, void* /*data*/) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_map); // NOLINT(modernize-use-auto)
    self->handleMap();
  }

  void XwaylandUnmanaged::onUnmap(wl_listener* listener, void* /*data*/) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_unmap); // NOLINT(modernize-use-auto)
    self->handleUnmap();
  }

  void XwaylandUnmanaged::onSetGeometry(wl_listener* listener, void* /*data*/) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_setGeometry); // NOLINT(modernize-use-auto)
    self->handleSetGeometry();
  }

  void XwaylandUnmanaged::onRequestConfigure(wl_listener* listener, void* data) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_requestConfigure); // NOLINT(modernize-use-auto)
    self->handleRequestConfigure(static_cast<const wlr_xwayland_surface_configure_event*>(data));
  }

  void XwaylandUnmanaged::onCommit(wl_listener* listener, void* /*data*/) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_commit); // NOLINT(modernize-use-auto)
    self->updateShape();
  }

  void XwaylandUnmanaged::onShapeChanged(wl_listener* listener, void* data) {
    XwaylandUnmanaged* self = wl_container_of(listener, self, m_shapeChanged); // NOLINT(modernize-use-auto)
    if (data == self->m_xsurface->surface) {
      self->updateShape();
    }
  }

  void XwaylandUnmanaged::updateShape() {
    wlr_surface* surface = m_xsurface->surface;
    applySurfaceTreeShape(&m_tree->node, surface, m_server.xwayland()->surfaceShape(surface));
  }

  void XwaylandUnmanaged::handleMap() {
    syncPosition();
    wlr_scene_node_set_enabled(&m_tree->node, true);
    wlr_scene_node_raise_to_top(&m_tree->node);
    if (!takesKeyboard(m_xsurface) || m_server.sessionLocked() || m_server.exclusiveKeyboardLayer() != nullptr) {
      return;
    }
    // A menu that takes the keyboard: the window it belongs to keeps its activated chrome, as with an xdg popup.
    wlr_seat* seat = m_server.seat()->wlr();
    if (seat->drag == nullptr && wlr_seat_keyboard_has_grab(seat)) {
      wlr_seat_keyboard_end_grab(seat);
    }
    m_server.notifyKeyboardEnter(m_xsurface->surface);
    // An override-redirect window sets X input focus itself; a managed one waits for the window manager. The
    // activated window reclaims it when it takes the seat back.
    if (!m_xsurface->override_redirect) {
      wlr_xwayland_surface_activate(m_xsurface, true);
    }
  }

  void XwaylandUnmanaged::handleUnmap() {
    wlr_scene_node_set_enabled(&m_tree->node, false);
    releaseKeyboard();
  }

  void XwaylandUnmanaged::releaseKeyboard() {
    wlr_seat* seat = m_server.seat()->wlr();
    if (m_xsurface->surface == nullptr || seat->keyboard_state.focused_surface != m_xsurface->surface) {
      return;
    }
    m_server.restoreActivatedViewKeyboardFocus();
    if (seat->keyboard_state.focused_surface == m_xsurface->surface) {
      m_server.clearKeyboardFocus();
    }
  }

  void XwaylandUnmanaged::handleSetGeometry() { syncPosition(); }

  void XwaylandUnmanaged::syncPosition() {
    const XwaylandRegion region = m_server.xwayland()->outputs().regionAtX(m_xsurface->x, m_xsurface->y);
    wlr_scene_node_set_position(&m_tree->node, region.toLayoutX(m_xsurface->x), region.toLayoutY(m_xsurface->y));
    if (region.scale != m_scale) {
      m_scale = region.scale;
      wlr_scene_subsurface_tree_set_scale(&m_tree->node, m_scale);
    }
    updateShape();
  }

  void XwaylandUnmanaged::handleRequestConfigure(const wlr_xwayland_surface_configure_event* event) {
    // The resulting set_geometry moves the node.
    grantConfigureRequest(m_xsurface, event);
  }

} // namespace umbriel
