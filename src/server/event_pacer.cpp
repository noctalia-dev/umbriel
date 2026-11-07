#include "server/event_pacer.h"

#include <utility>

namespace umbriel {

  EventPacer::EventPacer(wl_event_loop* loop, int intervalMs, std::function<void()> emit)
      : m_loop(loop), m_intervalMs(intervalMs), m_emit(std::move(emit)),
        m_interval(wl_event_loop_add_timer(loop, onIntervalEnd, this)) {}

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
    }
  }

  void EventPacer::emit() {
    m_inInterval = m_interval != nullptr && wl_event_source_timer_update(m_interval, m_intervalMs) == 0;
    m_emit();
  }

  void EventPacer::onIdle(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
    pacer->m_idle = nullptr;
    pacer->emit();
  }

  int EventPacer::onIntervalEnd(void* data) {
    auto* pacer = static_cast<EventPacer*>(data);
    pacer->m_inInterval = false;
    if (pacer->m_changedInInterval) {
      pacer->m_changedInInterval = false;
      pacer->emit();
    }
    return 0;
  }

} // namespace umbriel
