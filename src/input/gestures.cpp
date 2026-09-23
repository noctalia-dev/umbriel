#include "input/gestures.h"

#include "input/cursor.h"
#include "input/gesture_physics.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <chrono>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    // Finger travel for a full overview open or close. One workspace of the three-finger switch travels the same
    // distance, so the hand reads one gesture distance on either side of the overview.
    constexpr double kOverviewDistancePx = 300.0;

    // Monotonic milliseconds, on the same clock the input path timestamps gesture events with. The overview taking
    // over a swipe needs a release sample and has no event to read one from.
    uint32_t nowMsec() {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }

    int touchpadGestureDirection(wlr_pointer* pointer) {
      if (pointer == nullptr || !wlr_input_device_is_libinput(&pointer->base)) {
        return 1;
      }
      libinput_device* device = wlr_libinput_get_device_handle(&pointer->base);
      if (device == nullptr || libinput_device_config_scroll_has_natural_scroll(device) == 0) {
        return 1;
      }
      return libinput_device_config_scroll_get_natural_scroll_enabled(device) != 0 ? 1 : -1;
    }

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
      m_scrollSource = ScrollSource::None;
      m_state = State::Idle;
    }
  }

  void Gestures::cancelForLayoutChange() {
    if (m_state == State::Idle || m_state == State::Forward) {
      return;
    }
    // The old layout objects are still installed here, so the settling paths in
    // cancelActive() can restore them before they are replaced.
    cancelActive();
    m_output = nullptr;
    m_scrollWorkspace = nullptr;
    m_switchGroup = nullptr;
    m_scrollSource = ScrollSource::None;
    m_state = State::Idle;
  }

  Gestures::SwitchPick Gestures::pickSwitchForOverview() {
    if (m_state != State::Switch) {
      return {};
    }
    WorkspaceGroup* group = m_switchGroup;
    Workspace* base = group != nullptr && group->slideActive() ? group->active() : nullptr;
    // Read the release before clearing the state: the filmstrip picks the swipe up from where the fingers left it, and
    // the projection is what decides which row that is.
    const GesturePhysics::StepRelease settle =
        base != nullptr ? switchSettle(nowMsec()) : GesturePhysics::StepRelease{};
    m_switchGroup = nullptr;
    m_output = nullptr;
    m_state = State::Idle;
    if (base == nullptr) {
      return {};
    }
    int delta = settle.target;
    const size_t index = base->index();
    Workspace* target = base;
    if (delta < 0 && index > 0) {
      target = group->workspaceAt(index - 1);
    } else if (delta > 0 && index + 1 < group->workspaceCount()) {
      target = group->workspaceAt(index + 1);
    } else {
      delta = 0;
    }
    return {
        .group = group,
        .target = target,
        .offset = settle.position - delta,
        .velocity = settle.velocity,
    };
  }

  void Gestures::cancelActive() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true, 0);
      break;
    case State::Switch:
      finishSwitch(true, 0);
      break;
    case State::Overview:
      finishOverview(true, 0);
      break;
    case State::Forward:
      // Forward a cancel end so clients see the end.
      wlr_pointer_gestures_v1_send_swipe_end(m_server->pointerGestures(), m_server->seat()->wlr(), 0, true);
      m_state = State::Idle;
      break;
    case State::OverviewSelect:
      if (Overview* overview = m_server->overview()) {
        overview->endNavigation(true, 0, NavigationSource::Swipe);
      }
      m_state = State::Idle;
      break;
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  bool Gestures::beginScroll(Workspace* workspace, double scale, ScrollSource source) {
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr || scrolling->columns().empty()) {
      return false;
    }
    m_scrollWorkspace = workspace;
    m_viewportPrimary = workspace->scrollViewportExtent();
    m_scrollStart = scrolling->scroll();
    m_scrollTracker.reset();
    m_scrollStartCentered = scrolling->centeredRest();
    m_scrollScale = scale;
    m_scrollVertical = workspace->scrollingVertical();
    m_scrollSource = source;
    workspace->markArrange(false);
    m_state = State::Scroll;
    return true;
  }

  void Gestures::updateScroll(double delta, uint32_t timeMsec) {
    if (m_state != State::Scroll || m_scrollWorkspace == nullptr) {
      return;
    }
    WorkspaceGroup* group = m_output != nullptr ? m_output->workspaceGroup() : nullptr;
    const bool overviewPointer = m_scrollSource == ScrollSource::OverviewPointer;
    if (group == nullptr || (!overviewPointer && group->active() != m_scrollWorkspace)) {
      finishScroll(true, timeMsec);
      return;
    }
    ScrollingLayout* scrolling = m_scrollWorkspace->scrollingLayout();
    if (scrolling == nullptr) {
      finishScroll(true, timeMsec);
      return;
    }
    m_scrollTracker.push(delta, timeMsec);
    scrolling->setScroll(m_scrollStart - m_scrollTracker.pos() * m_scrollScale);
    m_scrollWorkspace->markArrange(false);
  }

  void Gestures::updateSwitch(double travel, uint32_t timeMsec) {
    if (m_state != State::Switch || m_switchGroup == nullptr) {
      return;
    }
    // Natural: swiping toward the negative axis direction moves to the next workspace.
    m_switchTracker.push(-travel * m_naturalScrollDirection, timeMsec);
    const double lo = m_hasPrev ? -1.0 : 0.0;
    const double hi = m_hasNext ? 1.0 : 0.0;
    const double position = GesturePhysics::rubberBand(
        m_switchStart + m_switchTracker.pos() / kSwipeWorkspacePx, lo, hi, GesturePhysics::kOverscrollLimit
    );
    m_switchGroup->slideApply(position);
  }

  GesturePhysics::StepRelease Gestures::switchSettle(uint32_t timeMsec) {
    // Idle time between the last motion event and the release still bleeds speed, so feed a zero-delta sample before
    // reading the tracker. The strip scroll does the same for the same reason.
    m_switchTracker.push(0.0, timeMsec);
    return GesturePhysics::release(
        m_switchTracker, kSwipeWorkspacePx, m_switchStart, m_hasPrev ? -1.0 : 0.0, m_hasNext ? 1.0 : 0.0
    );
  }

  bool Gestures::beginPointerScroll(double lx, double ly) {
    if (m_server->sessionLocked()) {
      return false;
    }
    if (m_state != State::Idle) {
      cancelActive();
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      // The row under the pointer, not the active one: the overview pans whichever row the drag started over, and the
      // strip it scrolls always runs across the workspace axis.
      Workspace* workspace = overview->pointerScrollWorkspace(lx, ly);
      WorkspaceGroup* group = workspace != nullptr ? workspace->group() : nullptr;
      m_output = group != nullptr ? group->output() : nullptr;
      // Previews are drawn at the settled zoom, so travel over one covers that much more of the content below it.
      if (m_output == nullptr
          || !beginScroll(workspace, 1.0 / Overview::settledZoom(), ScrollSource::OverviewPointer)) {
        m_output = nullptr;
        return false;
      }
      return true;
    }
    Output* output = m_server->outputFromWlr(m_server->preferredOutput());
    Workspace* workspace =
        output != nullptr && output->workspaceGroup() != nullptr ? output->workspaceGroup()->active() : nullptr;
    m_output = output;
    if (!beginScroll(workspace, 1.0, ScrollSource::Pointer)) {
      m_output = nullptr;
      return false;
    }
    return true;
  }

  void Gestures::updatePointerScroll(double dx, double dy, uint32_t timeMsec) {
    if (pointerScrollActive()) {
      updateScroll(m_scrollVertical ? dy : dx, timeMsec);
    }
  }

  void Gestures::endPointerScroll(bool cancelled, uint32_t timeMsec) {
    if (pointerScrollActive()) {
      finishScroll(cancelled, timeMsec);
    }
  }

  // Restore layout/slide state without sending any protocol events.
  // Used when the session locks mid-gesture.
  void Gestures::silentCancel() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true, 0);
      break;
    case State::Switch:
      finishSwitch(true, 0);
      break;
    case State::Overview:
      finishOverview(true, 0);
      break;
    case State::OverviewSelect:
      if (Overview* overview = m_server->overview()) {
        overview->endNavigation(true, 0, NavigationSource::Swipe);
      }
      m_state = State::Idle;
      break;
    case State::Forward:
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  // ===== Swipe handlers =====

  void Gestures::handleSwipeBegin(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_begin_event*>(data);
    m_server->notifyInputActivity();
    m_server->cancelModifierTap();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }
    if (pointerScrollActive()) {
      return;
    }
    if (m_state != State::Idle) {
      cancelActive();
    }
    Overview* overview = m_server->overview();
    if (event->fingers == 4 && overview != nullptr) {
      overview->cancelNavigation();
      m_state = State::Overview;
      m_accumX = 0;
      m_accumY = 0;
      m_overviewWasOpen = overview->active();
      m_overviewTracker.reset();
      return;
    }
    if (event->fingers == 3) {
      m_naturalScrollDirection = touchpadGestureDirection(event->pointer);
      if (overview != nullptr && overview->active()) {
        const wlr_cursor* cursor = m_server->cursor()->wlr();
        overview->beginNavigation(event->pointer, NavigationSource::Swipe, cursor->x, cursor->y);
        m_state = State::OverviewSelect;
        m_accumX = 0;
        m_accumY = 0;
        m_output = nullptr;
        return;
      }
      // Outside overview, axis lock chooses strip scrolling or workspace switching.
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
    m_server->notifyInputActivity();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }
    if (pointerScrollActive()) {
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
      if (maxAccum < GesturePhysics::kAxisLock) {
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
      m_workspaceAxis = out->workspaceGroup()->workspaceAxis();
      // Ties resolve to the vertical component, as they always have.
      const bool horizontalTravel = std::abs(m_accumX) > std::abs(m_accumY);
      const bool alongWorkspaceAxis = horizontalTravel == (m_workspaceAxis == WorkspaceAxis::Horizontal);

      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        // Overview opened after this swipe began. Do not start a desktop slide
        // behind it; the next gesture will take ownership of its navigation.
        m_state = State::Idle;
        return;
      }

      if (!alongWorkspaceAxis) {
        // Perpendicular to the workspace axis is the strip axis: scroll it.
        Workspace* ws = out->workspaceGroup()->active();
        ScrollingLayout* scrolling = ws != nullptr ? ws->scrollingLayout() : nullptr;
        if (scrolling == nullptr || scrolling->columns().empty()) {
          m_state = State::Idle;
          return;
        }
        const double scale =
            static_cast<double>(ws->scrollViewportExtent()) / kSwipeViewportPx * m_naturalScrollDirection;
        if (!beginScroll(ws, scale, ScrollSource::Swipe)) {
          m_state = State::Idle;
        }
      } else {
        // Travel along the workspace axis switches workspaces.
        WorkspaceGroup* group = out->workspaceGroup();
        const size_t idx = group->active()->index();
        m_hasPrev = idx > 0;
        m_hasNext = idx + 1 < group->workspaceCount();
        if (!group->slideBegin(m_hasPrev, m_hasNext)) {
          m_state = State::Idle;
          return;
        }
        m_switchGroup = group;
        m_switchTracker.reset();
        m_switchStart = group->slideProgress();
        m_state = State::Switch;
        // The travel that decided the axis is this gesture's first reading, so the slide moves with the fingers from
        // the frame the lock happens instead of after it.
        updateSwitch(m_workspaceAxis == WorkspaceAxis::Horizontal ? event->dx : event->dy, event->time_msec);
      }
      return;
    }

    case State::Scroll: {
      // Natural: fingers move against the strip axis → content follows them. The strip runs
      // unclamped, past its edges included; the release resolves the overscroll.
      updateScroll(m_scrollVertical ? event->dy : event->dx, event->time_msec);
      return;
    }

    case State::Switch: {
      // slideApply needs a live slide. Another workspace change (a switch, a reorder) can settle it under the fingers.
      Output* out = m_server->outputFromWlr(m_server->preferredOutput());
      if (out == nullptr || out->workspaceGroup() != m_switchGroup || !m_switchGroup->slideActive()) {
        m_switchGroup->slideFinish();
        m_switchGroup = nullptr;
        m_output = nullptr;
        m_state = State::Idle;
        return;
      }
      updateSwitch(m_workspaceAxis == WorkspaceAxis::Horizontal ? event->dx : event->dy, event->time_msec);
      return;
    }

    case State::OverviewSelect: {
      Overview* overview = m_server->overview();
      if (overview == nullptr || !overview->interactive()) {
        // The overview started closing under the fingers; the rest of this swipe belongs to nothing.
        if (overview != nullptr) {
          overview->cancelNavigation();
        }
        m_state = State::Idle;
        return;
      }
      overview->updateNavigation(
          -event->dx * m_naturalScrollDirection, -event->dy * m_naturalScrollDirection, event->time_msec
      );
      return;
    }

    case State::Overview: {
      Overview* overview = m_server->overview();
      if (overview == nullptr) {
        m_state = State::Idle;
        return;
      }
      // Swipe up opens, swipe down closes; base is where the gesture started.
      const double base = m_overviewWasOpen ? 1.0 : 0.0;
      m_overviewTracker.push(-event->dy, event->time_msec);
      const double p = GesturePhysics::rubberBand(
          base + m_overviewTracker.pos() / kOverviewDistancePx, 0.0, 1.0, GesturePhysics::kOverscrollLimit
      );
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
    if (pointerScrollActive()) {
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
      m_state = State::Idle;
      return;
    case State::OverviewSelect:
      if (Overview* overview = m_server->overview()) {
        overview->endNavigation(event->cancelled, event->time_msec, NavigationSource::Swipe);
      }
      m_state = State::Idle;
      return;

    case State::Scroll:
      finishScroll(event->cancelled, event->time_msec);
      return;

    case State::Switch:
      finishSwitch(event->cancelled, event->time_msec);
      return;

    case State::Overview:
      finishOverview(event->cancelled, event->time_msec);
      return;

    case State::Idle:
      return;
    }
  }

  // ===== Overview finish (4-finger) =====

  void Gestures::finishOverview(bool cancelled, uint32_t timeMsec) {
    m_state = State::Idle;
    Overview* overview = m_server->overview();
    if (overview == nullptr) {
      return;
    }
    const double base = m_overviewWasOpen ? 1.0 : 0.0;
    bool commitOpen = m_overviewWasOpen;
    double velocity = 0.0;
    if (!cancelled) {
      m_overviewTracker.push(0.0, timeMsec);
      const GesturePhysics::StepRelease release =
          GesturePhysics::release(m_overviewTracker, kOverviewDistancePx, base, 0.0, 1.0);
      commitOpen = release.target == 1;
      velocity = release.velocity;
    }
    overview->gestureEnd(commitOpen, velocity);
  }

  // ===== Scroll finish (Step 5) =====

  void Gestures::finishScroll(bool cancelled, uint32_t timeMsec) {
    Workspace* workspace = m_scrollWorkspace;
    Output* output = m_output;
    m_state = State::Idle;
    m_scrollSource = ScrollSource::None;
    m_scrollWorkspace = nullptr;
    m_output = nullptr;
    if (workspace == nullptr) {
      return;
    }
    ScrollingLayout* scrolling = workspace->scrollingLayout();
    if (scrolling == nullptr) {
      return;
    }
    WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    if (group == nullptr || group->active() != workspace) {
      cancelled = true;
    }
    if (cancelled) {
      scrolling->setScroll(m_scrollStart, m_scrollStartCentered);
      workspace->markArrange(true);
      return;
    }

    // Idle time between the last motion event and the release still bleeds speed, so feed a zero-delta sample before
    // reading the tracker.
    m_scrollTracker.push(0.0, timeMsec);

    const double currentScroll = scrolling->scroll();
    // Where the swipe would coast to a stop under deceleration.
    const double projected = m_scrollStart - m_scrollTracker.projectedEndPos() * m_scrollScale;

    const auto maxScroll = static_cast<double>(scrolling->maxScroll(m_viewportPrimary));
    const auto columnCount = static_cast<int>(scrolling->columns().size());

    // Snapping points are the scroll offsets aligning each column flush with either viewport edge,
    // folded into the strip range. Release settles on the point closest to the projected position,
    // so a flick commits whatever column it would decelerate past, not merely the one under the
    // fingers.
    int best = -1;
    double bestDist = 1e18;
    double bestSnap = currentScroll;
    for (int i = 0; i < columnCount; ++i) {
      const auto x = static_cast<double>(scrolling->columnX(i, m_viewportPrimary));
      const auto w = static_cast<double>(scrolling->columnWidth(i, m_viewportPrimary));
      const double snaps[2] = {
          std::clamp(x, 0.0, maxScroll),
          std::clamp(x + w - static_cast<double>(m_viewportPrimary), 0.0, maxScroll),
      };
      for (const double snap : snaps) {
        const double dist = std::abs(snap - projected);
        if (dist < bestDist) {
          bestDist = dist;
          bestSnap = snap;
          best = i;
        }
      }
    }

    // Focus the outermost column fully visible at the snap in the travel direction: wide viewports
    // show several columns, and the hand expects the farthest one it swiped toward.
    const bool forward = projected >= currentScroll;
    if (forward) {
      for (int i = best + 1; i < columnCount; ++i) {
        const auto x = static_cast<double>(scrolling->columnX(i, m_viewportPrimary));
        const auto w = static_cast<double>(scrolling->columnWidth(i, m_viewportPrimary));
        if (x < bestSnap || x + w > bestSnap + static_cast<double>(m_viewportPrimary)) {
          break;
        }
        best = i;
      }
    } else {
      for (int i = best - 1; i >= 0; --i) {
        const auto x = static_cast<double>(scrolling->columnX(i, m_viewportPrimary));
        const auto w = static_cast<double>(scrolling->columnWidth(i, m_viewportPrimary));
        if (x < bestSnap || x + w > bestSnap + static_cast<double>(m_viewportPrimary)) {
          break;
        }
        best = i;
      }
    }

    if (best < 0) {
      workspace->ensureFocusedVisible();
      workspace->markArrange(true);
      return;
    }

    // Park the strip exactly on the chosen snap first, so the reveal below keeps it there instead of
    // re-deriving an anchor from the overscrolled position.
    scrolling->setScroll(bestSnap);

    View* focused = workspace->focusedView();
    if (focused != nullptr && scrolling->columnOf(focused) == best) {
      // Snap back / settle: target column already focused.
      workspace->ensureFocusedVisible();
      workspace->markArrange(true);
    } else if (!scrolling->columns()[static_cast<size_t>(best)].views.empty()) {
      View* target = scrolling->columns()[static_cast<size_t>(best)].views.front();
      m_server->focusView(target, FocusReason::Directional);
    } else {
      workspace->ensureFocusedVisible();
      workspace->markArrange(true);
    }
  }

  // ===== Switch finish (Step 6) =====

  void Gestures::finishSwitch(bool cancelled, uint32_t timeMsec) {
    if (m_switchGroup == nullptr || !m_switchGroup->slideActive()) {
      m_switchGroup = nullptr;
      m_output = nullptr;
      m_state = State::Idle;
      return;
    }
    int delta = 0;
    double velocity = 0.0;
    if (!cancelled) {
      const GesturePhysics::StepRelease settle = switchSettle(timeMsec);
      delta = settle.target;
      velocity = settle.velocity;
    }
    m_switchGroup->slideSettle(delta, velocity);
    if (delta != 0) {
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
    }
    m_switchGroup = nullptr;
    m_output = nullptr;
    m_state = State::Idle;
  }

  // ===== Pinch handlers (forward unconditionally) =====

  void Gestures::handlePinchBegin(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_begin_event*>(data);
    m_server->notifyInputActivity();
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
    m_server->notifyInputActivity();
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
    m_server->notifyInputActivity();
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
