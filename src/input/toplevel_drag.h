#pragma once

#include <cstdint>
#include <vector>
#include <wayland-server-core.h>

struct wlr_data_source;
struct wlr_drag;
struct wlr_xdg_toplevel;

namespace umbriel {
  class Server;
  class View;

  // xdg-toplevel-drag-v1 augments wlroots' data drag with an attached window.
  class ToplevelDragManager {
  public:
    explicit ToplevelDragManager(Server& server);
    ~ToplevelDragManager();
    ToplevelDragManager(const ToplevelDragManager&) = delete;
    ToplevelDragManager& operator=(const ToplevelDragManager&) = delete;

    void start(wlr_drag* drag);
    void finish();
    void dropWithoutTarget(uint32_t button);
    bool rejectSelection(wlr_data_source* source);
    [[nodiscard]] bool attached(const wlr_xdg_toplevel* toplevel) const;
    void mapped(View* view, bool tileOnDrop);
    void unmapped(View* view);

  private:
    struct Drag;
    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void destroyManager(wl_resource* resource);
    static void getDrag(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* source);
    static void destroyDrag(wl_resource* resource);
    static void requestDestroy(wl_client* client, wl_resource* resource);
    static void attach(wl_client* client, wl_resource* resource, wl_resource* toplevel, int32_t x, int32_t y);
    static void sourceDestroyed(wl_listener* listener, void* data);
    static void toplevelDestroyed(wl_listener* listener, void* data);
    static void dropped(wl_listener* listener, void* data);
    void detach(Drag& drag);
    void move(Drag& drag, bool tileOnDrop);

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_bindings;
    std::vector<Drag*> m_drags;
    Drag* m_active = nullptr;
  };
} // namespace umbriel
