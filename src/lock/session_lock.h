#pragma once

#include "scene/node.h"

#include <memory>
#include <vector>
#include <wayland-server-core.h>

struct wlr_scene_tree;
struct wlr_session_lock_surface_v1;
struct wlr_session_lock_v1;
struct wlr_surface;

namespace umbriel {

  class Server;

  class LockSurface : public SceneNode {
  public:
    LockSurface(Server& server, wlr_session_lock_surface_v1* lockSurface, wlr_scene_tree* parent);
    ~LockSurface();

    LockSurface(const LockSurface&) = delete;
    LockSurface& operator=(const LockSurface&) = delete;

    [[nodiscard]] wlr_session_lock_surface_v1* lockSurface() const { return m_lockSurface; }
    [[nodiscard]] wlr_surface* surface() const;
    void focus();
    void configure();

  private:
    static void onMap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onOutputCommit(wl_listener* listener, void* data);

    void handleMap();
    void handleDestroy();
    void handleOutputCommit();

    Server* m_server = nullptr;
    wlr_session_lock_surface_v1* m_lockSurface = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;

    wl_listener m_map{};
    wl_listener m_destroy{};
    wl_listener m_outputCommit{};
  };

  class SessionLock {
  public:
    SessionLock(Server& server, wlr_session_lock_v1* lock);
    ~SessionLock();

    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    [[nodiscard]] wlr_session_lock_v1* lock() const { return m_lock; }
    [[nodiscard]] bool unlocked() const { return m_unlocked; }

    void removeSurface(LockSurface* surface);

  private:
    static void onNewSurface(wl_listener* listener, void* data);
    static void onUnlock(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleNewSurface(void* data);
    void handleUnlock();
    void handleDestroy();

    Server* m_server = nullptr;
    wlr_session_lock_v1* m_lock = nullptr;
    bool m_unlocked = false;

    wl_listener m_newSurface{};
    wl_listener m_unlock{};
    wl_listener m_destroy{};

    std::vector<std::unique_ptr<LockSurface>> m_surfaces;
  };

} // namespace umbriel
