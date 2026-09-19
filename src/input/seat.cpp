#include "input/seat.h"

#include "config/config.h"
#include "input/cursor.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  Seat::Seat(Server& server) : m_server(&server) {
    m_seat = wlr_seat_create(m_server->display(), "seat0");
    m_primarySelectionEnabled = config().input.middleClickPaste;

    m_cursorShapeManager = wlr_cursor_shape_manager_v1_create(m_server->display(), 2);
    m_requestSetShape.notify = onRequestSetShape;
    wl_signal_add(&m_cursorShapeManager->events.request_set_shape, &m_requestSetShape);

    m_requestCursor.notify = onRequestCursor;
    wl_signal_add(&m_seat->events.request_set_cursor, &m_requestCursor);

    m_pointerFocusChange.notify = onPointerFocusChange;
    wl_signal_add(&m_seat->pointer_state.events.focus_change, &m_pointerFocusChange);

    m_requestSetSelection.notify = onRequestSetSelection;
    wl_signal_add(&m_seat->events.request_set_selection, &m_requestSetSelection);

    m_requestSetPrimarySelection.notify = onRequestSetPrimarySelection;
    wl_signal_add(&m_seat->events.request_set_primary_selection, &m_requestSetPrimarySelection);

    m_requestStartDrag.notify = onRequestStartDrag;
    wl_signal_add(&m_seat->events.request_start_drag, &m_requestStartDrag);

    m_startDrag.notify = onStartDrag;
    wl_signal_add(&m_seat->events.start_drag, &m_startDrag);
  }

  Seat::~Seat() {
    wl_list_remove(&m_requestCursor.link);
    wl_list_remove(&m_requestSetShape.link);
    wl_list_remove(&m_pointerFocusChange.link);
    wl_list_remove(&m_requestSetSelection.link);
    wl_list_remove(&m_requestSetPrimarySelection.link);
    wl_list_remove(&m_requestStartDrag.link);
    wl_list_remove(&m_startDrag.link);
    if (m_dragDestroy.link.next != nullptr) {
      wl_list_remove(&m_dragDestroy.link);
    }
  }

  void Seat::applyConfig() {
    const bool enabled = config().input.middleClickPaste;
    if (enabled == m_primarySelectionEnabled) {
      return;
    }

    m_primarySelectionEnabled = enabled;
    if (!enabled && m_seat->primary_selection_source != nullptr) {
      wlr_seat_set_primary_selection(m_seat, nullptr, wl_display_next_serial(m_server->display()));
    }
  }

  void Seat::updateCapabilities(bool hasKeyboard, bool hasTouch) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (hasKeyboard) {
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    if (hasTouch) {
      caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wlr_seat_set_capabilities(m_seat, caps);
  }

  void Seat::onRequestCursor(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestCursor);
    self->handleRequestCursor(data);
  }

  void Seat::onRequestSetShape(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestSetShape);
    self->handleRequestSetShape(data);
  }

  void Seat::onPointerFocusChange(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_pointerFocusChange);
    self->handlePointerFocusChange(data);
  }

  void Seat::onRequestSetSelection(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestSetSelection);
    self->handleRequestSetSelection(data);
  }

  void Seat::onRequestSetPrimarySelection(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestSetPrimarySelection);
    self->handleRequestSetPrimarySelection(data);
  }

  void Seat::onRequestStartDrag(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestStartDrag);
    self->handleRequestStartDrag(data);
  }

  void Seat::onStartDrag(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_startDrag);
    self->handleStartDrag(data);
  }

  void Seat::onDragDestroy(wl_listener* listener, void* /*data*/) {
    Seat* self = wl_container_of(listener, self, m_dragDestroy);
    self->handleDragDestroy();
  }

  void Seat::handleRequestCursor(void* data) {
    auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    // Recorded even while the compositor owns the cursor, so ending that
    // override replays the client's cursor instead of making it resend one.
    if (m_seat->pointer_state.focused_client == event->seat_client) {
      m_server->cursor()->setCursorSurface(event->surface, event->hotspot_x, event->hotspot_y);
    }
  }

  void Seat::handleRequestSetShape(void* data) {
    auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    bool pointer = false;
    if (event->device_type == WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
      if (m_seat->pointer_state.focused_client != event->seat_client) {
        return;
      }
      pointer = true;
    } else if (event->device_type == WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_TABLET_TOOL) {
      // The tool's focused surface is the acceptance check; there is no
      // pointer-focus client for a tablet cursor.
      if (event->tablet_tool == nullptr
          || event->tablet_tool->focused_surface == nullptr
          || wl_resource_get_client(event->tablet_tool->focused_surface->resource) != event->seat_client->client) {
        return;
      }
    } else {
      return;
    }

    const char* name = wlr_cursor_shape_v1_name(event->shape);
    if (name == nullptr) {
      return;
    }
    if (pointer) {
      m_server->cursor()->setCursorShape(name);
    } else if (!m_server->cursor()->compositorOwnsCursor()) {
      m_server->cursor()->setXcursor(name);
    }
  }

  void Seat::handlePointerFocusChange(void* data) {
    auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
    if (event->old_surface != nullptr) {
      wlr_seat_client* oldClient =
          wlr_seat_client_for_wl_client(m_seat, wl_resource_get_client(event->old_surface->resource));
      if (oldClient != m_seat->pointer_state.focused_client) {
        sendPointerModifiers(oldClient, true);
      }
    }
    notifyPointerModifiers();
    m_server->cursor()->notePointerFocusChange(event->new_surface);
  }

  void Seat::notifyPointerModifiers(bool clear) {
    // Pointer focus includes the implicit button grab, not just the hit-tested
    // surface. Never grant keyboard focus or deliver keys to a hovered panel.
    if (!clear && m_server->sessionLocked()) {
      wlr_surface* surface = m_seat->pointer_state.focused_surface;
      if (surface == nullptr || wlr_session_lock_surface_v1_try_from_wlr_surface(surface) == nullptr) {
        return;
      }
    }
    sendPointerModifiers(m_seat->pointer_state.focused_client, clear);
  }

  void Seat::sendPointerModifiers(wlr_seat_client* client, bool clear) {
    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_seat);
    if (client == nullptr
        || client == m_seat->keyboard_state.focused_client
        || (!clear && (keyboard == nullptr || keyboard->keymap == nullptr))) {
      return;
    }
    // wl_keyboard.modifiers explicitly permits delivery without keyboard focus
    // to associate modifiers with pointer focus. Use the active keymap's XKB
    // masks (not WLR_MODIFIER_*), retaining locks and layout when leaving.
    // A reset must work after keyboard removal, and a lock-time leave must
    // not expose lock/layout state to the old normal client.
    wlr_keyboard_modifiers modifiers{};
    if (keyboard != nullptr && keyboard->keymap != nullptr && !(clear && m_server->sessionLocked())) {
      modifiers = keyboard->modifiers;
    }
    const uint32_t serial = wlr_seat_client_next_serial(client);
    wl_resource* resource = nullptr;
    wl_resource_for_each(resource, &client->keyboards) {
      // A keyboard resource can outlive the seat's keyboard capability.
      if (wl_resource_get_user_data(resource) != nullptr) {
        wl_keyboard_send_modifiers(
            resource, serial, clear ? 0 : modifiers.depressed, clear ? 0 : modifiers.latched, modifiers.locked,
            modifiers.group
        );
      }
    }
  }

  void Seat::handleRequestSetSelection(void* data) {
    auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(m_seat, event->source, event->serial);
  }

  void Seat::handleRequestSetPrimarySelection(void* data) {
    auto* event = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
    if (!m_primarySelectionEnabled) {
      return;
    }
    wlr_seat_set_primary_selection(m_seat, event->source, event->serial);
  }

  void Seat::handleRequestStartDrag(void* data) {
    auto* event = static_cast<wlr_seat_request_start_drag_event*>(data);
    if (wlr_seat_validate_pointer_grab_serial(m_seat, event->origin, event->serial)) {
      wlr_seat_start_pointer_drag(m_seat, event->drag, event->serial);
      return;
    }
    wlr_data_source_destroy(event->drag->source);
  }

  void Seat::handleStartDrag(void* data) {
    auto* drag = static_cast<wlr_drag*>(data);
    if (m_dragDestroy.link.next != nullptr) {
      wl_list_remove(&m_dragDestroy.link);
    }
    m_dragDestroy.notify = onDragDestroy;
    wl_signal_add(&drag->events.destroy, &m_dragDestroy);
    if (drag->icon == nullptr) {
      return;
    }
    // Scene node is destroyed with the icon; no extra listener needed.
    wlr_scene_drag_icon_create(m_server->dragIconTree(), drag->icon);
    wlr_scene_node_set_position(
        &m_server->dragIconTree()->node, static_cast<int>(m_server->cursor()->wlr()->x),
        static_cast<int>(m_server->cursor()->wlr()->y)
    );
  }

  void Seat::handleDragDestroy() {
    wl_list_remove(&m_dragDestroy.link);
    m_dragDestroy.link.next = nullptr;
    m_dragDestroy.link.prev = nullptr;
    m_server->restoreActivatedViewKeyboardFocus();
  }

} // namespace umbriel
