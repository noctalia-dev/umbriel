#include "input/toplevel_drag.h"

#include "input/cursor.h"
#include "input/seat.h"
#include "server/server.h"
#include "view/view.h"
#include "view/view_internal.h"
#include "wlr.h"
#include "xdg-toplevel-drag-v1-server-protocol.h"

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>

namespace umbriel {
  namespace {
    // wlroots 0.20's wl_data_source resource stores wlr_client_data_source,
    // whose first member is wlr_data_source. Keep this prefix assumption in
    // sync with the pinned wlroots series; no private tail fields are read.
    wlr_data_source* dataSource(wl_resource* resource) {
      return static_cast<wlr_data_source*>(wl_resource_get_user_data(resource));
    }
    void destroyResource(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
  } // namespace

  struct ToplevelDragManager::Drag {
    ToplevelDragManager* manager = nullptr;
    wl_resource* resource = nullptr;
    wl_resource* source = nullptr;
    wlr_xdg_toplevel* toplevel = nullptr;
    int x = 0;
    int y = 0;
    bool started = false;
    bool ended = false;
    std::optional<bool> tileOnDrop = std::nullopt;
    wl_listener sourceDestroy{};
    wl_listener toplevelDestroy{};
    wl_listener drop{};
  };

  ToplevelDragManager::ToplevelDragManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &xdg_toplevel_drag_manager_v1_interface, 1, this, bind);
    if (m_global == nullptr) {
      throw std::runtime_error("failed to create toplevel drag manager");
    }
  }

  ToplevelDragManager::~ToplevelDragManager() {
    while (!m_drags.empty()) {
      wl_resource_destroy(m_drags.back()->resource);
    }
    while (!m_bindings.empty()) {
      wl_resource_destroy(m_bindings.back());
    }
    wl_global_destroy(m_global);
  }

  void ToplevelDragManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* manager = static_cast<ToplevelDragManager*>(data);
    wl_resource* resource = wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, version, id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    static const struct xdg_toplevel_drag_manager_v1_interface implementation = {
        .destroy = destroyResource, .get_xdg_toplevel_drag = getDrag
    };
    wl_resource_set_implementation(resource, &implementation, manager, destroyManager);
    manager->m_bindings.push_back(resource);
  }

  void ToplevelDragManager::destroyManager(wl_resource* resource) {
    auto* manager = static_cast<ToplevelDragManager*>(wl_resource_get_user_data(resource));
    std::erase(manager->m_bindings, resource);
  }

  void ToplevelDragManager::getDrag(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* source) {
    auto* manager = static_cast<ToplevelDragManager*>(wl_resource_get_user_data(resource));
    wlr_seat* seat = manager->m_server.seat()->wlr();
    const bool used = dataSource(source) == nullptr
        || seat->selection_source == dataSource(source)
        || (seat->drag != nullptr && seat->drag->source == dataSource(source))
        || std::ranges::any_of(manager->m_drags, [source](const Drag* drag) { return drag->source == source; });
    if (used) {
      wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "source already used");
      return;
    }
    auto* drag = new (std::nothrow) Drag{.manager = manager, .source = source};
    if (drag == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    drag->resource = wl_resource_create(client, &xdg_toplevel_drag_v1_interface, 1, id);
    if (drag->resource == nullptr) {
      delete drag;
      wl_client_post_no_memory(client);
      return;
    }
    static const struct xdg_toplevel_drag_v1_interface implementation = {.destroy = requestDestroy, .attach = attach};
    wl_resource_set_implementation(drag->resource, &implementation, drag, destroyDrag);
    drag->sourceDestroy.notify = sourceDestroyed;
    wl_resource_add_destroy_listener(source, &drag->sourceDestroy);
    manager->m_drags.push_back(drag);
  }

  void ToplevelDragManager::requestDestroy(wl_client*, wl_resource* resource) {
    auto* drag = static_cast<Drag*>(wl_resource_get_user_data(resource));
    if (drag->started && !drag->ended) {
      wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG, "drag has not ended");
      return;
    }
    wl_resource_destroy(resource);
  }

  void ToplevelDragManager::destroyDrag(wl_resource* resource) {
    auto* drag = static_cast<Drag*>(wl_resource_get_user_data(resource));
    drag->manager->detach(*drag);
    if (drag->manager->m_active == drag) {
      drag->manager->m_active = nullptr;
    }
    view_detail::removeListener(drag->sourceDestroy);
    view_detail::removeListener(drag->drop);
    std::erase(drag->manager->m_drags, drag);
    delete drag;
  }

  void ToplevelDragManager::attach(wl_client*, wl_resource* resource, wl_resource* toplevel, int32_t x, int32_t y) {
    auto* drag = static_cast<Drag*>(wl_resource_get_user_data(resource));
    if (drag->toplevel != nullptr) {
      wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED, "toplevel already attached");
      return;
    }
    drag->toplevel = wlr_xdg_toplevel_from_resource(toplevel);
    if (drag->toplevel == nullptr) {
      return;
    }
    drag->x = x;
    drag->y = y;
    drag->toplevelDestroy.notify = toplevelDestroyed;
    wl_signal_add(&drag->toplevel->events.destroy, &drag->toplevelDestroy);
    if (View* view = View::fromSurface(drag->toplevel->base->surface); view != nullptr && view->mapped()) {
      drag->manager->move(*drag, view->tiled());
    }
  }

  void ToplevelDragManager::sourceDestroyed(wl_listener* listener, void*) {
    Drag* drag = wl_container_of(listener, drag, sourceDestroy);
    // Wayland emits resource destroy listeners before wlroots tears down its
    // source and drag. Place a surviving window before clearing the link.
    if (drag->manager->m_active == drag) {
      drag->manager->m_server.cursor()->finishToplevelDrag(true);
    }
    drag->manager->detach(*drag);
    drag->source = nullptr;
    drag->ended = true;
    view_detail::removeListener(drag->sourceDestroy);
  }

  void ToplevelDragManager::toplevelDestroyed(wl_listener* listener, void*) {
    Drag* drag = wl_container_of(listener, drag, toplevelDestroy);
    drag->manager->detach(*drag);
  }

  void ToplevelDragManager::dropped(wl_listener* listener, void*) {
    Drag* drag = wl_container_of(listener, drag, drop);
    drag->ended = true;
    drag->manager->m_server.cursor()->finishToplevelDrag(true);
  }

  void ToplevelDragManager::detach(Drag& drag) {
    if (m_active == &drag) {
      m_server.cursor()->finishToplevelDrag(false);
    }
    view_detail::removeListener(drag.toplevelDestroy);
    drag.toplevel = nullptr;
  }

  void ToplevelDragManager::start(wlr_drag* dataDrag) {
    for (Drag* drag : m_drags) {
      if (drag->source == nullptr || dataSource(drag->source) != dataDrag->source) {
        continue;
      }
      m_active = drag;
      drag->started = true;
      drag->drop.notify = dropped;
      wl_signal_add(&dataDrag->events.drop, &drag->drop);
      if (drag->toplevel != nullptr) {
        if (View* view = View::fromSurface(drag->toplevel->base->surface); view != nullptr && view->mapped()) {
          move(*drag, drag->tileOnDrop.value_or(view->tiled()));
        }
      }
      break;
    }
  }

  void ToplevelDragManager::finish() {
    if (m_active == nullptr) {
      return;
    }
    Drag* drag = m_active;
    drag->ended = true;
    view_detail::removeListener(drag->drop);
    m_server.cursor()->finishToplevelDrag(true);
    m_active = nullptr;
  }

  bool ToplevelDragManager::dropWithoutTarget(uint32_t button) {
    wlr_seat* seat = m_server.seat()->wlr();
    wlr_drag* dataDrag = seat->drag;
    if (m_active == nullptr
        || m_active->source == nullptr
        || m_active->toplevel == nullptr
        || dataDrag == nullptr
        || dataDrag->source == nullptr
        || dataDrag->dropped
        || seat->pointer_state.grab_button != button
        || m_server.sessionLocked()
        || wl_resource_get_version(m_active->source) < WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
      return false;
    }
    // Mirrors wlroots' drop condition in drag_handle_pointer_button.
    if (dataDrag->focus_client != nullptr && dataDrag->source->current_dnd_action != 0 && dataDrag->source->accepted) {
      return false;
    }
    // wlroots cancels a drop nobody accepted, and the protocol tells clients to
    // revert a cancelled drag. Releasing a carried window over empty space is a
    // tear-off, so complete the drag instead.
    dataDrag->dropped = true;
    wlr_data_source_dnd_drop(dataDrag->source);
    wlr_data_source_dnd_finish(dataDrag->source);
    // Ending the grab destroys the drag without destroying its source; the
    // destroy handler places the window through finish().
    wlr_seat_pointer_end_grab(seat);
    return true;
  }

  bool ToplevelDragManager::rejectSelection(wlr_data_source* source) {
    for (const Drag* drag : m_drags) {
      if (drag->source != nullptr && dataSource(drag->source) == source) {
        wl_resource_post_error(
            drag->resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "toplevel drag source used as selection"
        );
        return true;
      }
    }
    return false;
  }

  bool ToplevelDragManager::attached(const wlr_xdg_toplevel* toplevel) const {
    return toplevel != nullptr && std::ranges::any_of(m_drags, [toplevel](const Drag* drag) {
             return drag->source != nullptr && !drag->ended && drag->toplevel == toplevel;
           });
  }

  void ToplevelDragManager::move(Drag& drag, bool tileOnDrop) {
    if (m_active != &drag || drag.ended || m_server.sessionLocked()) {
      return;
    }
    View* view = View::fromSurface(drag.toplevel->base->surface);
    if (view != nullptr && view->mapped() && !view->attached()) {
      m_server.cursor()->beginToplevelDrag(view, drag.x, drag.y, tileOnDrop);
    }
  }

  void ToplevelDragManager::mapped(View* view, bool tileOnDrop) {
    for (Drag* drag : m_drags) {
      if (drag->toplevel != nullptr && drag->toplevel == view->toplevel() && !drag->ended) {
        drag->tileOnDrop = tileOnDrop;
        move(*drag, tileOnDrop);
      }
    }
  }

  void ToplevelDragManager::unmapped(View* view) {
    for (Drag* drag : m_drags) {
      if (drag->toplevel != nullptr && drag->toplevel == view->toplevel()) {
        detach(*drag);
      }
    }
  }
} // namespace umbriel
