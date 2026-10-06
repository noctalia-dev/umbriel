#include "config/store.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "xwayland/xwayland.h"

namespace umbriel {

  void Cursor::handleNewConstraint(wlr_pointer_constraint_v1* constraint) {
    wlr_surface* focused = m_server->seat()->wlr()->pointer_state.focused_surface;
    if (focused != nullptr && focused == constraint->surface) {
      updateConstraintForSurface(focused);
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
    View* view = View::fromSurface(wlr_surface_get_root_surface(m_activeConstraint->surface));
    return view != nullptr
        && view->mapped()
        && view->onActiveWorkspace()
        && (!ruleConstraintApplies(*view) || ruleConstraintEligible(*view));
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
      if (View* view = View::fromSurface(wlr_surface_get_root_surface(surface));
          view != nullptr && ruleConstraintApplies(*view) && !ruleConstraintEligible(*view)) {
        constraint = nullptr;
      }
    }
    setActiveConstraint(constraint);
  }

  void Cursor::releaseRuleConfinement(View& view) {
    m_hoverFocusInvalidated = false;
    const char* id = view.extForeignIdentifier();
    m_ruleConfinementView = id != nullptr ? id : "";
    m_ruleConfinementGeneration = configStore().generation();
    if (m_activeConstraint != nullptr
        && View::fromSurface(wlr_surface_get_root_surface(m_activeConstraint->surface)) == &view) {
      clearConstraint();
    }
  }

  bool Cursor::ruleConstraintApplies(View& view) const {
    const char* id = view.extForeignIdentifier();
    return view.confinePointer()
        || (id != nullptr && m_ruleConfinementView == id && m_ruleConfinementGeneration == configStore().generation());
  }

  bool Cursor::ruleConstraintEligible(View& view) const {
    return view.mapped()
        && view.onActiveWorkspace()
        && !m_server->sessionLocked()
        && isPassthrough()
        && !m_server->overview()->active()
        && m_server->seat()->wlr()->drag == nullptr
        && View::fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface) == &view;
  }

  void Cursor::confineRuleDelta(double* dx, double* dy) {
    View* view = View::fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface);
    if (view == nullptr || !view->confinePointer() || !ruleConstraintEligible(*view)) {
      return;
    }
    Output* output = view->currentOutput();
    if (output == nullptr) {
      return;
    }
    // Scene coordinates include workspace movement; presented size excludes decorations and shadows.
    // Match the output's scene-root clipping at shared monitor edges.
    wlr_box bounds = view->presentedBox();
    if (!wlr_scene_node_coords(&view->sceneTree()->node, &bounds.x, &bounds.y)) {
      return;
    }
    const wlr_box outputBox = output->layoutBox();
    if (!wlr_box_intersection(&bounds, &bounds, &outputBox)
        || !wlr_box_contains_point(&bounds, m_cursor->x, m_cursor->y)) {
      return;
    }
    // An obscured window must not capture input belonging to a layer surface or another window.
    wlr_surface* surface = nullptr;
    double sx = 0;
    double sy = 0;
    if (m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy) != view) {
      return;
    }
    pixman_region32_t region{};
    pixman_region32_init_rect(&region, bounds.x, bounds.y, bounds.width, bounds.height);
    double x = 0;
    double y = 0;
    if (wlr_region_confine(&region, m_cursor->x, m_cursor->y, m_cursor->x + *dx, m_cursor->y + *dy, &x, &y)) {
      *dx = x - m_cursor->x;
      *dy = y - m_cursor->y;
    }
    pixman_region32_fini(&region);
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

    // The hint and the seat position are surface-local; the cursor moves in layout units.
    const double scale = surfaceScale(constraint->surface);
    double sx = seat->pointer_state.sx;
    double sy = seat->pointer_state.sy;
    double lx = m_cursor->x + ((constraint->current.cursor_hint.x - sx) / scale);
    double ly = m_cursor->y + ((constraint->current.cursor_hint.y - sy) / scale);
    wlr_cursor_warp(m_cursor, nullptr, lx, ly);
    forwardEffectPointer();
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

    // The region is surface-local; the deltas are in layout units.
    const double scale = surfaceScale(m_activeConstraint->surface);
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
    const bool ok = wlr_region_confine(region, sx, sy, sx + (*dx * scale), sy + (*dy * scale), &confinedX, &confinedY);
    if (region == &fullSurface) {
      pixman_region32_fini(&fullSurface);
    }
    if (!ok) {
      return false;
    }

    *dx = (confinedX - sx) / scale;
    *dy = (confinedY - sy) / scale;
    return true;
  }

  double Cursor::surfaceScale(wlr_surface* surface) const {
    const Xwayland* xwayland = m_server->xwayland();
    return xwayland != nullptr ? xwayland->surfaceScale(surface) : 1.0;
  }

} // namespace umbriel
