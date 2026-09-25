#pragma once

#include "scene/node.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <wayland-server-core.h>

struct wlr_output;
struct wlr_scene_tree;
struct wlr_session_lock_surface_v1;
struct wlr_session_lock_v1;
struct wlr_surface;

namespace umbriel {

  class Output;
  class Server;

  class LockSurface : public SceneNode {
  public:
    LockSurface(Server& server, wlr_session_lock_surface_v1* lockSurface, wlr_scene_tree* parent);
    ~LockSurface();

    LockSurface(const LockSurface&) = delete;
    LockSurface& operator=(const LockSurface&) = delete;

    [[nodiscard]] wlr_session_lock_surface_v1* lockSurface() const { return m_lockSurface; }
    [[nodiscard]] wlr_surface* surface() const;
    [[nodiscard]] wlr_output* output() const;
    [[nodiscard]] bool mapped() const { return m_mapped; }
    void focus();
    void configure();

  private:
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onOutputCommit(wl_listener* listener, void* data);

    void handleMap();
    void handleUnmap();
    void handleDestroy();
    void handleOutputCommit();

    Server* m_server = nullptr;
    wlr_session_lock_surface_v1* m_lockSurface = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;
    bool m_mapped = false;

    wl_listener m_map{};
    wl_listener m_unmap{};
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
    [[nodiscard]] bool presenting() const;
    [[nodiscard]] bool locked() const;

    void start();
    void removeSurface(LockSurface* surface);
    void handleOutputCommit(Output& output, uint32_t commitSeq);
    void handleOutputPresent(Output& output, uint32_t commitSeq, bool presented);
    void handleOutputStateChanged(Output& output);
    void outputsChanged();
    void forgetOutput(wlr_output* output);

  private:
    friend class LockSurface;

    enum class Phase : uint8_t {
      WaitingForSurfaces,
      Presenting,
      Locked,
    };

    struct OutputPresentation {
      wlr_output* output = nullptr;
      uint32_t commitSeq = 0;
      bool hasCommit = false;
      bool presented = false;
    };

    static void onNewSurface(wl_listener* listener, void* data);
    static void onUnlock(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static int onSurfaceDeadline(void* data);

    void handleNewSurface(void* data);
    void handleUnlock();
    void handleDestroy();
    void surfaceMapped(LockSurface* surface);
    void beginPresenting(bool deadlineExpired);
    void syncOutputPresentations();
    void maybeSendLocked();
    void focusMappedSurface();
    [[nodiscard]] bool outputActive(const Output& output) const;
    [[nodiscard]] bool surfacesReady() const;
    [[nodiscard]] OutputPresentation* presentationFor(wlr_output* output);

    Server* m_server = nullptr;
    wlr_session_lock_v1* m_lock = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;
    wl_event_source* m_surfaceDeadline = nullptr;
    Phase m_phase = Phase::WaitingForSurfaces;
    bool m_unlocked = false;

    wl_listener m_newSurface{};
    wl_listener m_unlock{};
    wl_listener m_destroy{};

    std::vector<std::unique_ptr<LockSurface>> m_surfaces;
    std::vector<OutputPresentation> m_outputPresentations;
  };

} // namespace umbriel
