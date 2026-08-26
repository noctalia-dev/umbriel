#include "input/gestures.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    // Tuning constants: file-local, no config keys.
    constexpr double kAxisLockPx = 16.0;
    constexpr double kScrollFactor = 3.0;
    constexpr double kSwitchDistancePx = 300.0;
    constexpr double kCommitProgress = 0.35;
    constexpr double kCommitVelocityPxMs = 0.9;
    constexpr double kOverscrollCompress = 0.15;
    constexpr double kOverscrollMaxWs = 0.08;
    // Finger travel for a full overview open or close.
    constexpr double kOverviewDistancePx = 300.0;
    // Finger travel per workspace step while the overview is up. Outside it, a switch commits once the swipe passes
    // kCommitProgress of a full slide, so that same travel is what the hand already reads as "one workspace"; there is
    // no slide to be a fraction of in here, so it becomes the step itself.
    constexpr double kOverviewStepPx = kSwitchDistancePx * kCommitProgress;
  } // namespace

  // trampolines (same pattern as Cursor)
  void Gestures::onSwipeBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeBegin);
    self->handleSwipeBegin(data);
  }
  void Gestures::onSwipeUpdate(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeUpdate);
    self->handleSwipeUpdate(data);
  }
  void Gestures::onSwipeEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeEnd);
    self->handleSwipeEnd(data);
  }
  void Gestures::onPinchBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchBegin);
    self->handlePinchBegin(data);
  }
  void Gestures::onPinchUpdate(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchUpdate);
    self->handlePinchUpdate(data);
  }
  void Gestures::onPinchEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchEnd);
    self->handlePinchEnd(data);
  }
  void Gestures::onHoldBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_holdBegin);
    self->handleHoldBegin(data);
  }
  void Gestures::onHoldEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_holdEnd);
    self->handleHoldEnd(data);
  }

  Gestures::Gestures(Server& server) : m_server(&server) {
    wlr_cursor* cursor = m_server->cursor()->wlr();

    m_swipeBegin.notify = onSwipeBegin;
    wl_signal_add(&cursor->events.swipe_begin, &m_swipeBegin);
    m_swipeUpdate.notify = onSwipeUpdate;
    wl_signal_add(&cursor->events.swipe_update, &m_swipeUpdate);
    m_swipeEnd.notify = onSwipeEnd;
    wl_signal_add(&cursor->events.swipe_end, &m_swipeEnd);
    m_pinchBegin.notify = onPinchBegin;
    wl_signal_add(&cursor->events.pinch_begin, &m_pinchBegin);
    m_pinchUpdate.notify = onPinchUpdate;
    wl_signal_add(&cursor->events.pinch_update, &m_pinchUpdate);
    m_pinchEnd.notify = onPinchEnd;
    wl_signal_add(&cursor->events.pinch_end, &m_pinchEnd);
    m_holdBegin.notify = onHoldBegin;
    wl_signal_add(&cursor->events.hold_begin, &m_holdBegin);
    m_holdEnd.notify = onHoldEnd;
    wl_signal_add(&cursor->events.hold_end, &m_holdEnd);
  }

  Gestures::~Gestures() {
    wl_list_remove(&m_swipeBegin.link);
    wl_list_remove(&m_swipeUpdate.link);
    wl_list_remove(&m_swipeEnd.link);
    wl_list_remove(&m_pinchBegin.link);
    wl_list_remove(&m_pinchUpdate.link);
    wl_list_remove(&m_pinchEnd.link);
    wl_list_remove(&m_holdBegin.link);
    wl_list_remove(&m_holdEnd.link);
  }

  void Gestures::cancelForOutput(Output* output) {
    if (m_output == output
        && (m_state == State::Pending
            || m_state == State::Scroll
            || m_state == State::Switch
            || m_state == State::OverviewSelect)) {
      // Hard reset: do NOT call into workspace/group objects (they may be mid-destruction).
      m_output = nullptr;
      m_scrollWorkspace = nullptr;
      m_switchGroup = nullptr;
      m_state = State::Idle;
    }
  }

  void Gestures::cancelActive() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true);
      break;
    case State::Switch:
      finishSwitch(true);
      break;
    case State::Overview:
      finishOverview(true);
      break;
    case State::Forward:
      // Forward a cancel end so clients see the end.
      wlr_pointer_gestures_v1_send_swipe_end(m_server->pointerGestures(), m_server->seat()->wlr(), 0, true);
      m_state = State::Idle;
      break;
    case State::OverviewSelect:
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  // Restore layout/slide state without sending any protocol events.
  // Used when the session locks mid-gesture.
  void Gestures::silentCancel() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true);
      break;
    case State::Switch:
      finishSwitch(true);
      break;
    case State::Overview:
      finishOverview(true);
      break;
    case State::Forward:
    case State::OverviewSelect:
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  // ===== Swipe handlers =====

  void Gestures::handleSwipeBegin(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_begin_event*>(data);
    m_server->notifyIdleActivity();
    m_server->cancelModifierTap();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }
    if (m_state != State::Idle) {
      cancelActive();
    }
    Overview* overview = m_server->overview();
    if (event->fingers == 4 && overview != nullptr) {
      m_state = State::Overview;
      m_accumX = 0;
      m_accumY = 0;
      m_overviewWasOpen = overview->active();
      m_progress = m_overviewWasOpen ? 1.0 : 0.0;
      m_velocity = 0;
      m_lastTimeMsec = event->time_msec;
      return;
    }
    if (event->fingers == 3) {
      // Which of the three-finger gestures this is (scroll, switch, or an
      // overview row step) is decided once the axis locks, not here.
      m_state = State::Pending;
      m_accumX = 0;
      m_accumY = 0;
      m_output = nullptr;
      return;
    }
    m_state = State::Forward;
    wlr_pointer_gestures_v1_send_swipe_begin(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
    );
  }

  void Gestures::handleSwipeUpdate(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_update_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }

    switch (m_state) {
    case State::Forward:
      wlr_pointer_gestures_v1_send_swipe_update(
          m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->dx, event->dy
      );
      return;

    case State::Pending: {
      m_accumX += event->dx;
      m_accumY += event->dy;
      const double maxAccum = std::max(std::abs(m_accumX), std::abs(m_accumY));
      if (maxAccum < kAxisLockPx) {
        return; // Not enough travel to decide axis.
      }
      // Resolve output.
      wlr_output* wlrOut = m_server->preferredOutput();
      Output* out = m_server->outputFromWlr(wlrOut);
      if (out == nullptr || out->workspaceGroup() == nullptr) {
        m_state = State::Idle;
        return;
      }
      m_output = out;

      if (Overview* overview = m_server->overview(); overview != nullptr && overview->interactive()) {
        // Horizontal has no meaning over the filmstrip, and letting it through
        // would step rows on any swipe that drifted off true.
        if (std::abs(m_accumX) > std::abs(m_accumY)) {
          m_state = State::Idle;
          return;
        }
        // Start measuring row travel from the lock point, not the touch down.
        m_accumY = 0;
        m_state = State::OverviewSelect;
        return;
      }

      if (std::abs(m_accumX) > std::abs(m_accumY)) {
        // Horizontal lock scrolls the active workspace.
        Workspace* ws = out->workspaceGroup()->active();
        ScrollingLayout* scrolling = ws != nullptr ? ws->scrollingLayout() : nullptr;
        if (scrolling == nullptr || scrolling->columns().empty()) {
          m_state = State::Idle;
          return;
        }
        if (ws->scrollingVertical()) {
          m_state = State::Idle;
          return;
        }
        m_scrollWorkspace = ws;
        m_viewportPrimary = ws->scrollViewportExtent();
        m_scrollStart = scrolling->scroll();
        m_scrollStartCentered = scrolling->centeredRest();
        ws->markArrange(false);
        m_state = State::Scroll;
      } else {
        // Vertical lock switches workspaces.
        WorkspaceGroup* group = out->workspaceGroup();
        const size_t idx = group->active()->index();
        m_hasPrev = idx > 0;
        m_hasNext = idx + 1 < group->workspaceCount();
        if (!group->slideBegin(m_hasPrev, m_hasNext)) {
          m_state = State::Idle;
          return;
        }
        m_switchGroup = group;
        m_progress = 0;
        m_velocity = 0;
        m_lastTimeMsec = event->time_msec;
        m_state = State::Switch;
      }
      return;
    }

    case State::Scroll: {
      // Abort if active workspace changed under us.
      Output* out = m_server->outputFromWlr(m_server->preferredOutput());
      if (out == nullptr || out->workspaceGroup() == nullptr || out->workspaceGroup()->active() != m_scrollWorkspace) {
        m_state = State::Idle;
        m_scrollWorkspace = nullptr;
        return;
      }
      m_accumX += event->dx;
      // Natural: fingers left → content moves left → scroll increases.
      double target = m_scrollStart - m_accumX * kScrollFactor;
      ScrollingLayout* scrolling = m_scrollWorkspace->scrollingLayout();
      if (scrolling == nullptr) {
        m_state = State::Idle;
        return;
      }
      const auto maxScroll = static_cast<double>(scrolling->maxScroll(m_viewportPrimary));
      // The strip can rest past its own edge (a centered end column sits half a viewport out), so measure the
      // rubber-band from where it is rather than from the edges, or the first finger movement yanks it back in.
      const double restLow = std::min(m_scrollStart, 0.0);
      const double restHigh = std::max(m_scrollStart, maxScroll);
      if (target < restLow) {
        target = std::max(restLow + (target - restLow) * kOverscrollCompress, restLow - 0.1 * m_viewportPrimary);
      }
      if (target > restHigh) {
        target = std::min(restHigh + (target - restHigh) * kOverscrollCompress, restHigh + 0.1 * m_viewportPrimary);
      }
      scrolling->setScroll(target);
      m_scrollWorkspace->markArrange(false);
      return;
    }

    case State::Switch: {
      // Abort if group changed.
      Output* out = m_server->outputFromWlr(m_server->preferredOutput());
      if (out == nullptr || out->workspaceGroup() != m_switchGroup) {
        m_switchGroup->slideFinish();
        m_switchGroup = nullptr;
        m_state = State::Idle;
        return;
      }
      m_accumY += event->dy;
      // Natural: swipe up (negative dy) → next workspace (positive progress).
      double p = -m_accumY / kSwitchDistancePx;
      const double lo = m_hasPrev ? -1.0 : 0.0;
      const double hi = m_hasNext ? 1.0 : 0.0;
      if (p < lo) {
        p = std::max(lo + (p - lo) * kOverscrollCompress, lo - kOverscrollMaxWs);
      }
      if (p > hi) {
        p = std::min(hi + (p - hi) * kOverscrollCompress, hi + kOverscrollMaxWs);
      }
      const uint32_t dt = std::max(1U, event->time_msec - m_lastTimeMsec);
      m_velocity = 0.75 * m_velocity + 0.25 * (-event->dy / static_cast<double>(dt));
      m_lastTimeMsec = event->time_msec;
      m_progress = p;
      m_switchGroup->slideApply(p);
      return;
    }

    case State::OverviewSelect: {
      Overview* overview = m_server->overview();
      if (overview == nullptr || !overview->interactive()) {
        m_state = State::Idle;
        return;
      }
      m_accumY += event->dy;
      // Natural, and the same sense as the switch outside the overview: swipe up (negative dy) moves to the next
      // workspace. The leftover travel stays in m_accumY so one long swipe crosses several rows.
      while (m_accumY <= -kOverviewStepPx) {
        m_accumY += kOverviewStepPx;
        overview->selectRelativeWorkspace(1, m_output);
      }
      while (m_accumY >= kOverviewStepPx) {
        m_accumY -= kOverviewStepPx;
        overview->selectRelativeWorkspace(-1, m_output);
      }
      return;
    }

    case State::Overview: {
      Overview* overview = m_server->overview();
      if (overview == nullptr) {
        m_state = State::Idle;
        return;
      }
      m_accumY += event->dy;
      // Swipe up opens, swipe down closes; base is where the gesture started.
      const double base = m_overviewWasOpen ? 1.0 : 0.0;
      const double p = std::clamp(base - m_accumY / kOverviewDistancePx, 0.0, 1.0);
      const uint32_t dt = std::max(1U, event->time_msec - m_lastTimeMsec);
      m_velocity = 0.75 * m_velocity + 0.25 * (-event->dy / static_cast<double>(dt));
      m_lastTimeMsec = event->time_msec;
      m_progress = p;
      overview->gestureUpdate(p);
      return;
    }

    case State::Idle:
      return;
    }
  }

  void Gestures::handleSwipeEnd(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_end_event*>(data);
    m_server->notifyIdleActivity();

    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }

    switch (m_state) {
    case State::Forward:
      wlr_pointer_gestures_v1_send_swipe_end(
          m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
      );
      m_state = State::Idle;
      return;

    case State::Pending:
    // Each row step was committed as it happened; there is nothing to settle.
    case State::OverviewSelect:
      m_state = State::Idle;
      return;

    case State::Scroll:
      finishScroll(event->cancelled);
      return;

    case State::Switch:
      finishSwitch(event->cancelled);
      return;

    case State::Overview:
      finishOverview(event->cancelled);
      return;

    case State::Idle:
      return;
    }
  }

  // ===== Overview finish (4-finger) =====

  void Gestures::finishOverview(bool cancelled) {
    m_state = State::Idle;
    Overview* overview = m_server->overview();
    if (overview == nullptr) {
      return;
    }
    bool commitOpen = m_overviewWasOpen;
    if (!cancelled) {
      const double base = m_overviewWasOpen ? 1.0 : 0.0;
      const bool farEnough = std::abs(m_progress - base) > kCommitProgress;
      // Positive velocity is a swipe up, which only commits an opening gesture.
      const bool fastEnough = std::abs(m_velocity) > kCommitVelocityPxMs && (m_velocity > 0) == !m_overviewWasOpen;
      if (farEnough || fastEnough) {
        commitOpen = !m_overviewWasOpen;
      }
    }
    overview->gestureEnd(commitOpen);
  }

  // ===== Scroll finish (Step 5) =====

  void Gestures::finishScroll(bool cancelled) {
    if (m_scrollWorkspace == nullptr) {
      m_state = State::Idle;
      return;
    }
    ScrollingLayout* scrolling = m_scrollWorkspace->scrollingLayout();
    if (scrolling == nullptr) {
      m_state = State::Idle;
      m_scrollWorkspace = nullptr;
      return;
    }
    if (cancelled) {
      scrolling->setScroll(m_scrollStart, m_scrollStartCentered);
      m_scrollWorkspace->markArrange(true);
    } else {
      // Snap to the column nearest the viewport center.
      const ScrollingLayout& layout = *scrolling;
      const auto maxScroll = static_cast<double>(layout.maxScroll(m_viewportPrimary));
      const double rawScroll = layout.scroll();
      const double currentScroll = std::clamp(rawScroll, 0.0, maxScroll);
      int best = -1;

      // The viewport-center heuristic has no snap point at either strip edge. With mixed column widths, the second
      // column can remain closer to the viewport center even at scroll 0, making the first column unreachable. Preserve
      // the gesture direction at an overscrolled edge by explicitly selecting that endpoint.
      if (m_accumX > 0.0 && rawScroll <= 0.0) {
        best = 0;
      } else if (m_accumX < 0.0 && rawScroll >= maxScroll) {
        best = static_cast<int>(layout.columns().size()) - 1;
      } else {
        const double center = currentScroll + m_viewportPrimary / 2.0;
        double bestDist = 1e18;
        for (int i = 0; i < static_cast<int>(layout.columns().size()); ++i) {
          const double colCenter = static_cast<double>(layout.columnX(i, m_viewportPrimary))
              + layout.columnWidth(i, m_viewportPrimary) / 2.0;
          const double dist = std::abs(colCenter - center);
          if (dist < bestDist) {
            bestDist = dist;
            best = i;
          }
        }
      }
      if (best >= 0) {
        View* focused = m_scrollWorkspace->focusedView();
        if (focused != nullptr && layout.columnOf(focused) == best) {
          // Snap back / cancel: nearest column is already focused.
          m_scrollWorkspace->ensureFocusedVisible();
          m_scrollWorkspace->markArrange(true);
        } else if (
            best < static_cast<int>(layout.columns().size())
            && !layout.columns()[static_cast<size_t>(best)].views.empty()
        ) {
          View* target = layout.columns()[static_cast<size_t>(best)].views.front();
          m_server->focusView(target, FocusReason::Directional);
        } else {
          m_scrollWorkspace->ensureFocusedVisible();
          m_scrollWorkspace->markArrange(true);
        }
      } else {
        m_scrollWorkspace->ensureFocusedVisible();
        m_scrollWorkspace->markArrange(true);
      }
      scrolling->setScroll(std::clamp(layout.scroll(), 0.0, maxScroll));
      m_scrollWorkspace->markArrange(true);
    }
    m_scrollWorkspace = nullptr;
    m_state = State::Idle;
  }

  // ===== Switch finish (Step 6) =====

  void Gestures::finishSwitch(bool cancelled) {
    if (m_switchGroup == nullptr) {
      m_state = State::Idle;
      return;
    }
    int delta = 0;
    if (!cancelled) {
      const double lo = m_hasPrev ? -1.0 : 0.0;
      const double hi = m_hasNext ? 1.0 : 0.0;
      const double clamped = std::clamp(m_progress, lo, hi);
      if (std::abs(clamped) >= kCommitProgress) {
        delta = clamped > 0 ? 1 : -1;
      } else if (std::abs(m_velocity) >= kCommitVelocityPxMs && m_velocity * clamped > 0) {
        delta = clamped > 0 ? 1 : -1;
      }
    }
    m_switchGroup->slideSettle(delta);
    if (delta != 0) {
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
    }
    m_switchGroup = nullptr;
    m_state = State::Idle;
  }

  // ===== Pinch handlers (forward unconditionally) =====

  void Gestures::handlePinchBegin(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_begin_event*>(data);
    m_server->notifyIdleActivity();
    m_server->cancelModifierTap();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_begin(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
    );
  }

  void Gestures::handlePinchUpdate(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_update_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_update(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->dx, event->dy, event->scale,
        event->rotation
    );
  }

  void Gestures::handlePinchEnd(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_end_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_end(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
    );
  }

  // ===== Hold handlers (forward unconditionally) =====

  void Gestures::handleHoldBegin(void* data) {
    auto* event = static_cast<wlr_pointer_hold_begin_event*>(data);
    m_server->notifyIdleActivity();
    m_server->cancelModifierTap();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_hold_begin(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
    );
  }

  void Gestures::handleHoldEnd(void* data) {
    auto* event = static_cast<wlr_pointer_hold_end_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_hold_end(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
    );
  }

} // namespace umbriel
