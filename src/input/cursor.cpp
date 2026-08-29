#include "input/cursor.h"

#include "config/config.h"
#include "core/log.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include "wlr.h"
// clang-format on
#include "wlr/util/edges.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("cursor");
    constexpr double kHotCornerExtent = 8.0;

    // Panels (top/overlay) keep working inside the overview. Wallpaper and bottom-layer widgets are part of the inert
    // desktop behind the filmstrip, so their clicks belong to the overview instead.
    bool overviewPassthroughLayer(const LayerSurface* layer) {
      if (layer == nullptr) {
        return false;
      }
      const uint32_t which = layer->layerSurface()->current.layer;
      return which == ZWLR_LAYER_SHELL_V1_LAYER_TOP || which == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    }

    bool isXdgPopupSurface(wlr_surface* surface) {
      return surface != nullptr && wlr_xdg_popup_try_from_wlr_surface(wlr_surface_get_root_surface(surface)) != nullptr;
    }

    // `[input.touchpad] scroll_factor` scales a touchpad's smooth scroll delta before it reaches the focused client.
    // Reads the live config per event so a successful reload applies on the very next axis; non-touchpads and unset
    // values stay at identity (1.0). Only the continuous delta is scaled, never the discrete value120 notches.
    double touchpadScrollFactor(wlr_pointer* pointer) {
      if (pointer == nullptr || !wlr_input_device_is_libinput(&pointer->base)) {
        return 1.0;
      }
      libinput_device* device = wlr_libinput_get_device_handle(&pointer->base);
      if (device == nullptr || libinput_device_config_tap_get_finger_count(device) == 0) {
        return 1.0;
      }
      return config().input.touchpad.scrollFactor.value_or(1.0);
    }

    bool surfaceLocalCoordinates(wlr_scene* scene, wlr_surface* target, double lx, double ly, double* sx, double* sy) {
      if (target == nullptr) {
        return false;
      }

      struct SurfacePosition {
        wlr_surface* target;
        int x = 0;
        int y = 0;
        bool found = false;
      } position{target};

      wlr_scene_node_for_each_buffer(
          &scene->tree.node,
          [](wlr_scene_buffer* buffer, int x, int y, void* data) {
            auto* position = static_cast<SurfacePosition*>(data);
            if (position->found) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == position->target) {
              position->x = x;
              position->y = y;
              position->found = true;
            }
          },
          &position
      );

      if (!position.found) {
        return false;
      }
      *sx = lx - position.x;
      *sy = ly - position.y;
      return true;
    }

  } // namespace

  Cursor::Cursor(Server& server) : m_server(&server) {
    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_server->outputLayout());
    const Config::Input::Cursor& configured = config().input.cursor;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;
    m_xcursorManager =
        wlr_xcursor_manager_create(m_xcursorTheme.empty() ? nullptr : m_xcursorTheme.c_str(), m_xcursorSize);

    m_motion.notify = onMotion;
    wl_signal_add(&m_cursor->events.motion, &m_motion);
    m_motionAbsolute.notify = onMotionAbsolute;
    wl_signal_add(&m_cursor->events.motion_absolute, &m_motionAbsolute);
    m_button.notify = onButton;
    wl_signal_add(&m_cursor->events.button, &m_button);
    m_axis.notify = onAxis;
    wl_signal_add(&m_cursor->events.axis, &m_axis);
    m_frame.notify = onFrame;
    wl_signal_add(&m_cursor->events.frame, &m_frame);

    m_touchDown.notify = onTouchDown;
    wl_signal_add(&m_cursor->events.touch_down, &m_touchDown);
    m_touchUp.notify = onTouchUp;
    wl_signal_add(&m_cursor->events.touch_up, &m_touchUp);
    m_touchMotion.notify = onTouchMotion;
    wl_signal_add(&m_cursor->events.touch_motion, &m_touchMotion);
    m_touchCancel.notify = onTouchCancel;
    wl_signal_add(&m_cursor->events.touch_cancel, &m_touchCancel);
    m_touchFrame.notify = onTouchFrame;
    wl_signal_add(&m_cursor->events.touch_frame, &m_touchFrame);

    m_tabletToolAxis.notify = onTabletToolAxis;
    wl_signal_add(&m_cursor->events.tablet_tool_axis, &m_tabletToolAxis);
    m_tabletToolProximity.notify = onTabletToolProximity;
    wl_signal_add(&m_cursor->events.tablet_tool_proximity, &m_tabletToolProximity);
    m_tabletToolTip.notify = onTabletToolTip;
    wl_signal_add(&m_cursor->events.tablet_tool_tip, &m_tabletToolTip);
    m_tabletToolButton.notify = onTabletToolButton;
    wl_signal_add(&m_cursor->events.tablet_tool_button, &m_tabletToolButton);

    m_constraintDestroy.link.next = nullptr;
    updateHideTimer();
  }

  Cursor::~Cursor() {
    if (m_hotCornerTimer != nullptr) {
      wl_event_source_remove(m_hotCornerTimer);
    }
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
    }
    if (m_constraintDestroy.link.next != nullptr) {
      wl_list_remove(&m_constraintDestroy.link);
    }
    wl_list_remove(&m_motion.link);
    wl_list_remove(&m_motionAbsolute.link);
    wl_list_remove(&m_button.link);
    wl_list_remove(&m_axis.link);
    wl_list_remove(&m_frame.link);
    wl_list_remove(&m_touchDown.link);
    wl_list_remove(&m_touchUp.link);
    wl_list_remove(&m_touchMotion.link);
    wl_list_remove(&m_touchCancel.link);
    wl_list_remove(&m_touchFrame.link);
    wl_list_remove(&m_tabletToolAxis.link);
    wl_list_remove(&m_tabletToolProximity.link);
    wl_list_remove(&m_tabletToolTip.link);
    wl_list_remove(&m_tabletToolButton.link);
    wlr_cursor_destroy(m_cursor);
    wlr_xcursor_manager_destroy(m_xcursorManager);
  }

  void Cursor::attachInputDevice(wlr_input_device* device) { wlr_cursor_attach_input_device(m_cursor, device); }
  void Cursor::applyConfig() {
    const Config::Input::Cursor& configured = config().input.cursor;
    updateHideTimer();
    cancelHotCorner();
    updateHotCorner();
    if (configured.theme == m_xcursorTheme && configured.size == m_xcursorSize) {
      return;
    }

    wlr_xcursor_manager* manager =
        wlr_xcursor_manager_create(configured.theme.empty() ? nullptr : configured.theme.c_str(), configured.size);
    if (manager == nullptr) {
      return;
    }

    wlr_xcursor_manager* oldManager = m_xcursorManager;
    m_xcursorManager = manager;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;

    if (m_activeXcursorManager == oldManager) {
      setXcursor(m_activeXcursorName.c_str());
    } else if (m_server->seat()->wlr()->pointer_state.focused_surface == nullptr) {
      setXcursor("default");
    }
    wlr_xcursor_manager_destroy(oldManager);
  }

  void Cursor::noteActivity() {
    if (m_cursorHidden) {
      m_cursorHidden = false;
      if (m_compositorOwnsCursor) {
        setXcursor(m_compositorCursorName.c_str());
      } else {
        restoreClientCursor();
      }
    }
    updateHideTimer();
  }

  void Cursor::noteTyping() {
    if (config().input.cursor.hideWhenTyping) {
      hideCursor();
    }
  }

  void Cursor::updateHideTimer() {
    const int timeoutMs = config().input.cursor.hideTimeoutMs;
    if (timeoutMs == 0) {
      if (m_hideTimer != nullptr) {
        wl_event_source_timer_update(m_hideTimer, 0);
      }
      if (m_cursorHidden) {
        m_cursorHidden = false;
        if (m_compositorOwnsCursor) {
          setXcursor(m_compositorCursorName.c_str());
        } else {
          restoreClientCursor();
        }
      }
      return;
    }
    if (m_hideTimer == nullptr) {
      m_hideTimer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onHideTimer, this);
      if (m_hideTimer == nullptr) {
        return;
      }
    }
    if (!m_cursorHidden) {
      wl_event_source_timer_update(m_hideTimer, timeoutMs);
    }
  }

  void Cursor::hideCursor() {
    // Detaching the cursor surface in the middle of an implicit pointer grab
    // disrupts simultaneous mouse and keyboard input in games and other
    // interactive clients. The release restarts the inactivity timer, and a
    // later keypress can hide the cursor normally.
    if (m_cursorHidden || m_server->seat()->wlr()->pointer_state.button_count != 0) {
      return;
    }
    m_cursorHidden = true;
    wlr_cursor_set_surface(m_cursor, nullptr, 0, 0);
  }

  int Cursor::onHideTimer(void* data) {
    static_cast<Cursor*>(data)->hideCursor();
    return 0;
  }

  const Keybind* Cursor::hotCornerAction(size_t* cornerIndex) const {
    const Config::HotCorners& configured = config().hotCorners;
    if (!isPassthrough() || m_server->sessionLocked() || m_server->exclusiveKeyboardLayer() != nullptr) {
      return nullptr;
    }
    const wlr_seat* seat = m_server->seat()->wlr();
    if (seat->drag != nullptr || seat->pointer_state.button_count != 0) {
      return nullptr;
    }

    wlr_output* output = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    wlr_box box{};
    if (output == nullptr) {
      return nullptr;
    }
    if (Output* umbrielOutput = m_server->outputFromWlr(output);
        umbrielOutput != nullptr && umbrielOutput->hasFullscreenView()) {
      return nullptr;
    }
    wlr_output_layout_get_box(m_server->outputLayout(), output, &box);

    // A small logical area lets delayed corners remain reachable beside another output.
    const double horizontalExtent = std::min(kHotCornerExtent, box.width / 2.0);
    const double verticalExtent = std::min(kHotCornerExtent, box.height / 2.0);
    const bool left = m_cursor->x < box.x + horizontalExtent;
    const bool right = m_cursor->x >= box.x + box.width - horizontalExtent;
    const bool top = m_cursor->y < box.y + verticalExtent;
    const bool bottom = m_cursor->y >= box.y + box.height - verticalExtent;
    size_t index = configured.corners.size();
    if (left && top) {
      index = 0;
    } else if (right && top) {
      index = 1;
    } else if (left && bottom) {
      index = 2;
    } else if (right && bottom) {
      index = 3;
    }
    if (index == configured.corners.size()) {
      return nullptr;
    }
    const Config::HotCorner& corner = configured.corners[index];
    if (!corner.enabled || !corner.action) {
      return nullptr;
    }
    if (cornerIndex != nullptr) {
      *cornerIndex = index;
    }
    return &*corner.action;
  }

  void Cursor::cancelHotCorner() {
    m_hotCornerPending = false;
    m_hotCornerTriggered = false;
    m_hotCornerIndex = config().hotCorners.corners.size();
    if (m_hotCornerTimer != nullptr) {
      wl_event_source_timer_update(m_hotCornerTimer, 0);
    }
  }

  void Cursor::updateHotCorner() {
    size_t cornerIndex = 0;
    const Keybind* action = hotCornerAction(&cornerIndex);
    if (action == nullptr) {
      cancelHotCorner();
      return;
    }
    if (cornerIndex != m_hotCornerIndex) {
      cancelHotCorner();
      m_hotCornerIndex = cornerIndex;
    }
    updatePointerOutput();
    if (m_hotCornerTriggered) {
      return;
    }
    const int delayMs = config().hotCorners.corners[cornerIndex].delayMs;
    if (delayMs == 0) {
      m_hotCornerTriggered = true;
      Keybind triggered = *action;
      m_server->executeKeybindAction(triggered);
      return;
    }
    if (m_hotCornerPending) {
      return;
    }
    if (m_hotCornerTimer == nullptr) {
      m_hotCornerTimer =
          wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onHotCornerTimer, this);
      if (m_hotCornerTimer == nullptr) {
        return;
      }
    }
    m_hotCornerPending = true;
    wl_event_source_timer_update(m_hotCornerTimer, delayMs);
  }

  int Cursor::onHotCornerTimer(void* data) {
    auto* cursor = static_cast<Cursor*>(data);
    cursor->m_hotCornerPending = false;
    size_t cornerIndex = 0;
    if (const Keybind* action = cursor->hotCornerAction(&cornerIndex);
        action != nullptr && cornerIndex == cursor->m_hotCornerIndex) {
      cursor->m_hotCornerTriggered = true;
      Keybind triggered = *action;
      cursor->m_server->executeKeybindAction(triggered);
    }
    return 0;
  }

  void Cursor::setCursorSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY) {
    if (!m_cursorHidden) {
      wlr_cursor_set_surface(m_cursor, surface, hotspotX, hotspotY);
    }
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::setXcursor(const char* name) {
    if (!m_cursorHidden) {
      wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, name);
    }
    m_activeXcursorManager = m_xcursorManager;
    m_activeXcursorName = name;
  }

  bool Cursor::isPassthrough() const { return std::holds_alternative<PassthroughGrab>(m_grab); }

  View* Cursor::grabbedView() const {
    if (const auto* grab = std::get_if<FloatingMoveGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<TiledMoveGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<FloatingResizeGrab>(&m_grab)) {
      return grab->view;
    }
    if (const auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
      return grab->view;
    }
    return nullptr;
  }

  bool Cursor::isDraggingView(const View* view) const {
    if (view == nullptr) {
      return false;
    }
    if (const auto* grab = std::get_if<FloatingMoveGrab>(&m_grab)) {
      return grab->view == view;
    }
    if (const auto* grab = std::get_if<TiledMoveGrab>(&m_grab)) {
      return grab->view == view && !grab->pending;
    }
    return false;
  }

  bool Cursor::isResizingWorkspace(const Workspace* workspace) const {
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    return workspace != nullptr && grab != nullptr && grab->workspace == workspace;
  }

  void Cursor::beginMove(View* view, uint32_t button) {
    if (view == nullptr) {
      return;
    }
    if (button == 0) {
      const wlr_seat_pointer_state& pointer = m_server->seat()->wlr()->pointer_state;
      button = pointer.button_count > 0 ? pointer.grab_button : 0;
    }
    if (!isPassthrough()) {
      resetMode();
    }
    bool tiled = view->tiled();
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      view->setFloating(true);
      tiled = false;
    }

    setActiveConstraint(nullptr);
    const double offsetX = m_cursor->x - view->sceneTree()->node.x;
    const double offsetY = m_cursor->y - view->sceneTree()->node.y;
    if (!tiled) {
      m_grab = FloatingMoveGrab{.view = view, .offsetX = offsetX, .offsetY = offsetY};
      m_moveButton = button;
      view->enterDragPresentation();
      updateInteractiveCursor(view);
      return;
    }

    TiledMoveGrab grab{
        .view = view,
        .offsetX = offsetX,
        .offsetY = offsetY,
        .sourceWorkspace = view->workspace(),
        .sourceColumn = -1,
        .sourceWidth = std::nullopt,
        .drop = {},
        .pending = true,
        .startX = m_cursor->x,
        .startY = m_cursor->y,
    };
    grab.sourceColumn = grab.sourceWorkspace != nullptr ? grab.sourceWorkspace->layout().columnOf(view) : -1;
    if (grab.sourceWorkspace != nullptr) {
      grab.sourceWidth = captureDropColumnWidth(*grab.sourceWorkspace, view);
    }
    grab.drop = {
        .workspace = grab.sourceWorkspace,
        .column = std::max(0, grab.sourceColumn),
    };
    m_grab = grab;
    m_moveButton = button;
    updateInteractiveCursor(view);
  }

  void Cursor::beginResize(View* view, uint32_t edges) {
    if (view == nullptr) {
      return;
    }
    if (!isPassthrough()) {
      resetMode();
    }
    bool tiled = view->tiled();
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      view->setFloating(true);
      tiled = false;
    }

    if (tiled) {
      Workspace* workspace = view->workspace();
      if (workspace == nullptr || workspace->group() == nullptr || workspace->group()->output() == nullptr) {
        refreshInteractiveCursor();
        return;
      }
      Layout& layout = workspace->layout();
      const uint32_t resolvedEdges = layout.resolveResizeEdges(view, edges, m_cursor->x, m_cursor->y);
      if (resolvedEdges == 0) {
        if (workspace->focusedView() == view) {
          workspace->ensureFocusedVisible();
          workspace->markArrange(true);
        }
        refreshInteractiveCursor();
        return;
      }
      setActiveConstraint(nullptr);
      if (view->maximizedToEdges()) {
        view->setMaximizedToEdges(false);
      }
      const wlr_box usable = workspace->group()->output()->usableArea();
      std::unique_ptr<ResizeGrab> session = layout.beginResize(view, resolvedEdges, usable);
      if (session == nullptr) {
        refreshInteractiveCursor();
        return;
      }
      if (session->unmaximizeOnBegin()) {
        wlr_xdg_toplevel_set_maximized(view->toplevel(), false);
      }
      m_grab = TiledResizeGrab{
          .view = view,
          .workspace = workspace,
          .startX = m_cursor->x,
          .startY = m_cursor->y,
          .edges = resolvedEdges,
          .session = std::move(session),
      };
      updateInteractiveCursor(view);
      return;
    }
    if (edges == 0) {
      refreshInteractiveCursor();
      return;
    }
    setActiveConstraint(nullptr);
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false);
    }

    const wlr_box& geometry = view->toplevel()->base->geometry;
    const double borderX =
        (view->sceneTree()->node.x + geometry.x) + ((edges & WLR_EDGE_RIGHT) != 0 ? geometry.width : 0);
    const double borderY =
        (view->sceneTree()->node.y + geometry.y) + ((edges & WLR_EDGE_BOTTOM) != 0 ? geometry.height : 0);
    m_grab = FloatingResizeGrab{
        .view = view,
        .offsetX = m_cursor->x - borderX,
        .offsetY = m_cursor->y - borderY,
        .geometryX = geometry.x + view->sceneTree()->node.x,
        .geometryY = geometry.y + view->sceneTree()->node.y,
        .geometryWidth = geometry.width,
        .geometryHeight = geometry.height,
        .edges = edges,
    };
    view->beginFloatingResize(edges);
    updateInteractiveCursor(view);
  }

  void Cursor::warpTo(double lx, double ly) { warpTo(lx, ly, true); }

  void Cursor::warpToPreservingFocus(double lx, double ly) { warpTo(lx, ly, false); }

  void Cursor::warpTo(double lx, double ly, bool allowFocusChange) {
    noteActivity();
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_warp(m_cursor, nullptr, lx, ly);
    // processMotion needs a timestamp and no real input event backs a
    // programmatic warp; derive one from the monotonic clock.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint32_t timeMsec = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    m_server->notifyIdleActivity();
    processMotion(timeMsec, oldX, oldY, allowFocusChange);
  }

  void Cursor::resetMode() {
    m_server->hideInsertHint();
    View* view = grabbedView();
    const bool restoreDragPresentation = std::holds_alternative<FloatingMoveGrab>(m_grab)
        || (std::get_if<TiledMoveGrab>(&m_grab) != nullptr && !std::get<TiledMoveGrab>(m_grab).pending);
    if (std::holds_alternative<FloatingResizeGrab>(m_grab) && view != nullptr) {
      view->finishFloatingResize();
    }
    m_grab = PassthroughGrab{};
    m_moveButton = 0;
    if (restoreDragPresentation && view != nullptr) {
      view->restoreHomePresentation();
    }
    refreshInteractiveCursor();
  }

  void Cursor::cancelStaleTiledResize() {
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    if (grab != nullptr
        && (grab->workspace == nullptr
            || grab->session == nullptr
            || grab->session->ownerLayout() != &grab->workspace->layout())) {
      resetMode();
    }
  }

  void Cursor::onMotion(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_motion);
    self->handleMotion(data);
  }

  void Cursor::onMotionAbsolute(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_motionAbsolute);
    self->handleMotionAbsolute(data);
  }

  void Cursor::onButton(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_button);
    self->handleButton(data);
  }

  void Cursor::onAxis(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_axis);
    self->handleAxis(data);
  }

  void Cursor::onFrame(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_frame);
    self->handleFrame();
  }

  void Cursor::handleMotion(void* data) {
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();

    wlr_relative_pointer_manager_v1_send_relative_motion(
        m_server->relativePointerManager(), m_server->seat()->wlr(), static_cast<uint64_t>(event->time_msec) * 1000,
        event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy
    );

    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      // Workspace switch (etc.) can hide the locking surface without pointer motion.
      // Drop the lock so the cursor can move and become visible again.
      if (!constraintSurfaceActive()) {
        clearConstraint();
      } else {
        return;
      }
    }

    double dx = event->delta_x;
    double dy = event->delta_y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      if (!confineDelta(&dx, &dy)) {
        return;
      }
    }

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleMotionAbsolute(void* data) {
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      if (!constraintSurfaceActive()) {
        clearConstraint();
      } else {
        return;
      }
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      double dx = lx - m_cursor->x;
      double dy = ly - m_cursor->y;
      if (!confineDelta(&dx, &dy)) {
        return;
      }
      wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    } else {
      wlr_cursor_warp_absolute(m_cursor, &event->pointer->base, event->x, event->y);
    }
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleButton(void* data) {
    auto* event = static_cast<wlr_pointer_button_event*>(data);
    processButton(event->time_msec, event->button, event->state);
  }

  void Cursor::processButton(uint32_t timeMsec, uint32_t button, wl_pointer_button_state state) {
    noteActivity();
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      cancelHotCorner();
      m_server->cancelModifierTap();
      // Any pointer press cancels the confirmation without being consumed; the
      // click still reaches whatever it hit.
      if (QuitConfirm* confirm = m_server->quitConfirm(); confirm != nullptr && confirm->visible()) {
        confirm->hide();
      }
    }

    // Config mouse binds win over the overview and the built-in Mod+drag / Mod+resize grabs. Presses consumed here
    // swallow their paired release so clients never see an unmatched release.
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && isPassthrough()) {
      const uint32_t modifiers = m_server->keyboardModifiers();
      const std::optional<Keybind> bound = m_server->handleMouseBind(button, modifiers);
      // Any press dismisses the cheatsheet, as any key press does, except one that just ran a cheatsheet action. Unlike
      // a key press, an unbound press is consumed: the overlay hides whatever sits under the cursor, so the click that
      // dismisses it must not also reach that surface.
      if (Cheatsheet* sheet = m_server->cheatsheet();
          sheet != nullptr && sheet->visible() && !(bound.has_value() && isCheatsheetAction(bound->action))) {
        sheet->hide();
        if (!bound.has_value()) {
          m_swallowedButtons.push_back(button);
          return;
        }
      }
      if (bound.has_value()) {
        m_swallowedButtons.push_back(button);
        return;
      }
    }
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && std::erase(m_swallowedButtons, button) > 0) {
      return;
    }

    // A client data-device drag owns the seat grab. Its initiating release must reach wlroots even when the drag began
    // from a panel over the overview. Otherwise the drag icon and both input grabs remain active indefinitely.
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag != nullptr) {
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      if (seat->drag == nullptr) {
        // The drag grab suppressed normal pointer motion. Re-run hit testing at
        // the unchanged position so the client can restore its hover cursor.
        processMotion(timeMsec, m_cursor->x, m_cursor->y);
      }
      return;
    }

    // An interactive move ends only when its initiating button is released.
    if (m_moveButton != 0 && button != m_moveButton) {
      if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_swallowedButtons.push_back(button);
      }
      return;
    }

    // Overview owns the pointer while it is up: cards are its own hit-test surface and the desktop underneath is inert.
    // Top/overlay layer surfaces (panels) stay fully interactive.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked()) {
      const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overviewPassthroughLayer(layer) && !overview->dragging()) {
        if (surface != nullptr) {
          wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        }
        wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
        // The popup's xdg-shell grab already owns focus. Refocusing its parent layer would end the keyboard grab, whose
        // wlroots cancel handler also ends the pointer grab before the menu receives the matching release.
        if (pressed && !isXdgPopupSurface(surface)) {
          layer->focus();
        }
        return;
      }
      overview->handleButton(button, pressed, m_cursor->x, m_cursor->y);
      return;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (auto* grab = std::get_if<TiledMoveGrab>(&m_grab)) {
        if (grab->pending) {
          resetMode();
        } else {
          finishTileMove();
        }
        return;
      }
      if (std::holds_alternative<FloatingMoveGrab>(m_grab)) {
        finishFloatMove();
        return;
      }
      if (auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
        if (grab->workspace != nullptr) {
          if (grab->workspace->focusedView() == grab->view) {
            grab->workspace->ensureFocusedVisible();
          }
          grab->workspace->markArrange(true);
        }
        resetMode();
        return;
      }
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), timeMsec, button, state);

      // After the final release, refresh pointer focus so it matches the surface actually under the cursor. The
      // implicit-grab guard in processMotion kept focus pinned while buttons were held; realign now so a subsequent
      // press without intervening motion targets the correct surface.
      if (m_server->seat()->wlr()->pointer_state.button_count == 0) {
        double sx2 = 0;
        double sy2 = 0;
        wlr_surface* surf = nullptr;
        m_server->viewAt(m_cursor->x, m_cursor->y, &surf, &sx2, &sy2);
        if (surf != nullptr) {
          wlr_seat_pointer_notify_enter(m_server->seat()->wlr(), surf, sx2, sy2);
        } else {
          wlr_seat_pointer_clear_focus(m_server->seat()->wlr());
        }
      }

      resetMode();
      return;
    }

    // An implicit grab belongs to the surface that received the first press.
    // Route additional presses there until every button has been released.
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (m_server->sessionLocked()) {
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), timeMsec, button, state);
      if (surface != nullptr) {
        if (wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(surface)) {
          if (auto* node = static_cast<LockSurface*>(lockSurface->data)) {
            node->focus();
          }
        }
      }
      return;
    }

    const bool modHeld = (m_server->keyboardModifiers() & m_server->modKey()) != 0;
    if (button == BTN_LEFT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      beginMove(view, button);
      return;
    }
    if (button == BTN_RIGHT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      beginResize(view, view->tiled() ? 0 : floatResizeEdges(view));
      return;
    }

    // Pointer focus must match the surface under the cursor before the button
    // event so wl_data_device drag serial validation succeeds.
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    } else {
      wlr_seat_pointer_clear_focus(seat);
    }

    wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
    if (layer != nullptr) {
      if (!isXdgPopupSurface(surface)) {
        layer->focus();
      }
    } else if (m_server->exclusiveKeyboardLayer() == nullptr) {
      if (view != nullptr) {
        if (!isXdgPopupSurface(surface)) {
          m_server->focusView(view, FocusReason::PointerPress);
        }
      } else {
        wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
        m_server->refocus(m_server->outputFromWlr(wlrOutput));
      }
    }
  }

  void Cursor::handleAxis(void* data) {
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    noteActivity();
    if (event->delta == 0 && event->delta_discrete == 0) {
      m_server->notifyIdleActivity();
    } else {
      m_server->notifyInputActivity();
    }
    m_server->cancelModifierTap();
    cancelHotCorner();

    const uint32_t modifiers = m_server->keyboardModifiers();
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);

    // Determine this event's signed wheel direction from delta and orientation.
    const bool isVertical = event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
    const double rawDelta = event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) : event->delta;
    WheelDirection eventDir;
    if (isVertical) {
      eventDir = rawDelta < 0 ? WheelDirection::Up : WheelDirection::Down;
    } else {
      eventDir = rawDelta < 0 ? WheelDirection::Left : WheelDirection::Right;
    }

    // Unmodified scrolling drives the overview filmstrip instead of the inert desktop under the cursor. Panels
    // (top/overlay) keep their own scrolling, and modifier chords still fall through to the wheel binds below.
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active() && effective == 0) {
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (!overviewPassthroughLayer(layer)) {
        if (!overview->interactive()) {
          return;
        }
        const int axis = isVertical ? 0 : 1;
        m_wheelAccum[axis] +=
            event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
        double& accumulated = m_wheelAccum[axis];
        while (std::abs(accumulated) >= 1.0) {
          overview->handleAxisNotch(isVertical, accumulated, m_cursor->x, m_cursor->y);
          accumulated -= std::copysign(1.0, accumulated);
        }
        return;
      }
    }

    // Arm only when a bind matches this exact direction and modifier set.
    bool armed = false;
    for (const Keybind& bind : config().keybinds) {
      if (bind.wheel != eventDir) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? m_server->modKey() : 0);
      if (effective == expected) {
        armed = true;
        break;
      }
    }

    const int orientation = isVertical ? 0 : 1;
    if (!armed) {
      m_wheelAccum[orientation] = 0;
      const double scale = touchpadScrollFactor(event->pointer);
      wlr_seat_pointer_notify_axis(
          m_server->seat()->wlr(), event->time_msec, event->orientation, event->delta * scale, event->delta_discrete,
          event->source, event->relative_direction
      );
      return;
    }

    // Accumulate normalized notches.
    const double notches =
        event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
    m_wheelAccum[orientation] += notches;

    double& acc = m_wheelAccum[orientation];
    while (std::abs(acc) >= 1.0) {
      WheelDirection direction;
      if (isVertical) {
        direction = acc < 0 ? WheelDirection::Up : WheelDirection::Down;
      } else {
        direction = acc < 0 ? WheelDirection::Left : WheelDirection::Right;
      }
      if (m_server->handleWheelBind(direction, modifiers)) {
        // A wheel action can move the strip without pointer motion. Recompute
        // the active target so release drops where the pointer now points.
        updateDropTarget();
      }
      acc -= std::copysign(1.0, acc);
    }
  }

  void Cursor::handleFrame() { wlr_seat_pointer_notify_frame(m_server->seat()->wlr()); }

  void Cursor::onTouchDown(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchDown);
    self->handleTouchDown(data);
  }

  void Cursor::onTouchUp(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchUp);
    self->handleTouchUp(data);
  }

  void Cursor::onTouchMotion(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchMotion);
    self->handleTouchMotion(data);
  }

  void Cursor::onTouchCancel(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchCancel);
    self->handleTouchCancel(data);
  }

  void Cursor::onTouchFrame(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchFrame);
    self->handleTouchFrame();
  }

  void Cursor::handleTouchDown(void* data) {
    auto* event = static_cast<wlr_touch_down_event*>(data);
    m_server->notifyInputActivity();
    m_server->cancelModifierTap();

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(lx, ly, &surface, &sx, &sy, &layer);

    // A tap in overview activates like a left click; panels keep their touch.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked() && !overviewPassthroughLayer(layer)) {
      if (overview->interactive()) {
        overview->handleButton(BTN_LEFT, true, lx, ly);
        overview->handleButton(BTN_LEFT, false, lx, ly);
      }
      return;
    }

    if (surface != nullptr) {
      // Focus the touched view (click-to-focus equivalent).
      if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
        if (layer != nullptr) {
          if (!isXdgPopupSurface(surface)) {
            layer->focus();
          }
        } else if (view != nullptr && !isXdgPopupSurface(surface)) {
          m_server->focusView(view, FocusReason::PointerPress);
        }
      }
      wlr_seat_touch_notify_down(m_server->seat()->wlr(), surface, event->time_msec, event->touch_id, sx, sy);
    }
  }

  void Cursor::handleTouchUp(void* data) {
    auto* event = static_cast<wlr_touch_up_event*>(data);
    m_server->notifyIdleActivity();
    wlr_seat_touch_notify_up(m_server->seat()->wlr(), event->time_msec, event->touch_id);
  }

  void Cursor::handleTouchMotion(void* data) {
    auto* event = static_cast<wlr_touch_motion_event*>(data);
    m_server->notifyInputActivity();

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_touch_point* point = wlr_seat_touch_get_point(seat, event->touch_id);
    if (point == nullptr) {
      kLog.warn("touch motion id={} has no active seat point", event->touch_id);
      return;
    }
    if (point->surface == nullptr) {
      kLog.warn("touch motion id={} has no target surface", event->touch_id);
      return;
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    if (!surfaceLocalCoordinates(m_server->scene(), point->surface, lx, ly, &sx, &sy)) {
      kLog.warn(
          "touch motion id={} could not map target surface {} at layout=({}, {})", event->touch_id,
          static_cast<void*>(point->surface), lx, ly
      );
      return;
    }
    wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id, sx, sy);
  }

  void Cursor::handleTouchCancel(void* data) {
    auto* event = static_cast<wlr_touch_cancel_event*>(data);
    (void)event;
    m_server->notifyIdleActivity();

    wlr_seat* seat = m_server->seat()->wlr();
    // Find the first client with an active touch point, then cancel outside
    // the iteration: wlr_seat_touch_notify_cancel may mutate the list.
    wlr_seat_client* client = nullptr;
    wlr_touch_point* point;
    wl_list_for_each(point, &seat->touch_state.touch_points, link) {
      if (point->client != nullptr) {
        client = point->client;
        break;
      }
    }
    if (client != nullptr) {
      wlr_seat_touch_notify_cancel(seat, client);
    }
  }

  void Cursor::handleTouchFrame() { wlr_seat_touch_notify_frame(m_server->seat()->wlr()); }

  void Cursor::processMotion(uint32_t timeMsec, double oldX, double oldY, bool allowFocusChange) {
    updateHotCorner();
    // Overview owns motion: cards follow a drag, panels keep passthrough, and
    // the inert desktop underneath never receives enter/motion or hover focus.
    if (Overview* overview = m_server->overview(); overview != nullptr
        && overview->active()
        && !m_server->sessionLocked()
        && m_server->seat()->wlr()->drag == nullptr) {
      overview->handleMotion(m_cursor->x, m_cursor->y);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overview->dragging()) {
        wlr_seat_pointer_clear_focus(seat);
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (overviewPassthroughLayer(layer) && surface != nullptr) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
        return;
      }
      wlr_seat_pointer_clear_focus(seat);
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      return;
    }

    if (std::holds_alternative<FloatingMoveGrab>(m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processMove();
        return;
      }
    }
    if (auto* grab = std::get_if<TiledMoveGrab>(&m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        if (grab->pending) {
          constexpr double kDragThreshold = 10.0;
          const double dx = m_cursor->x - grab->startX;
          const double dy = m_cursor->y - grab->startY;
          if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
            return;
          }
          grab->pending = false;
          if (grab->sourceWorkspace != nullptr) {
            grab->sourceWorkspace->layoutDetach(grab->view);
          }
          grab->view->enterDragPresentation();
        }
        processMove();
        updateDropTarget();
        return;
      }
    }
    if (std::holds_alternative<FloatingResizeGrab>(m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processResize();
        return;
      }
    }
    if (std::holds_alternative<TiledResizeGrab>(m_grab)) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processResizeTile();
        return;
      }
    }

    wlr_seat* seat = m_server->seat()->wlr();
    if (seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      // Keep an implicit grab in the coordinate space established by the press. Re-resolving against the scene
      // would turn compositor-driven window animation into apparent pointer travel and make small clicks look like
      // client drags.
      const double sx = seat->pointer_state.sx + (m_cursor->x - oldX);
      const double sy = seat->pointer_state.sy + (m_cursor->y - oldY);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
      updateConstraintForSurface(seat->pointer_state.focused_surface);
      return;
    }
    updatePointerOutput(allowFocusChange);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (allowFocusChange
        && config().input.focus.followsMouse
        && layer == nullptr
        && view != nullptr
        && view->mapped()) {
      view = hoverFocus(view, &surface, &sx, &sy, &layer, oldX, oldY);
    }

    if (surface != nullptr) {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else if (!m_compositorOwnsCursor) {
      setXcursor("default");
      wlr_seat_pointer_clear_focus(seat);
    } else {
      wlr_seat_pointer_clear_focus(seat);
    }

    // Update the drag icon after seat motion so drop targets are recognized.
    if (seat->drag != nullptr && seat->drag->icon != nullptr) {
      wlr_scene_node_set_position(
          &m_server->dragIconTree()->node, static_cast<int>(m_cursor->x), static_cast<int>(m_cursor->y)
      );
    }

    updateConstraintForSurface(surface);
    updateInteractiveCursor(view);
  }

  void Cursor::updatePointerOutput(bool allowFocusChange) {
    // Crossing outputs updates keyboard / foreign-toplevel focus so clients that follow the
    // focused screen match preferredOutput() / workspace-switch behavior.
    wlr_output* pointerOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (pointerOutput != m_pointerOutput) {
      m_pointerOutput = pointerOutput;
      if (allowFocusChange
          && config().input.focus.followsMouse
          && !m_server->sessionLocked()
          && m_server->exclusiveKeyboardLayer() == nullptr) {
        m_server->refocus(m_server->outputFromWlr(pointerOutput));
      }
    }
  }

  View* Cursor::hoverFocus(
      View* view, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer, double oldX, double oldY
  ) {
    // Only activate when the pointer enters a different window (under old pos != under new pos). Do not warp the
    // pointer with scroll: that re-arms enters during a swipe and cascades across columns.
    if (m_server->seat()->wlr()->drag != nullptr
        || m_server->sessionLocked()
        || *layer != nullptr
        || view == nullptr
        || !view->mapped()) {
      return view;
    }
    wlr_surface* oldSurface = nullptr;
    double oldSx = 0;
    double oldSy = 0;
    View* oldView = m_server->viewAt(oldX, oldY, &oldSurface, &oldSx, &oldSy);
    const bool entered = view != oldView;
    const bool alreadyFocused = view->workspace() != nullptr && view->workspace()->focusedView() == view;
    if (entered && !alreadyFocused) {
      m_server->focusView(view, FocusReason::PointerHover);
      // Scroll may have moved another surface under the cursor; refresh hit-test for
      // pointer notify only. Keyboard focus stays on the entered view until a real enter.
      view = m_server->viewAt(m_cursor->x, m_cursor->y, surface, sx, sy, layer);
    }
    return view;
  }

  void Cursor::onTabletToolAxis(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_tabletToolAxis);
    self->handleTabletToolAxis(data);
  }

  void Cursor::onTabletToolProximity(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_tabletToolProximity);
    self->handleTabletToolProximity(data);
  }

  void Cursor::onTabletToolTip(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_tabletToolTip);
    self->handleTabletToolTip(data);
  }

  void Cursor::onTabletToolButton(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_tabletToolButton);
    self->handleTabletToolButton(data);
  }

  void Cursor::onToolDestroy(wl_listener* listener, void* /*data*/) {
    TabletToolState* watch;
    watch = wl_container_of(listener, watch, destroy);
    Cursor* cursor = watch->cursor;
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->setCursor.link);
    std::erase_if(cursor->m_tools, [watch](const std::unique_ptr<TabletToolState>& entry) {
      return entry.get() == watch;
    });
  }

  void Cursor::onToolSetCursor(wl_listener* listener, void* data) {
    TabletToolState* watch;
    watch = wl_container_of(listener, watch, setCursor);
    auto* event = static_cast<wlr_tablet_v2_event_cursor*>(data);
    if (watch->cursor->compositorOwnsCursor()) {
      return;
    }
    if (watch->v2->focused_surface == nullptr
        || event->seat_client == nullptr
        || wl_resource_get_client(watch->v2->focused_surface->resource) != event->seat_client->client) {
      return;
    }
    watch->cursor->setCursorSurface(event->surface, event->hotspot_x, event->hotspot_y);
  }

  Cursor::TabletToolState* Cursor::toolState(wlr_tablet_tool* tool) {
    for (const auto& entry : m_tools) {
      if (entry->tool == tool) {
        return entry.get();
      }
    }
    auto state = std::make_unique<TabletToolState>();
    state->cursor = this;
    state->tool = tool;
    state->v2 = wlr_tablet_tool_create(m_server->tabletManager(), m_server->seat()->wlr(), tool);
    state->destroy.notify = onToolDestroy;
    wl_signal_add(&tool->events.destroy, &state->destroy);
    state->setCursor.notify = onToolSetCursor;
    wl_signal_add(&state->v2->events.set_cursor, &state->setCursor);
    m_tools.push_back(std::move(state));
    return m_tools.back().get();
  }

  void Cursor::setToolEmulating(TabletToolState* state, bool emulating) {
    if (state->emulating == emulating) {
      return;
    }
    if (emulating) {
      // Native → emulating: end the client's stroke cleanly.
      if (state->v2->focused_surface != nullptr) {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(state->v2);
      }
    } else {
      // Emulating → native: the surface must never receive doubled pointer and
      // tablet input for the same stroke.
      wlr_seat_pointer_clear_focus(m_server->seat()->wlr());
    }
    state->emulating = emulating;
  }

  void
  Cursor::processTabletMotion(uint32_t timeMsec, double oldX, double oldY, TabletToolState* state, wlr_tablet* tablet) {
    wlr_tablet_v2_tablet* v2tablet = m_server->tabletV2FromWlr(tablet);
    wlr_seat* seat = m_server->seat()->wlr();
    // Emulation wholesale: no tablet-v2 handle for this device, or a compositor state (overview, grab, lock, drag) that
    // must see a plain pointer. A stroke already being emulated stays emulated for its whole tip-down so a mid-stroke
    // bind of tablet-v2 cannot split it.
    const bool emulate = v2tablet == nullptr
        || m_server->overview()->active()
        || !isPassthrough()
        || m_server->sessionLocked()
        || seat->drag != nullptr
        || (state->emulating && state->tipDown);
    if (emulate) {
      setToolEmulating(state, true);
      processMotion(timeMsec, oldX, oldY);
      wlr_seat_pointer_notify_frame(seat);
      return;
    }

    // Native implicit grab: tip or a button is held on the focused surface, so motion belongs to that surface's
    // coordinate space even when the cursor leaves it.
    if (state->v2->focused_surface != nullptr && (state->tipDown || wlr_tablet_tool_v2_has_implicit_grab(state->v2))) {
      double sx = 0;
      double sy = 0;
      surfaceLocalCoordinates(m_server->scene(), state->v2->focused_surface, m_cursor->x, m_cursor->y, &sx, &sy);
      wlr_tablet_v2_tablet_tool_notify_motion(state->v2, sx, sy);
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (surface == nullptr || !wlr_surface_accepts_tablet_v2(surface, v2tablet)) {
      setToolEmulating(state, true);
      processMotion(timeMsec, oldX, oldY);
      wlr_seat_pointer_notify_frame(seat);
      return;
    }

    // Native hover.
    setToolEmulating(state, false);
    updatePointerOutput();
    if (config().input.focus.followsMouse && layer == nullptr && view != nullptr && view->mapped()) {
      view = hoverFocus(view, &surface, &sx, &sy, &layer, oldX, oldY);
      if (surface == nullptr) {
        setToolEmulating(state, true);
        processMotion(timeMsec, oldX, oldY);
        wlr_seat_pointer_notify_frame(seat);
        return;
      }
    }
    wlr_tablet_v2_tablet_tool_notify_proximity_in(state->v2, v2tablet, surface);
    wlr_tablet_v2_tablet_tool_notify_motion(state->v2, sx, sy);
  }

  void Cursor::handleTabletToolAxis(void* data) {
    auto* event = static_cast<wlr_tablet_tool_axis_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();
    TabletToolState* state = toolState(event->tool);
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    m_server->remapTablets();
    if (event->tool->type == WLR_TABLET_TOOL_TYPE_MOUSE || event->tool->type == WLR_TABLET_TOOL_TYPE_LENS) {
      wlr_cursor_move(m_cursor, &event->tablet->base, event->dx, event->dy);
    } else {
      if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_X) != 0) {
        state->x = event->x;
      }
      if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) != 0) {
        state->y = event->y;
      }
      wlr_cursor_warp_absolute(m_cursor, &event->tablet->base, state->x, state->y);
    }
    processTabletMotion(event->time_msec, oldX, oldY, state, event->tablet);
    if (state->emulating) {
      return;
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) != 0) {
      wlr_tablet_v2_tablet_tool_notify_pressure(state->v2, event->pressure);
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) != 0) {
      state->tiltX = event->tilt_x;
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) != 0) {
      state->tiltY = event->tilt_y;
    }
    if ((event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) != 0) {
      wlr_tablet_v2_tablet_tool_notify_tilt(state->v2, state->tiltX, state->tiltY);
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) != 0) {
      wlr_tablet_v2_tablet_tool_notify_distance(state->v2, event->distance);
    }
  }

  void Cursor::handleTabletToolProximity(void* data) {
    auto* event = static_cast<wlr_tablet_tool_proximity_event*>(data);
    noteActivity();
    if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
      TabletToolState* state = toolState(event->tool);
      state->x = event->x;
      state->y = event->y;
      const double oldX = m_cursor->x;
      const double oldY = m_cursor->y;
      m_server->remapTablets();
      wlr_cursor_warp_absolute(m_cursor, &event->tablet->base, event->x, event->y);
      processTabletMotion(event->time_msec, oldX, oldY, state, event->tablet);
      return;
    }
    // Proximity out: look up without creating; an unknown tool never produced events.
    TabletToolState* state = nullptr;
    for (const auto& entry : m_tools) {
      if (entry->tool == event->tool) {
        state = entry.get();
        break;
      }
    }
    if (state == nullptr) {
      return;
    }
    // Release a tip that was left down through emulation.
    if (state->emulating && state->tipDown) {
      processButton(event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    }
    if (!state->emulating && state->v2->focused_surface != nullptr) {
      wlr_tablet_v2_tablet_tool_notify_proximity_out(state->v2);
    }
    state->tipDown = false;
  }

  void Cursor::handleTabletToolTip(void* data) {
    auto* event = static_cast<wlr_tablet_tool_tip_event*>(data);
    noteActivity();
    if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
      m_server->cancelModifierTap();
    }
    TabletToolState* state = toolState(event->tool);
    const bool down = event->state == WLR_TABLET_TOOL_TIP_DOWN;
    if (state->emulating) {
      state->tipDown = down;
      processButton(
          event->time_msec, BTN_LEFT, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
      );
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
      return;
    }
    if (down) {
      state->tipDown = true;
      // Click-to-focus parity with processButton.
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (layer != nullptr) {
        if (!isXdgPopupSurface(surface)) {
          layer->focus();
        }
      } else if (m_server->exclusiveKeyboardLayer() == nullptr && view != nullptr && !isXdgPopupSurface(surface)) {
        m_server->focusView(view, FocusReason::PointerPress);
      }
      wlr_tablet_v2_tablet_tool_notify_down(state->v2);
      wlr_tablet_tool_v2_start_implicit_grab(state->v2);
    } else {
      state->tipDown = false;
      wlr_tablet_v2_tablet_tool_notify_up(state->v2);
    }
  }

  void Cursor::handleTabletToolButton(void* data) {
    auto* event = static_cast<wlr_tablet_tool_button_event*>(data);
    noteActivity();
    if (event->state == WLR_BUTTON_PRESSED) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    TabletToolState* state = toolState(event->tool);
    const bool pressed = event->state == WLR_BUTTON_PRESSED;
    if (!state->emulating) {
      wlr_tablet_v2_tablet_tool_notify_button(
          state->v2, event->button,
          pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED
      );
      return;
    }
    uint32_t mapped = event->button;
    if (mapped == BTN_STYLUS) {
      mapped = BTN_RIGHT;
    } else if (mapped == BTN_STYLUS2) {
      mapped = BTN_MIDDLE;
    } else if (mapped == BTN_STYLUS3) {
      mapped = BTN_SIDE;
    }
    processButton(
        event->time_msec, mapped, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
  }

  void Cursor::processMove() {
    View* view = nullptr;
    double offsetX = 0;
    double offsetY = 0;
    if (const auto* grab = std::get_if<FloatingMoveGrab>(&m_grab)) {
      view = grab->view;
      offsetX = grab->offsetX;
      offsetY = grab->offsetY;
    } else if (const auto* grab = std::get_if<TiledMoveGrab>(&m_grab)) {
      view = grab->view;
      offsetX = grab->offsetX;
      offsetY = grab->offsetY;
    }
    if (view == nullptr) {
      resetMode();
      return;
    }
    view->setDragPosition(static_cast<int>(m_cursor->x - offsetX), static_cast<int>(m_cursor->y - offsetY));
    presentGrabbedViewSpanning();
  }

  void Cursor::presentGrabbedViewSpanning() {
    View* view = grabbedView();
    if (view == nullptr) {
      return;
    }
    // A window dragged across a monitor boundary must span both outputs, not be
    // clipped to one. Native per-output rendering draws each half.
    view->setNodeEnabled(true);
    view->resetSurfaceClip();
  }

  void Cursor::updateDropTarget() {
    auto* grab = std::get_if<TiledMoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || grab->pending) {
      return;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (output == nullptr || output->workspaceGroup() == nullptr || output->workspaceGroup()->active() == nullptr) {
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (layer != nullptr) {
      return;
    }

    Workspace* workspace = output->workspaceGroup()->active();
    grab->drop = computeDropTarget(
        *workspace, m_cursor->x, m_cursor->y, grab->view,
        DropTargetOptions{
            .clipHintToUsable = true,
            .reserveScrollingViewportEdges = true,
            .endpointGapsOutsideColumns = false,
        }
    );
    if (grab->drop.hintBox.width > 0 && grab->drop.hintBox.height > 0) {
      m_server->insertHint().show(output, grab->drop.hintBox, config().appearance.cornerRadius);
    } else {
      m_server->hideInsertHint();
    }
    grab->view->raiseToTop();
  }

  void Cursor::finishTileMove() {
    m_server->hideInsertHint();
    auto* grab = std::get_if<TiledMoveGrab>(&m_grab);
    if (grab == nullptr) {
      resetMode();
      return;
    }
    View* view = grab->view;
    Workspace* target = grab->drop.workspace != nullptr ? grab->drop.workspace : grab->sourceWorkspace;
    if (view != nullptr && view->mapped() && target != nullptr) {
      applyDrop(
          *m_server, *view, *target, grab->drop, grab->sourceWidth.has_value() ? &*grab->sourceWidth : nullptr,
          /*animate=*/true
      );
    }
    resetMode();
  }

  void Cursor::finishFloatMove() {
    View* view = grabbedView();
    if (view == nullptr || !view->mapped()) {
      resetMode();
      return;
    }

    const int x = view->sceneTree()->node.x;
    const int y = view->sceneTree()->node.y;
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      scratchpad->finishMove(view, output);
      view->setPosition(x, y);
    } else if (output != nullptr && output->workspaceGroup() != nullptr) {
      if (Workspace* target = output->workspaceGroup()->active(); view->workspace() != target) {
        view->moveToWorkspace(target);
        view->setPosition(x, y);
      }
    }

    resetMode();
    m_server->focusView(view, FocusReason::DragDrop);
  }

  void Cursor::processResize() {
    auto* grab = std::get_if<FloatingResizeGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr) {
      resetMode();
      return;
    }
    const double borderX = m_cursor->x - grab->offsetX;
    const double borderY = m_cursor->y - grab->offsetY;
    int newLeft = grab->geometryX;
    int newRight = grab->geometryX + grab->geometryWidth;
    int newTop = grab->geometryY;
    int newBottom = grab->geometryY + grab->geometryHeight;
    const XdgSizeHints hints = xdgSizeHints(grab->view->toplevel());

    if ((grab->edges & WLR_EDGE_TOP) != 0) {
      newTop = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newTop = newBottom - hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newTop = newBottom - hints.maxHeight;
      }
    } else if ((grab->edges & WLR_EDGE_BOTTOM) != 0) {
      newBottom = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newBottom = newTop + hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newBottom = newTop + hints.maxHeight;
      }
    }

    if ((grab->edges & WLR_EDGE_LEFT) != 0) {
      newLeft = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newLeft = newRight - hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newLeft = newRight - hints.maxWidth;
      }
    } else if ((grab->edges & WLR_EDGE_RIGHT) != 0) {
      newRight = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newRight = newLeft + hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newRight = newLeft + hints.maxWidth;
      }
    }

    grab->view->resizeFloating(newRight - newLeft, newBottom - newTop);
  }

  uint32_t Cursor::floatResizeEdges(View* view) const {
    if (view == nullptr || view->sceneTree() == nullptr) {
      return WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
    }
    const wlr_box& geo = view->toplevel()->base->geometry;
    const int x = view->sceneTree()->node.x + geo.x;
    const int y = view->sceneTree()->node.y + geo.y;
    const wlr_box box{.x = x, .y = y, .width = geo.width, .height = geo.height};
    return resizeEdgesForPoint(box, m_cursor->x, m_cursor->y);
  }

  uint32_t Cursor::hoverResizeEdges(View* view) const {
    if (view == nullptr) {
      return 0;
    }
    // Only advertise resize when the pointer is near the edge that would be grabbed.
    constexpr double kHoverSlop = 28.0;

    if (view->floating()) {
      if (view->sceneTree() == nullptr) {
        return 0;
      }
      const wlr_box& geo = view->toplevel()->base->geometry;
      const double left = view->sceneTree()->node.x + geo.x;
      const double top = view->sceneTree()->node.y + geo.y;
      const double right = left + geo.width;
      const double bottom = top + geo.height;
      const double distLeft = std::abs(m_cursor->x - left);
      const double distRight = std::abs(m_cursor->x - right);
      const double distTop = std::abs(m_cursor->y - top);
      const double distBottom = std::abs(m_cursor->y - bottom);
      const double nearestH = std::min(distLeft, distRight);
      const double nearestV = std::min(distTop, distBottom);
      if (std::min(nearestH, nearestV) > kHoverSlop) {
        return 0;
      }
      return floatResizeEdges(view);
    }

    if (view->workspace() == nullptr) {
      return 0;
    }
    const wlr_box box = view->workspace()->layout().targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    uint32_t edges = view->workspace()->layout().resizeEdgesAt(view, m_cursor->x, m_cursor->y);
    // Advertise an edge that extends past the output from the reachable
    // output boundary, since the pointer cannot approach the real edge.
    wlr_box usable = box;
    if (view->workspace()->group() != nullptr && view->workspace()->group()->output() != nullptr) {
      usable = view->workspace()->group()->output()->usableArea();
    }
    const double left = std::max<double>(box.x, usable.x);
    const double right = std::min<double>(box.x + box.width, usable.x + usable.width);
    const double top = std::max<double>(box.y, usable.y);
    const double bottom = std::min<double>(box.y + box.height, usable.y + usable.height);
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      const double dist = (edges & WLR_EDGE_LEFT) != 0 ? std::abs(m_cursor->x - left) : std::abs(m_cursor->x - right);
      if (dist > kHoverSlop) {
        edges &= ~(WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
      }
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0) {
      const double dist = (edges & WLR_EDGE_TOP) != 0 ? std::abs(m_cursor->y - top) : std::abs(m_cursor->y - bottom);
      if (dist > kHoverSlop) {
        edges &= ~(WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
      }
    }
    return edges;
  }

  void Cursor::setCompositorCursor(const char* name) {
    if (name == nullptr) {
      if (m_compositorOwnsCursor) {
        restoreClientCursor();
      }
      return;
    }
    if (m_compositorOwnsCursor && m_compositorCursorName == name) {
      return;
    }
    m_compositorOwnsCursor = true;
    m_compositorCursorName = name;
    setXcursor(name);
  }

  void Cursor::restoreClientCursor() {
    m_compositorOwnsCursor = false;
    m_compositorCursorName.clear();

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      // Re-enter so the client can restore its pointer shape.
      wlr_seat_pointer_clear_focus(seat);
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    } else {
      setXcursor("default");
      wlr_seat_pointer_clear_focus(seat);
    }
  }

  void Cursor::updateInteractiveCursor(View* under) {
    if (m_server->sessionLocked()) {
      setCompositorCursor(nullptr);
      return;
    }

    uint32_t resizeEdges = 0;
    if (const auto* grab = std::get_if<FloatingResizeGrab>(&m_grab)) {
      resizeEdges = grab->edges;
    } else if (const auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
      resizeEdges = grab->edges;
    }
    if (resizeEdges != 0) {
      const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(resizeEdges));
      setCompositorCursor(name != nullptr ? name : "default");
      return;
    }
    if (std::holds_alternative<FloatingMoveGrab>(m_grab) || std::holds_alternative<TiledMoveGrab>(m_grab)) {
      setCompositorCursor("grabbing");
      return;
    }

    const bool modHeld = (m_server->keyboardModifiers() & m_server->modKey()) != 0;
    if (modHeld && under != nullptr && under->mapped()) {
      const uint32_t edges = hoverResizeEdges(under);
      if (edges != 0) {
        const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(edges));
        setCompositorCursor(name != nullptr ? name : "default");
        return;
      }
      setCompositorCursor("grab");
      return;
    }

    setCompositorCursor(nullptr);
  }

  void Cursor::refreshInteractiveCursor() {
    if (!isPassthrough()) {
      updateInteractiveCursor(grabbedView());
      return;
    }
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    updateInteractiveCursor(view);
  }

  void Cursor::processResizeTile() {
    auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    if (grab == nullptr
        || grab->workspace == nullptr
        || grab->workspace->group() == nullptr
        || grab->workspace->group()->output() == nullptr
        || grab->view == nullptr
        || grab->session == nullptr) {
      resetMode();
      return;
    }
    if (grab->session->ownerLayout() != &grab->workspace->layout()) {
      resetMode();
      return;
    }
    const wlr_box usable = grab->workspace->group()->output()->usableArea();
    grab->session->applyDelta(m_cursor->x - grab->startX, m_cursor->y - grab->startY, usable);
    wlr_xdg_toplevel_set_maximized(grab->view->toplevel(), false);
    grab->workspace->markArrange(false);
  }

} // namespace umbriel
