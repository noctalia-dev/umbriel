#include "input/cursor.h"
#include "input/seat.h"
#include "scene/node.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

namespace umbriel {

  void Cursor::handleNewConstraint(wlr_pointer_constraint_v1* constraint) {
    wlr_surface* focused = m_server->seat()->wlr()->pointer_state.focused_surface;
    if (focused != nullptr && focused == constraint->surface) {
      setActiveConstraint(constraint);
    }
  }

  void Cursor::clearConstraint() { setActiveConstraint(nullptr); }

  void Cursor::onConstraintDestroy(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_constraintDestroy);
    self->handleConstraintDestroy();
  }

  void Cursor::handleConstraintDestroy() {
    // The client can end a lock by destroying the constraint. Apply its last
    // committed position hint before wlroots releases the constraint state.
    warpToConstraintHint(m_activeConstraint);
    wl_list_remove(&m_constraintDestroy.link);
    m_constraintDestroy.link.next = nullptr;
    m_activeConstraint = nullptr;
    // Cursor visibility belongs to the focused client and remains valid across
    // pointer-constraint transitions. Focus loss restores the theme cursor.
  }

  void Cursor::setActiveConstraint(wlr_pointer_constraint_v1* constraint) {
    if (m_activeConstraint == constraint) {
      return;
    }

    if (m_activeConstraint != nullptr) {
      wlr_pointer_constraint_v1* previous = m_activeConstraint;
      m_activeConstraint = nullptr;
      if (m_constraintDestroy.link.next != nullptr) {
        wl_list_remove(&m_constraintDestroy.link);
        m_constraintDestroy.link.next = nullptr;
      }
      warpToConstraintHint(previous);
      wlr_pointer_constraint_v1_send_deactivated(previous);
    }

    m_activeConstraint = constraint;
    if (constraint == nullptr) {
      return;
    }

    m_constraintDestroy.notify = onConstraintDestroy;
    wl_signal_add(&constraint->events.destroy, &m_constraintDestroy);
    wlr_pointer_constraint_v1_send_activated(constraint);
  }

  bool Cursor::constraintSurfaceActive() const {
    if (m_activeConstraint == nullptr || m_activeConstraint->surface == nullptr) {
      return false;
    }
    wlr_surface* root = wlr_surface_get_root_surface(m_activeConstraint->surface);
    wlr_xdg_surface* xdg = root != nullptr ? wlr_xdg_surface_try_from_wlr_surface(root) : nullptr;
    if (xdg == nullptr || xdg->data == nullptr) {
      return false;
    }
    auto* tree = static_cast<wlr_scene_tree*>(xdg->data);
    if (tree == nullptr) {
      return false;
    }
    SceneNode* node = sceneNodeFrom(tree->node.data);
    if (node == nullptr || node->kind != SceneNodeKind::View) {
      return false;
    }
    auto* view = static_cast<View*>(node);
    return view->mapped() && view->onActiveWorkspace();
  }

  void Cursor::updateConstraintForSurface(wlr_surface* surface) {
    if (m_server->sessionLocked() || !isPassthrough()) {
      setActiveConstraint(nullptr);
      return;
    }

    wlr_pointer_constraint_v1* constraint = nullptr;
    if (surface != nullptr) {
      constraint = wlr_pointer_constraints_v1_constraint_for_surface(
          m_server->pointerConstraints(), surface, m_server->seat()->wlr()
      );
    }
    setActiveConstraint(constraint);
  }

  void Cursor::warpToConstraintHint(wlr_pointer_constraint_v1* constraint) {
    if (constraint == nullptr || constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
    }
    if (!constraint->current.cursor_hint.enabled) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    if (seat->pointer_state.focused_surface != constraint->surface) {
      return;
    }

    double sx = seat->pointer_state.sx;
    double sy = seat->pointer_state.sy;
    double lx = m_cursor->x + (constraint->current.cursor_hint.x - sx);
    double ly = m_cursor->y + (constraint->current.cursor_hint.y - sy);
    wlr_cursor_warp(m_cursor, nullptr, lx, ly);
    // Keep wlroots' surface-local pointer state in sync with the layout
    // cursor, avoiding a synthetic jump on the next pointer rebase.
    wlr_seat_pointer_warp(seat, constraint->current.cursor_hint.x, constraint->current.cursor_hint.y);
  }

  bool Cursor::confineDelta(double* dx, double* dy) const {
    if (m_activeConstraint == nullptr) {
      return true;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    if (seat->pointer_state.focused_surface != m_activeConstraint->surface) {
      return true;
    }

    double sx = seat->pointer_state.sx;
    double sy = seat->pointer_state.sy;

    pixman_region32_t* region = &m_activeConstraint->region;
    pixman_box32_t* extents = pixman_region32_extents(region);
    pixman_region32_t fullSurface{};
    if (extents->x2 - extents->x1 == 0 || extents->y2 - extents->y1 == 0) {
      // Empty region means the whole surface.
      wlr_surface* surface = m_activeConstraint->surface;
      pixman_region32_init_rect(&fullSurface, 0, 0, surface->current.width, surface->current.height);
      region = &fullSurface;
    }

    double confinedX = 0;
    double confinedY = 0;
    const bool ok = wlr_region_confine(region, sx, sy, sx + *dx, sy + *dy, &confinedX, &confinedY);
    if (region == &fullSurface) {
      pixman_region32_fini(&fullSurface);
    }
    if (!ok) {
      return false;
    }

    *dx = confinedX - sx;
    *dy = confinedY - sy;
    return true;
  }

} // namespace umbriel
