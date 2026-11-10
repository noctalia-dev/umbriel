#pragma once

#include <memory>
#include <wayland-server-core.h>

struct wlr_cursor_shape_manager_v1;
struct wlr_seat;
struct wlr_seat_client;

namespace umbriel {

  class Server;
  class ToplevelDragManager;

  class Seat {
  public:
    explicit Seat(Server& server);
    ~Seat();

    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    wlr_seat* wlr() const { return m_seat; }

    ToplevelDragManager& toplevelDrags() const { return *m_toplevelDrags; }

    void applyConfig();
    void updateCapabilities(bool hasKeyboard, bool hasTouch);
    void notifyPointerModifiers(bool clear = false);

  private:
    static void onRequestCursor(wl_listener* listener, void* data);
    static void onRequestSetShape(wl_listener* listener, void* data);
    static void onPointerFocusChange(wl_listener* listener, void* data);
    static void onRequestSetSelection(wl_listener* listener, void* data);
    static void onRequestSetPrimarySelection(wl_listener* listener, void* data);
    static void onRequestStartDrag(wl_listener* listener, void* data);
    static void onStartDrag(wl_listener* listener, void* data);
    static void onDragDestroy(wl_listener* listener, void* data);

    bool ownsPointerCursor(const wlr_seat_client* client) const;
    void handleRequestCursor(void* data);
    void handleRequestSetShape(void* data);
    void handlePointerFocusChange(void* data);
    void handleRequestSetSelection(void* data);
    void handleRequestSetPrimarySelection(void* data);
    void handleRequestStartDrag(void* data);
    void handleStartDrag(void* data);
    void handleDragDestroy();
    void sendPointerModifiers(wlr_seat_client* client, bool clear);

    std::unique_ptr<ToplevelDragManager> m_toplevelDrags;
    Server* m_server = nullptr;
    wlr_seat* m_seat = nullptr;
    wlr_cursor_shape_manager_v1* m_cursorShapeManager = nullptr;
    bool m_primarySelectionEnabled = true;

    wl_listener m_requestCursor{};
    wl_listener m_requestSetShape{};
    wl_listener m_pointerFocusChange{};
    wl_listener m_requestSetSelection{};
    wl_listener m_requestSetPrimarySelection{};
    wl_listener m_requestStartDrag{};
    wl_listener m_startDrag{};
    wl_listener m_dragDestroy{};
  };

} // namespace umbriel
