#pragma once
#include "scene/surface_blur.h"

#include <list>
#include <vector>
#include <wayland-server-core.h>

struct wlr_xdg_popup;
struct wlr_xdg_popup_configure;
struct wlr_scene_tree;

namespace umbriel {

  class Server;

  class Popup {
  public:
    explicit Popup(
        wlr_xdg_popup* popup, wlr_scene_tree* parentTree = nullptr, wlr_scene_tree* captureTree = nullptr,
        Server* server = nullptr
    );
    ~Popup();

    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;

    // Re-reads the blur configuration for every live popup; a popup commits no frame while a menu sits open, so a
    // reload would otherwise leave its blur on screen.
    static void refreshAllBlur();

  private:
    static std::vector<Popup*>& livePopups();

    void updateBlur();

    static void onCommit(wl_listener* listener, void* data);
    static void onReposition(wl_listener* listener, void* data);
    static void onConfigure(wl_listener* listener, void* data);
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleCommit();
    void handleReposition();
    void handleConfigure();
    void unconstrain();
    void handleMap();
    void handleDestroy();

    wlr_xdg_popup* m_popup = nullptr;
    Server* m_server = nullptr;
    SurfaceBlur m_blur;
    std::list<wlr_xdg_popup_configure> m_repositions;

    wl_listener m_commit{};
    wl_listener m_reposition{};
    wl_listener m_configure{};
    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
