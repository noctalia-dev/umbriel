#include "lock/session_lock.h"

#include "core/log.h"
#include "input/seat.h"
#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>

namespace umbriel {

  namespace {
    constexpr Logger kLog("lock");
    constexpr int kSurfaceDeadlineMs = 3000;
  } // namespace

  LockSurface::LockSurface(Server& server, wlr_session_lock_surface_v1* lockSurface, wlr_scene_tree* parent)
      : SceneNode(SceneNodeKind::LockSurface), m_server(&server), m_lockSurface(lockSurface) {
    m_lockSurface->data = this;
    m_sceneTree = parent != nullptr ? wlr_scene_subsurface_tree_create(parent, m_lockSurface->surface) : nullptr;
    if (m_sceneTree == nullptr) {
      kLog.error("failed to create lock surface scene tree");
    } else {
      m_sceneTree->node.data = sceneNodeData(this);
    }

    m_map.notify = onMap;
    wl_signal_add(&m_lockSurface->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_lockSurface->surface->events.unmap, &m_unmap);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_lockSurface->events.destroy, &m_destroy);
    m_outputCommit.notify = onOutputCommit;
    wl_signal_add(&m_lockSurface->output->events.commit, &m_outputCommit);

    configure();
  }

  LockSurface::~LockSurface() {
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_outputCommit.link);
    }
    if (m_lockSurface != nullptr && m_lockSurface->data == this) {
      m_lockSurface->data = nullptr;
    }
  }

  wlr_surface* LockSurface::surface() const { return m_lockSurface != nullptr ? m_lockSurface->surface : nullptr; }

  wlr_output* LockSurface::output() const { return m_lockSurface != nullptr ? m_lockSurface->output : nullptr; }

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

    m_server->notifyKeyboardEnter(surface);
    m_server->refreshOutputPolicies();
  }

  void LockSurface::configure() {
    if (m_lockSurface == nullptr) {
      return;
    }

    wlr_output* output = m_lockSurface->output;
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(output, &width, &height);
    wlr_session_lock_surface_v1_configure(m_lockSurface, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    if (m_sceneTree != nullptr) {
      wlr_box layoutBox{};
      wlr_output_layout_get_box(m_server->outputLayout(), output, &layoutBox);
      wlr_scene_node_set_position(&m_sceneTree->node, layoutBox.x, layoutBox.y);
    }
  }

  void LockSurface::onMap(wl_listener* listener, void* /*data*/) {
    LockSurface* self;
    self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void LockSurface::onUnmap(wl_listener* listener, void* /*data*/) {
    LockSurface* self;
    self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
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
    m_mapped = true;
    if (SessionLock* lock = m_server->sessionLock()) {
      lock->surfaceMapped(this);
      if (lock->presenting() || lock->locked()) {
        focus();
      }
    }
    m_server->updateIdleInhibit();
  }

  void LockSurface::handleUnmap() {
    m_mapped = false;
    if (SessionLock* lock = m_server->sessionLock(); lock != nullptr && (lock->presenting() || lock->locked())) {
      lock->focusMappedSurface();
    }
    m_server->updateIdleInhibit();
  }

  void LockSurface::handleDestroy() {
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_outputCommit.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_outputCommit.link.next = nullptr;
    if (m_lockSurface != nullptr && m_lockSurface->data == this) {
      m_lockSurface->data = nullptr;
    }
    m_lockSurface = nullptr;
    m_sceneTree = nullptr;
    m_mapped = false;

    if (SessionLock* lock = m_server->sessionLock()) {
      lock->removeSurface(this);
    }
  }

  void LockSurface::handleOutputCommit() { configure(); }

  SessionLock::SessionLock(Server& server, wlr_session_lock_v1* lock) : m_server(&server), m_lock(lock) {
    m_lock->data = this;

    m_sceneTree = wlr_scene_tree_create(m_server->lockTree());
    if (m_sceneTree == nullptr) {
      kLog.error("failed to create session lock scene tree");
    } else {
      wlr_scene_node_set_enabled(&m_sceneTree->node, false);
    }

    m_newSurface.notify = onNewSurface;
    wl_signal_add(&m_lock->events.new_surface, &m_newSurface);
    m_unlock.notify = onUnlock;
    wl_signal_add(&m_lock->events.unlock, &m_unlock);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_lock->events.destroy, &m_destroy);

    kLog.info("session lock requested");
  }

  SessionLock::~SessionLock() {
    if (m_surfaceDeadline != nullptr) {
      wl_event_source_remove(m_surfaceDeadline);
      m_surfaceDeadline = nullptr;
    }
    if (m_newSurface.link.next != nullptr) {
      wl_list_remove(&m_newSurface.link);
      wl_list_remove(&m_unlock.link);
      wl_list_remove(&m_destroy.link);
    }
    m_surfaces.clear();
    if (m_sceneTree != nullptr) {
      wlr_scene_node_destroy(&m_sceneTree->node);
      m_sceneTree = nullptr;
    }
    if (m_lock != nullptr && m_lock->data == this) {
      m_lock->data = nullptr;
    }
  }

  bool SessionLock::presenting() const { return m_phase == Phase::Presenting; }

  bool SessionLock::locked() const { return m_phase == Phase::Locked; }

  void SessionLock::start() {
    if (surfacesReady()) {
      beginPresenting(false);
      return;
    }

    m_surfaceDeadline =
        wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onSurfaceDeadline, this);
    if (m_surfaceDeadline == nullptr || wl_event_source_timer_update(m_surfaceDeadline, kSurfaceDeadlineMs) < 0) {
      kLog.error("failed to arm session lock surface deadline; using secure blank immediately");
      beginPresenting(true);
    }
  }

  void SessionLock::removeSurface(LockSurface* surface) {
    std::erase_if(m_surfaces, [surface](const std::unique_ptr<LockSurface>& entry) { return entry.get() == surface; });
    if (m_phase != Phase::WaitingForSurfaces) {
      focusMappedSurface();
    }
  }

  void SessionLock::handleOutputCommit(Output& output, uint32_t commitSeq) {
    if (m_phase != Phase::Presenting || !outputActive(output)) {
      return;
    }
    syncOutputPresentations();
    OutputPresentation* presentation = presentationFor(output.wlr());
    if (presentation == nullptr || presentation->presented) {
      return;
    }
    presentation->commitSeq = commitSeq;
    presentation->hasCommit = true;
  }

  void SessionLock::handleOutputPresent(Output& output, uint32_t commitSeq, bool presented) {
    if (m_phase != Phase::Presenting) {
      return;
    }
    OutputPresentation* presentation = presentationFor(output.wlr());
    if (presentation == nullptr
        || presentation->presented
        || !presentation->hasCommit
        || presentation->commitSeq != commitSeq) {
      return;
    }
    if (!presented) {
      presentation->hasCommit = false;
      output.scheduleFullFrame();
      return;
    }
    presentation->presented = true;
    maybeSendLocked();
  }

  void SessionLock::handleOutputStateChanged(Output& output) {
    if (m_phase == Phase::WaitingForSurfaces) {
      if (surfacesReady()) {
        beginPresenting(false);
      }
      return;
    }
    if (m_phase == Phase::Locked) {
      focusMappedSurface();
      return;
    }
    if (m_phase != Phase::Presenting) {
      return;
    }

    syncOutputPresentations();
    if (OutputPresentation* presentation = presentationFor(output.wlr())) {
      presentation->hasCommit = false;
      presentation->presented = false;
      output.scheduleFullFrame();
    }
    maybeSendLocked();
    focusMappedSurface();
  }

  void SessionLock::outputsChanged() {
    if (m_phase == Phase::WaitingForSurfaces) {
      if (surfacesReady()) {
        beginPresenting(false);
      }
      return;
    }
    if (m_phase == Phase::Locked) {
      focusMappedSurface();
      return;
    }
    if (m_phase != Phase::Presenting) {
      return;
    }

    syncOutputPresentations();
    for (OutputPresentation& presentation : m_outputPresentations) {
      if (!presentation.presented && !presentation.hasCommit) {
        if (Output* output = m_server->outputFromWlr(presentation.output)) {
          output->scheduleFullFrame();
        }
      }
    }
    maybeSendLocked();
    focusMappedSurface();
  }

  void SessionLock::forgetOutput(wlr_output* output) {
    std::erase_if(m_outputPresentations, [output](const OutputPresentation& entry) { return entry.output == output; });
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

  int SessionLock::onSurfaceDeadline(void* data) {
    auto* self = static_cast<SessionLock*>(data);
    self->beginPresenting(true);
    return 0;
  }

  void SessionLock::handleNewSurface(void* data) {
    auto* lockSurface = static_cast<wlr_session_lock_surface_v1*>(data);
    kLog.debug("lock surface output={}", lockSurface->output != nullptr ? lockSurface->output->name : "(none)");
    m_surfaces.push_back(std::make_unique<LockSurface>(*m_server, lockSurface, m_sceneTree));
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
      if (server->sessionLocked()) {
        kLog.warn("session lock client destroyed while locked; keeping session locked");
      } else {
        kLog.info("session lock client destroyed before lock confirmation");
      }
    }
    server->removeSessionLock(this);
  }

  void SessionLock::surfaceMapped(LockSurface* /*surface*/) {
    if (m_phase == Phase::WaitingForSurfaces && surfacesReady()) {
      beginPresenting(false);
    }
  }

  void SessionLock::beginPresenting(bool deadlineExpired) {
    if (m_phase != Phase::WaitingForSurfaces || m_lock == nullptr) {
      return;
    }
    if (m_surfaceDeadline != nullptr) {
      wl_event_source_timer_update(m_surfaceDeadline, 0);
    }
    if (deadlineExpired) {
      kLog.warn("lock surfaces were not ready after {} ms; using the secure blank", kSurfaceDeadlineMs);
    }

    m_phase = Phase::Presenting;
    if (m_sceneTree != nullptr) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, true);
    }
    m_server->activateSessionLock(this);
    syncOutputPresentations();
    for (OutputPresentation& presentation : m_outputPresentations) {
      if (Output* output = m_server->outputFromWlr(presentation.output)) {
        output->scheduleFullFrame();
      }
    }
    focusMappedSurface();
    maybeSendLocked();
  }

  void SessionLock::syncOutputPresentations() {
    std::erase_if(m_outputPresentations, [this](const OutputPresentation& presentation) {
      Output* output = m_server->outputFromWlr(presentation.output);
      return output == nullptr || !outputActive(*output);
    });
    for (const std::unique_ptr<Output>& output : m_server->outputs()) {
      if (!outputActive(*output) || presentationFor(output->wlr()) != nullptr) {
        continue;
      }
      m_outputPresentations.push_back({.output = output->wlr()});
    }
  }

  void SessionLock::maybeSendLocked() {
    if (m_phase != Phase::Presenting || m_lock == nullptr) {
      return;
    }
    syncOutputPresentations();
    if (!std::ranges::all_of(m_outputPresentations, [](const OutputPresentation& presentation) {
          return presentation.presented;
        })) {
      return;
    }

    wlr_session_lock_v1_send_locked(m_lock);
    m_phase = Phase::Locked;
    kLog.info("session is locked");
  }

  void SessionLock::focusMappedSurface() {
    const auto surface = std::ranges::find_if(m_surfaces, [this](const std::unique_ptr<LockSurface>& entry) {
      Output* output = m_server->outputFromWlr(entry->output());
      return entry->mapped() && output != nullptr && outputActive(*output);
    });
    if (surface != m_surfaces.end()) {
      (*surface)->focus();
    } else {
      m_server->notifyKeyboardClearFocus();
      m_server->refreshOutputPolicies();
    }
  }

  bool SessionLock::outputActive(const Output& output) const {
    const wlr_output* wlrOutput = output.wlr();
    return output.desktopEnabled()
        && !output.dpmsOff()
        && wlrOutput != nullptr
        && wlrOutput->enabled
        && wlrOutput->width > 0
        && wlrOutput->height > 0;
  }

  bool SessionLock::surfacesReady() const {
    return std::ranges::all_of(m_server->outputs(), [this](const std::unique_ptr<Output>& output) {
      if (!outputActive(*output)) {
        return true;
      }
      return std::ranges::any_of(m_surfaces, [&output](const std::unique_ptr<LockSurface>& surface) {
        return surface->mapped() && surface->output() == output->wlr();
      });
    });
  }

  SessionLock::OutputPresentation* SessionLock::presentationFor(wlr_output* output) {
    const auto presentation = std::ranges::find_if(m_outputPresentations, [output](const OutputPresentation& entry) {
      return entry.output == output;
    });
    return presentation != m_outputPresentations.end() ? &*presentation : nullptr;
  }

} // namespace umbriel
