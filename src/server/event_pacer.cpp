#include "server/event_pacer.h"

#include "core/log.h"

#include <utility>

namespace umbriel {

  namespace {
    constexpr Logger kLog("ipc");
  } // namespace

  EventPacer::EventPacer(wl_event_loop* loop, int intervalMs, std::function<void()> emit)
      : m_loop(loop), m_intervalMs(intervalMs), m_emit(std::move(emit)),
        m_interval(wl_event_loop_add_timer(loop, onIntervalEnd, this)) {
    if (m_interval == nullptr) {
      kLog.error("failed to register event pacing timer, events will go out unpaced");
    }
  }

  EventPacer::~EventPacer() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    if (m_interval != nullptr) {
      wl_event_source_remove(m_interval);
    }
  }

  void EventPacer::schedule() {
    if (m_inInterval) {
      m_changedInInterval = true;
      return;
    }
    if (m_idle == nullptr) {
      m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);
      if (m_idle == nullptr) {
        kLog.error("failed to register event pacing idle source, dropping a change notification");
      }
    }
  }

  void EventPacer::emit() {
    if (m_interval != nullptr) {
      m_inInterval = wl_event_source_timer_update(m_interval, m_intervalMs) == 0;
      if (!m_inInterval) {
        kLog.error("failed to arm event pacing timer, this event goes out unpaced");
      }
    }
    m_emit();
  }

  void EventPacer::endInterval() {
    m_inInterval = false;
    if (m_changedInInterval) {
      m_changedInInterval = false;
      emit();
    }
  }

#ifdef UMBRIEL_HARNESS
  void EventPacer::setIntervalHeld(bool held) {
    m_intervalHeld = held;
    if (!held) {
      endInterval();
      return;
    }
    m_inInterval = true;
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
      m_idle = nullptr;
      m_changedInInterval = true;
    }
  }
#endif

  void EventPacer::onIdle(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
    pacer->m_idle = nullptr;
    pacer->emit();
  }

  int EventPacer::onIntervalEnd(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
#ifdef UMBRIEL_HARNESS
    if (pacer->m_intervalHeld) {
      return 0;
    }
#endif
    pacer->endInterval();
    return 0;
  }

} // namespace umbriel
