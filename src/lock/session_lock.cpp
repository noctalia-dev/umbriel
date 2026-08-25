#include "lock/session_lock.h"

#include "core/log.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("lock");
  } // namespace

  LockSurface::LockSurface(Server& server, wlr_session_lock_surface_v1* lockSurface, wlr_scene_tree* parent)
      : SceneNode(SceneNodeKind::LockSurface), m_server(&server), m_lockSurface(lockSurface) {
    m_lockSurface->data = this;
    m_sceneTree = wlr_scene_subsurface_tree_create(parent, m_lockSurface->surface);
    if (m_sceneTree == nullptr) {
      kLog.error("failed to create lock surface scene tree");
      return;
    }
    m_sceneTree->node.data = sceneNodeData(this);

    m_map.notify = onMap;
    wl_signal_add(&m_lockSurface->surface->events.map, &m_map);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_lockSurface->events.destroy, &m_destroy);
    m_outputCommit.notify = onOutputCommit;
    wl_signal_add(&m_lockSurface->output->events.commit, &m_outputCommit);

    configure();
  }

  LockSurface::~LockSurface() {
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_outputCommit.link);
    }
    if (m_lockSurface != nullptr && m_lockSurface->data == this) {
      m_lockSurface->data = nullptr;
    }
  }

  wlr_surface* LockSurface::surface() const { return m_lockSurface != nullptr ? m_lockSurface->surface : nullptr; }

  void LockSurface::focus() {
    wlr_surface* surface = this->surface();
    if (surface == nullptr) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    // Session lock takes the entire seat. Cancel any popup or drag keyboard
    // grab left by the unlocked desktop before delivering the lock surface's
    // enter, otherwise the first key can disappear into that stale grab.
    bool endedGrab = false;
    if (wlr_seat_keyboard_has_grab(seat)) {
      wlr_seat_keyboard_end_grab(seat);
      endedGrab = true;
    }
    if (!endedGrab && seat->keyboard_state.focused_surface == surface) {
      return;
    }

    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
      wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
    }
    m_server->refreshOutputPolicies();
  }

  void LockSurface::configure() {
    if (m_lockSurface == nullptr || m_sceneTree == nullptr) {
      return;
    }

    wlr_output* output = m_lockSurface->output;
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(output, &width, &height);
    wlr_session_lock_surface_v1_configure(m_lockSurface, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output, &layoutBox);
    wlr_scene_node_set_position(&m_sceneTree->node, layoutBox.x, layoutBox.y);
  }

  void LockSurface::onMap(wl_listener* listener, void* /*data*/) {
    LockSurface* self;
    self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void LockSurface::onDestroy(wl_listener* listener, void* /*data*/) {
    LockSurface* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void LockSurface::onOutputCommit(wl_listener* listener, void* /*data*/) {
    LockSurface* self;
    self = wl_container_of(listener, self, m_outputCommit);
    self->handleOutputCommit();
  }

  void LockSurface::handleMap() {
    focus();
    m_server->updateIdleInhibit();
  }

  void LockSurface::handleDestroy() {
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_outputCommit.link);
    m_map.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_outputCommit.link.next = nullptr;
    if (m_lockSurface != nullptr && m_lockSurface->data == this) {
      m_lockSurface->data = nullptr;
    }
    m_lockSurface = nullptr;
    m_sceneTree = nullptr;

    if (SessionLock* lock = m_server->sessionLock()) {
      lock->removeSurface(this);
    }
  }

  void LockSurface::handleOutputCommit() { configure(); }

  SessionLock::SessionLock(Server& server, wlr_session_lock_v1* lock) : m_server(&server), m_lock(lock) {
    m_lock->data = this;

    m_newSurface.notify = onNewSurface;
    wl_signal_add(&m_lock->events.new_surface, &m_newSurface);
    m_unlock.notify = onUnlock;
    wl_signal_add(&m_lock->events.unlock, &m_unlock);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_lock->events.destroy, &m_destroy);

    kLog.info("session lock requested");
    wlr_session_lock_v1_send_locked(m_lock);
  }

  SessionLock::~SessionLock() {
    if (m_newSurface.link.next != nullptr) {
      wl_list_remove(&m_newSurface.link);
      wl_list_remove(&m_unlock.link);
      wl_list_remove(&m_destroy.link);
    }
    m_surfaces.clear();
    if (m_lock != nullptr && m_lock->data == this) {
      m_lock->data = nullptr;
    }
  }

  void SessionLock::removeSurface(LockSurface* surface) {
    std::erase_if(m_surfaces, [surface](const std::unique_ptr<LockSurface>& entry) { return entry.get() == surface; });
  }

  void SessionLock::onNewSurface(wl_listener* listener, void* data) {
    SessionLock* self;
    self = wl_container_of(listener, self, m_newSurface);
    self->handleNewSurface(data);
  }

  void SessionLock::onUnlock(wl_listener* listener, void* /*data*/) {
    SessionLock* self;
    self = wl_container_of(listener, self, m_unlock);
    self->handleUnlock();
  }

  void SessionLock::onDestroy(wl_listener* listener, void* /*data*/) {
    SessionLock* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void SessionLock::handleNewSurface(void* data) {
    auto* lockSurface = static_cast<wlr_session_lock_surface_v1*>(data);
    kLog.debug("lock surface output={}", lockSurface->output != nullptr ? lockSurface->output->name : "(none)");
    m_surfaces.push_back(std::make_unique<LockSurface>(*m_server, lockSurface, m_server->lockTree()));
  }

  void SessionLock::handleUnlock() {
    m_unlocked = true;
    kLog.info("session unlocked");
    m_server->unlockSession();
  }

  void SessionLock::handleDestroy() {
    wl_list_remove(&m_newSurface.link);
    wl_list_remove(&m_unlock.link);
    wl_list_remove(&m_destroy.link);
    m_newSurface.link.next = nullptr;
    m_unlock.link.next = nullptr;
    m_destroy.link.next = nullptr;
    if (m_lock != nullptr && m_lock->data == this) {
      m_lock->data = nullptr;
    }
    m_lock = nullptr;
    m_surfaces.clear();

    const bool unlocked = m_unlocked;
    Server* server = m_server;
    if (!unlocked) {
      kLog.warn("session lock client destroyed while locked; keeping session locked");
    }
    server->removeSessionLock(this);
  }

} // namespace umbriel
