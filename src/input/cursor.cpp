#include "input/cursor.h"

#include "config/config.h"
#include "core/log.h"
#include "input/event_time.h"
#include "input/gestures.h"
#include "input/seat.h"
#include "input/xcursor_matcher.h"
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
#include "view/size_hints.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <limits>
#include "wlr.h"
// clang-format on
#include "wlr/util/edges.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("cursor");
    constexpr double kHotCornerExtent = 8.0;
    constexpr int kDataDragEdgeScrollTickMs = 16;
    constexpr uint32_t kDataDragEdgeScrollMaxElapsedMs = 50;

    // Panels (top/overlay) keep working inside the overview. Background- and bottom-layer surfaces are part of the
    // inert backdrop behind the filmstrip, so their clicks belong to the overview instead.
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

    // The tab bar slot under a layout point on that output's active workspace. A bar is compositor-drawn, so the scene
    // hit test never reports it: what the hit test did find wins when it sits above the bar. Windows, their popups, X11
    // menus, and top or overlay panels do; background and bottom layer surfaces, a wallpaper, lie below it.
    std::optional<TabHit> tabBarHitAt(
        Server& server, double lx, double ly, const wlr_surface* hitSurface, const View* hitView,
        const LayerSurface* hitLayer
    ) {
      const bool covered =
          hitView != nullptr || (hitLayer != nullptr ? overviewPassthroughLayer(hitLayer) : hitSurface != nullptr);
      if (covered) {
        return std::nullopt;
      }
      Output* output = server.outputFromWlr(wlr_output_layout_output_at(server.outputLayout(), lx, ly));
      WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
      Workspace* workspace = group != nullptr ? group->active() : nullptr;
      return workspace != nullptr ? workspace->tabs().tabAt(lx, ly) : std::nullopt;
    }

    // `[input.touchpad] scroll_factor` (overridden per direction by `horizontal`/`vertical`) scales a touchpad's smooth
    // scroll delta, never the discrete value120 notches. Read per event so a reload applies on the next axis;
    // non-touchpads and unset values stay at 1.0.
    double touchpadScrollFactor(wlr_pointer* pointer, bool vertical) {
      if (pointer == nullptr || !wlr_input_device_is_libinput(&pointer->base)) {
        return 1.0;
      }
      libinput_device* device = wlr_libinput_get_device_handle(&pointer->base);
      if (device == nullptr || libinput_device_config_tap_get_finger_count(device) == 0) {
        return 1.0;
      }
      const std::optional<Config::Input::Touchpad::ScrollFactor>& factor = config().input.touchpad.scrollFactor;
      return factor ? (vertical ? factor->vertical : factor->horizontal).value_or(1.0) : 1.0;
    }

    // `scale` is the surface-local units per layout unit in `target`.
    bool surfaceLocalCoordinates(
        wlr_scene* scene, wlr_surface* target, double scale, double lx, double ly, double* sx, double* sy
    ) {
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
      *sx = (lx - position.x) * scale;
      *sy = (ly - position.y) * scale;
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
    m_clientCursorOwnerDestroy.notify = onClientCursorOwnerDestroy;
    m_clientCursorOwnerDestroy.link.next = nullptr;
    m_clientCursorDestroy.notify = onClientCursorDestroy;
    m_clientCursorDestroy.link.next = nullptr;
    m_clientCursorClientCommit.notify = onClientCursorClientCommit;
    m_clientCursorClientCommit.link.next = nullptr;
    m_clientCursorCommit.notify = onClientCursorCommit;
    m_clientCursorCommit.link.next = nullptr;
    updateHideTimer();
  }

  Cursor::~Cursor() {
    if (m_dataDragEdgeScrollTimer != nullptr) {
      wl_event_source_remove(m_dataDragEdgeScrollTimer);
    }
    if (m_hotCornerTimer != nullptr) {
      wl_event_source_remove(m_hotCornerTimer);
    }
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
    }
    if (m_constraintDestroy.link.next != nullptr) {
      wl_list_remove(&m_constraintDestroy.link);
    }
    if (m_clientCursorOwnerDestroy.link.next != nullptr) {
      wl_list_remove(&m_clientCursorOwnerDestroy.link);
    }
    if (m_clientCursorDestroy.link.next != nullptr) {
      wl_list_remove(&m_clientCursorDestroy.link);
    }
    if (m_clientCursorClientCommit.link.next != nullptr) {
      wl_list_remove(&m_clientCursorClientCommit.link);
    }
    if (m_clientCursorCommit.link.next != nullptr) {
      wl_list_remove(&m_clientCursorCommit.link);
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
  void Cursor::resetWheelAccumulation() { m_wheelAccum[0] = m_wheelAccum[1] = 0; }

  void Cursor::handleDataDragStarted() { updateDataDragEdgeScroll(); }
  void Cursor::handleDataDragEnded() { cancelDataDragEdgeScroll(); }

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

    const bool hasXwaylandClientCursor = m_clientCursorFromXwayland && m_clientCursorSurface != nullptr;
    if (hasXwaylandClientCursor) {
      if (const std::optional<std::string> match = matchXwaylandCursor()) {
        m_clientCursorShape = *match;
      } else {
        m_clientCursorShape.clear();
      }
    }

    if (m_compositorOwnsCursor) {
      setXcursor(m_compositorCursorName.c_str());
    } else if (hasXwaylandClientCursor) {
      applyClientCursor();
    } else if (m_activeXcursorManager == oldManager) {
      setXcursor(m_activeXcursorName.c_str());
    } else if (m_server->seat()->wlr()->pointer_state.focused_surface == nullptr) {
      setXcursor("default");
    }
    if (hasXwaylandClientCursor && !m_clientCursorShape.empty()) {
      finishClientCursorFrame();
    }
    // Xwayland's default cursor is a buffer of the manager's image, so it moves over before the old manager dies.
    if (Xwayland* xwayland = m_server->xwayland()) {
      xwayland->applyCursor(manager);
    }
    wlr_xcursor_manager_destroy(oldManager);
  }

  void Cursor::noteActivity() {
    if (m_cursorHidden) {
      m_cursorHidden = false;
      forwardEffectPointer();
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
        forwardEffectPointer();
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
    forwardEffectPointer();
    wlr_cursor_set_surface(m_cursor, nullptr, 0, 0);
  }

  void Cursor::forwardEffectPointer() const {
    if (m_server->effects().cursorEffectActive()) {
      m_server->effects().pointerMoved(m_cursor->x, m_cursor->y, !m_cursorHidden);
    }
  }

  void Cursor::handleOutputLayoutChange() const {
    m_server->effects().forgetPointerOutput();
    forwardEffectPointer();
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
    const Output* umbrielOutput = m_server->outputFromWlr(output);
    View* focused = View::fromSurface(seat->keyboard_state.focused_surface);
    if (focused != nullptr
        && focused->mapped()
        && focused->onActiveWorkspace()
        && focused->currentOutput() == umbrielOutput
        && (focused->layoutFullscreen() || focused->currentFullscreen())) {
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

  bool Cursor::tiledMoveDragActive() const {
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && grab->view != nullptr && !grab->pending && grab->target == DragTarget::Tiled;
  }

  Workspace* Cursor::dataDragEdgeScrollTarget(double* speed) const {
    *speed = 0;
    const Config::Input::DragEdgeScroll& edge = config().input.dragEdgeScroll;
    if (!edge.enabled
        || m_server->sessionLocked()
        || (m_server->seat()->wlr()->drag == nullptr && !tiledMoveDragActive())
        || (m_server->overview() != nullptr && m_server->overview()->active())) {
      return nullptr;
    }

    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    Workspace* workspace = group != nullptr ? group->active() : nullptr;
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr || scrolling->columns().empty()) {
      return nullptr;
    }

    const wlr_box area = workspace->usableArea();
    const bool vertical = workspace->scrollingVertical();
    const double origin = vertical ? area.y : area.x;
    const double extent = vertical ? area.height : area.width;
    if (extent <= 0) {
      return nullptr;
    }
    const double trigger = std::min(static_cast<double>(edge.triggerZone), extent / 2.0);
    const double position = vertical ? m_cursor->y : m_cursor->x;
    if (position < origin + trigger) {
      *speed = -edge.maxSpeed * std::clamp((origin + trigger - position) / trigger, 0.0, 1.0);
    } else if (position > origin + extent - trigger) {
      *speed = edge.maxSpeed * std::clamp((position - (origin + extent - trigger)) / trigger, 0.0, 1.0);
    }
    if (*speed == 0) {
      return nullptr;
    }

    const double oldScroll = scrolling->scroll();
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    if (maximum <= 0) {
      return nullptr;
    }
    if ((*speed < 0 && oldScroll <= 0) || (*speed > 0 && oldScroll >= maximum)) {
      return nullptr;
    }
    return workspace;
  }

  void Cursor::updateDataDragEdgeScroll() {
    const Config::Input::DragEdgeScroll& edge = config().input.dragEdgeScroll;
    double speed = 0;
    Workspace* workspace = dataDragEdgeScrollTarget(&speed);
    const int direction = (speed > 0) - (speed < 0);
    const int previousDirection = (m_dataDragEdgeScrollSpeed > 0) - (m_dataDragEdgeScrollSpeed < 0);
    if (workspace == nullptr) {
      cancelDataDragEdgeScroll();
      return;
    }
    if (workspace == m_dataDragEdgeScrollWorkspace && direction == previousDirection) {
      m_dataDragEdgeScrollSpeed = speed;
      return;
    }
    if (m_dataDragEdgeScrollTimer == nullptr) {
      m_dataDragEdgeScrollTimer =
          wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onDataDragEdgeScrollTimer, this);
      if (m_dataDragEdgeScrollTimer == nullptr) {
        return;
      }
    }
    m_dataDragEdgeScrollWorkspace = workspace;
    m_dataDragEdgeScrollSpeed = speed;
    m_dataDragEdgeScrollLastMsec = 0;
    m_dataDragEdgeScrollPending = true;
    if (edge.delayMs > 0) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, edge.delayMs);
    } else {
      handleDataDragEdgeScrollTimer();
    }
  }

  void Cursor::cancelDataDragEdgeScroll() {
    if (m_dataDragEdgeScrollTimer != nullptr) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, 0);
    }
    m_dataDragEdgeScrollWorkspace = nullptr;
    m_dataDragEdgeScrollSpeed = 0;
    m_dataDragEdgeScrollLastMsec = 0;
    m_dataDragEdgeScrollPending = false;
  }

  int Cursor::onDataDragEdgeScrollTimer(void* data) {
    return static_cast<Cursor*>(data)->handleDataDragEdgeScrollTimer();
  }

  int Cursor::handleDataDragEdgeScrollTimer() {
    double speed = 0;
    Workspace* workspace = dataDragEdgeScrollTarget(&speed);
    const int direction = (speed > 0) - (speed < 0);
    const int activeDirection = (m_dataDragEdgeScrollSpeed > 0) - (m_dataDragEdgeScrollSpeed < 0);
    if (workspace == nullptr) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    if (workspace != m_dataDragEdgeScrollWorkspace || direction != activeDirection) {
      updateDataDragEdgeScroll();
      return 0;
    }
    m_dataDragEdgeScrollSpeed = speed;

    const uint32_t now = monotonicMsec();
    if (m_dataDragEdgeScrollPending) {
      m_dataDragEdgeScrollPending = false;
      m_dataDragEdgeScrollLastMsec = now;
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
      return 0;
    }

    const uint32_t elapsed = std::min(now - m_dataDragEdgeScrollLastMsec, kDataDragEdgeScrollMaxElapsedMs);
    m_dataDragEdgeScrollLastMsec = now;
    if (elapsed == 0) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
      return 0;
    }
    ScrollingLayout* scrolling = workspace->scrollingLayout();
    if (scrolling == nullptr) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    const double oldScroll = scrolling->scroll();
    const double nextScroll = std::clamp(oldScroll + speed * static_cast<double>(elapsed) / 1000.0, 0.0, maximum);
    if (nextScroll == oldScroll) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    scrolling->setScroll(nextScroll);
    workspace->markArrange(false);
    wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
    return 0;
  }

  void Cursor::setCursorSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY, wl_client* owner) {
    const bool sameSurface = m_clientCursorKnown && surface != nullptr && surface == m_clientCursorSurface;
    const bool sameHotspot = sameSurface && hotspotX == m_clientCursorHotspotX && hotspotY == m_clientCursorHotspotY;
    const bool keepPromotion = sameHotspot && m_clientCursorFromXwayland && !m_clientCursorShape.empty();
    if (!sameSurface) {
      forgetClientCursor();
      m_clientCursorKnown = true;
      m_clientCursorOwner = owner;
      if (owner != nullptr) {
        wl_client_add_destroy_listener(owner, &m_clientCursorOwnerDestroy);
      }
      m_clientCursorSurface = surface;
      if (surface != nullptr) {
        Xwayland* xwayland = m_server->xwayland();
        m_clientCursorFromXwayland = xwayland != nullptr && xwayland->ownsSurface(surface);
        wl_signal_add(&surface->events.destroy, &m_clientCursorDestroy);
        if (m_clientCursorFromXwayland) {
          wl_signal_add(&surface->events.client_commit, &m_clientCursorClientCommit);
          wl_signal_add(&surface->events.commit, &m_clientCursorCommit);
          if (surface->buffer != nullptr) {
            captureXwaylandCursorImage(&surface->buffer->base);
          }
        }
      }
    }
    m_clientCursorHotspotX = hotspotX;
    m_clientCursorHotspotY = hotspotY;
    // Xwayland may commit a static cursor before assigning it to the pointer.
    // Inspect that already-current buffer now; later animation and cursor
    // changes arrive through m_clientCursorCommit.
    if ((!sameSurface || !sameHotspot) && m_clientCursorFromXwayland && !m_clientCursorPixels.empty()) {
      handleClientCursorCommit();
      if (m_compositorOwnsCursor || !m_clientCursorShape.empty()) {
        return;
      }
    } else if (!sameHotspot) {
      m_clientCursorShape.clear();
    }
    if (m_compositorOwnsCursor) {
      // Replayed when the override ends.
      return;
    }
    if (keepPromotion) {
      setXcursor(m_clientCursorShape.c_str());
      return;
    }
    if (!m_cursorHidden) {
      wlr_cursor_set_surface(m_cursor, surface, hotspotX, hotspotY);
    }
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::setCursorShape(const char* name, wl_client* owner) {
    forgetClientCursor();
    m_clientCursorKnown = true;
    m_clientCursorOwner = owner;
    if (owner != nullptr) {
      wl_client_add_destroy_listener(owner, &m_clientCursorOwnerDestroy);
    }
    m_clientCursorShape = name;
    if (m_compositorOwnsCursor) {
      return;
    }
    setXcursor(name);
  }

  void Cursor::applyClientCursor() {
    if (!m_clientCursorKnown) {
      setXcursor("default");
      return;
    }
    if (!m_clientCursorShape.empty()) {
      setXcursor(m_clientCursorShape.c_str());
      return;
    }
    if (!m_cursorHidden) {
      wlr_cursor_set_surface(m_cursor, m_clientCursorSurface, m_clientCursorHotspotX, m_clientCursorHotspotY);
    }
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::captureXwaylandCursorImage(wlr_buffer* buffer) {
    m_clientCursorPixels.clear();
    m_clientCursorImageWidth = 0;
    m_clientCursorImageHeight = 0;
    if (buffer == nullptr || buffer->width <= 0 || buffer->height <= 0) {
      return;
    }

    void* pixels = nullptr;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) {
      return;
    }

    constexpr size_t kBytesPerPixel = sizeof(uint32_t);
    const auto width = static_cast<uint32_t>(buffer->width);
    const auto height = static_cast<uint32_t>(buffer->height);
    const bool widthOverflows = width > std::numeric_limits<size_t>::max() / kBytesPerPixel;
    const size_t rowBytes = widthOverflows ? 0 : static_cast<size_t>(width) * kBytesPerPixel;
    const bool sizeOverflows = rowBytes == 0 || height > std::numeric_limits<size_t>::max() / rowBytes;
    if (format == DRM_FORMAT_ARGB8888 && pixels != nullptr && stride >= rowBytes && !sizeOverflows) {
      m_clientCursorPixels.resize(rowBytes * height);
      const auto* source = static_cast<const uint8_t*>(pixels);
      for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            m_clientCursorPixels.data() + static_cast<size_t>(y) * rowBytes, source + static_cast<size_t>(y) * stride,
            rowBytes
        );
      }
      m_clientCursorImageWidth = width;
      m_clientCursorImageHeight = height;
    }
    wlr_buffer_end_data_ptr_access(buffer);
  }

  std::optional<std::string> Cursor::matchXwaylandCursor() const {
    if (!m_clientCursorFromXwayland
        || m_clientCursorSurface == nullptr
        || m_clientCursorSurface->current.scale != 1
        || m_clientCursorSurface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL
        || m_clientCursorSurface->current.viewport.has_src
        || m_clientCursorSurface->current.viewport.has_dst
        || m_clientCursorPixels.empty()
        || m_clientCursorSurface->current.width != static_cast<int>(m_clientCursorImageWidth)
        || m_clientCursorSurface->current.height != static_cast<int>(m_clientCursorImageHeight)
        || m_clientCursorHotspotX < 0
        || m_clientCursorHotspotY < 0) {
      return std::nullopt;
    }

    return matchXcursorManagerImage(
        m_xcursorManager,
        {
            .width = m_clientCursorImageWidth,
            .height = m_clientCursorImageHeight,
            .hotspotX = static_cast<uint32_t>(m_clientCursorHotspotX),
            .hotspotY = static_cast<uint32_t>(m_clientCursorHotspotY),
            .pixels = m_clientCursorPixels.data(),
            .stride = static_cast<size_t>(m_clientCursorImageWidth) * sizeof(uint32_t),
        },
        m_clientCursorShape
    );
  }

  void Cursor::finishClientCursorFrame() const {
    if (m_clientCursorSurface == nullptr) {
      return;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_send_frame_done(m_clientCursorSurface, &now);
  }

  void Cursor::handleClientCursorCommit() {
    const bool wasPromoted = !m_clientCursorShape.empty();
    if (const std::optional<std::string> match = matchXwaylandCursor()) {
      m_clientCursorShape = *match;
      if (!m_compositorOwnsCursor) {
        setXcursor(m_clientCursorShape.c_str());
      }
      // Xwayland waits for this callback before committing an animated cursor's
      // next frame. wlroots no longer owns the source surface after promotion.
      finishClientCursorFrame();
      return;
    }

    m_clientCursorShape.clear();
    if (!m_compositorOwnsCursor) {
      if (wasPromoted && !m_cursorHidden) {
        wlr_cursor_set_surface(m_cursor, m_clientCursorSurface, m_clientCursorHotspotX, m_clientCursorHotspotY);
      }
      m_activeXcursorManager = nullptr;
      m_activeXcursorName.clear();
    }
    // wlroots paces a visible raw surface. When Umbriel has detached it for a
    // compositor override or cursor hiding, complete Xwayland's callback here.
    if (m_compositorOwnsCursor || m_cursorHidden) {
      finishClientCursorFrame();
    }
  }

  void Cursor::forgetClientCursor() {
    if (m_clientCursorOwnerDestroy.link.next != nullptr) {
      wl_list_remove(&m_clientCursorOwnerDestroy.link);
      m_clientCursorOwnerDestroy.link.next = nullptr;
    }
    if (m_clientCursorDestroy.link.next != nullptr) {
      wl_list_remove(&m_clientCursorDestroy.link);
      m_clientCursorDestroy.link.next = nullptr;
    }
    if (m_clientCursorClientCommit.link.next != nullptr) {
      wl_list_remove(&m_clientCursorClientCommit.link);
      m_clientCursorClientCommit.link.next = nullptr;
    }
    if (m_clientCursorCommit.link.next != nullptr) {
      wl_list_remove(&m_clientCursorCommit.link);
      m_clientCursorCommit.link.next = nullptr;
    }
    m_clientCursorKnown = false;
    m_clientCursorOwner = nullptr;
    m_clientCursorSurface = nullptr;
    m_clientCursorHotspotX = 0;
    m_clientCursorHotspotY = 0;
    m_clientCursorShape.clear();
    m_clientCursorFromXwayland = false;
    m_clientCursorPixels.clear();
    m_clientCursorImageWidth = 0;
    m_clientCursorImageHeight = 0;
  }

  void Cursor::onClientCursorOwnerDestroy(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_clientCursorOwnerDestroy);
    self->forgetClientCursor();
    if (!self->m_compositorOwnsCursor) {
      self->setXcursor("default");
    }
  }

  void Cursor::onClientCursorDestroy(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_clientCursorDestroy);
    const bool promoted = self->m_clientCursorSurface != nullptr && !self->m_clientCursorShape.empty();
    self->forgetClientCursor();
    if (promoted && !self->m_compositorOwnsCursor) {
      self->setXcursor("default");
    }
  }

  void Cursor::onClientCursorClientCommit(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_clientCursorClientCommit);
    wlr_surface_state& pending = self->m_clientCursorSurface->pending;
    if ((pending.committed & WLR_SURFACE_STATE_BUFFER) != 0) {
      self->captureXwaylandCursorImage(pending.buffer);
    }
  }

  void Cursor::onClientCursorCommit(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_clientCursorCommit);
    self->m_clientCursorHotspotX -= self->m_clientCursorSurface->current.dx;
    self->m_clientCursorHotspotY -= self->m_clientCursorSurface->current.dy;
    self->handleClientCursorCommit();
  }

  void Cursor::notePointerFocusChange(wlr_surface* newSurface) {
    if (newSurface != nullptr
        && m_clientCursorOwner != nullptr
        && wl_resource_get_client(newSurface->resource) == m_clientCursorOwner) {
      return;
    }
    forgetClientCursor();
    if (newSurface == nullptr && !m_compositorOwnsCursor) {
      setXcursor("default");
    }
  }

  void Cursor::setXcursor(const char* name) {
    if (!m_cursorHidden) {
      wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, name);
    }
    m_activeXcursorManager = m_xcursorManager;
    m_activeXcursorName = name;
  }

#ifdef UMBRIEL_TEST_IPC
  std::string Cursor::clientCursorSourceForTest() const {
    if (!m_clientCursorKnown) {
      return "none";
    }
    if (!m_clientCursorShape.empty()) {
      return "xcursor";
    }
    return m_clientCursorSurface != nullptr ? "surface" : "hidden";
  }

  std::optional<Cursor::RenderedCursorStateForTest> Cursor::renderedCursorStateForTest() const {
    wlr_output* output = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (output == nullptr) {
      return std::nullopt;
    }

    wlr_output_cursor* outputCursor = nullptr;
    wl_list_for_each(outputCursor, &output->cursors, link) {
      if (outputCursor->enabled && outputCursor->visible && outputCursor->texture != nullptr) {
        return RenderedCursorStateForTest{
            .textureWidth = outputCursor->texture->width,
            .textureHeight = outputCursor->texture->height,
            .renderWidth = outputCursor->width,
            .renderHeight = outputCursor->height,
        };
      }
    }
    return std::nullopt;
  }
#endif

  bool Cursor::isPassthrough() const { return std::holds_alternative<PassthroughGrab>(m_grab); }

  View* Cursor::grabbedView() const {
    if (const auto* grab = std::get_if<MoveGrab>(&m_grab)) {
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
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && grab->view == view && !grab->pending;
  }

  bool Cursor::isDraggingIntoLayout() const {
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    return grab != nullptr && !grab->pending && grab->target == DragTarget::Tiled;
  }

  bool Cursor::isResizingWorkspace(const Workspace* workspace) const {
    const auto* grab = std::get_if<TiledResizeGrab>(&m_grab);
    return workspace != nullptr && grab != nullptr && grab->workspace == workspace;
  }

  bool Cursor::beginMove(View* view, uint32_t button, bool deferred) {
    if (view == nullptr || !view->mapped() || button == 0) {
      return false;
    }
    // A modal dialog has no place of its own: dragging it drags the window it is attached to, tiled or floating, and
    // the dialog stays centered on it.
    view = view->attachedRoot();
    if (!isPassthrough()) {
      resetMode();
    }
    bool tiled = view->tiled();
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      view->restoreMaximizedForMove();
      view->setFloating(true);
      tiled = false;
    }

    setActiveConstraint(nullptr);
    const bool pinned = !tiled && view->pinned();
    // A pinned window is floating, but it remembers whether it was tiled when
    // it got pinned, and that is where unpinning it puts it back.
    const bool tiledUnderneath = tiled || (pinned && view->restoresTiledOnUnpin());
    MoveGrab grab{
        .view = view,
        .offsetX = m_cursor->x - view->sceneTree()->node.x,
        .offsetY = m_cursor->y - view->sceneTree()->node.y,
        .target = tiled ? DragTarget::Tiled : (pinned ? DragTarget::Pinned : DragTarget::Floating),
        .unpinned = tiledUnderneath ? DragTarget::Tiled : DragTarget::Floating,
        .sourceWorkspace = tiled ? view->workspace() : nullptr,
        .sourceColumn = -1,
        .sourceWidth = std::nullopt,
        .drop = {},
        .pending = tiled || deferred,
        .startX = m_cursor->x,
        .startY = m_cursor->y,
        .lastX = m_cursor->x,
        .lastY = m_cursor->y,
    };
    if (grab.sourceWorkspace != nullptr) {
      grab.sourceColumn = grab.sourceWorkspace->layout().columnOf(view);
      grab.sourceWidth = captureDropColumnWidth(*grab.sourceWorkspace, view);
      grab.drop = {
          .workspace = grab.sourceWorkspace,
          .column = std::max(0, grab.sourceColumn),
      };
    }
    m_grab = grab;
    m_grabButton = button;
    if (!grab.pending) {
      view->enterDragPresentation();
      std::get<MoveGrab>(m_grab).physics = view->beginDragPhysics(grab.offsetX, grab.offsetY);
    }
    updateInteractiveCursor(view);
    return true;
  }

  bool Cursor::beginResize(View* view, uint32_t edges, uint32_t button) {
    if (view == nullptr || !view->mapped() || button == 0) {
      return false;
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
        return false;
      }
      Layout& layout = workspace->layout();
      uint32_t resolvedEdges = 0;
      if (edges != 0) {
        resolvedEdges = layout.resolveResizeEdges(view, edges, m_cursor->x, m_cursor->y);
      } else {
        const wlr_box presented = workspace->presentedTiledBox(view);
        const wlr_box visible = workspace->usableArea();
        wlr_box reachable{};
        if (wlr_box_intersection(&reachable, &presented, &visible)) {
          resolvedEdges = layout.sanitizeResizeEdges(view, resizeEdgesForPoint(reachable, m_cursor->x, m_cursor->y));
        }
      }
      if (resolvedEdges == 0) {
        if (workspace->focusedView() == view) {
          workspace->ensureFocusedVisible();
          workspace->markArrange(true);
        }
        refreshInteractiveCursor();
        return false;
      }
      setActiveConstraint(nullptr);
      if (view->maximizedToEdges()) {
        view->setMaximizedToEdges(false, false);
      }
      const wlr_box usable = workspace->tiledArea();
      std::unique_ptr<ResizeGrab> session = layout.beginResize(view, resolvedEdges, usable);
      if (session == nullptr) {
        refreshInteractiveCursor();
        return false;
      }
      if (session->unmaximizeOnBegin()) {
        view->setMaximizedState(false);
      }
      m_grab = TiledResizeGrab{
          .view = view,
          .workspace = workspace,
          .startX = m_cursor->x,
          .startY = m_cursor->y,
          .edges = resolvedEdges,
          .session = std::move(session),
      };
      m_grabButton = button;
      updateInteractiveCursor(view);
      return true;
    }
    if (edges == 0) {
      refreshInteractiveCursor();
      return false;
    }
    setActiveConstraint(nullptr);
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false, false);
    }

    const wlr_box& geometry = view->geometryBox();
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
    m_grabButton = button;
    view->beginFloatingResize(edges);
    updateInteractiveCursor(view);
    return true;
  }

  std::optional<uint32_t>
  Cursor::clientPointerGrabButton(const View* view, wlr_seat_client* seatClient, uint32_t serial) const {
    if (view == nullptr || !isPassthrough()) {
      return std::nullopt;
    }
    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* focused = seat->pointer_state.focused_surface;
    if (seat->drag != nullptr
        || wlr_seat_pointer_has_grab(seat)
        || focused == nullptr
        || wlr_surface_get_root_surface(focused) != view->rootSurface()
        || seat->pointer_state.button_count != 1) {
      return std::nullopt;
    }
    // X11 requests carry no seat client or serial; the pressed pointer on the window is their only credential.
    if (seatClient == nullptr) {
      return view->xwayland() ? std::optional(seat->pointer_state.grab_button) : std::nullopt;
    }
    if (seatClient->seat != seat || !wlr_seat_validate_pointer_grab_serial(seat, focused, serial)) {
      return std::nullopt;
    }
    return seat->pointer_state.grab_button;
  }

  void Cursor::beginClientMove(View* view, wlr_seat_client* seatClient, uint32_t serial) {
    const std::optional<uint32_t> button = clientPointerGrabButton(view, seatClient, serial);
    if (!button.has_value() || *button == 0 || !beginMove(view, *button)) {
      return;
    }
    // xdg-shell transfers this device away from the client for an accepted
    // interactive operation. The notify variant also retires wlroots' implicit
    // button grab; m_grabButton keeps the raw release needed to finish ours.
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
  }

  void Cursor::beginClientResize(View* view, wlr_seat_client* seatClient, uint32_t serial, uint32_t edges) {
    const std::optional<uint32_t> button = clientPointerGrabButton(view, seatClient, serial);
    if (!button.has_value() || *button == 0 || !beginResize(view, edges, *button)) {
      return;
    }
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
  }

  void Cursor::warpTo(double lx, double ly) { warpTo(lx, ly, true); }

  void Cursor::warpToPreservingFocus(double lx, double ly) { warpTo(lx, ly, false); }

  bool Cursor::warpToView(View& view) {
    Output* output = view.currentOutput();
    if (output == nullptr) {
      return false;
    }

    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }

    wlr_box target = view.presentedBox();
    Workspace* workspace = view.workspace();
    if (view.layoutFullscreen()) {
      target = outputBox;
    } else if (view.maximizedToEdges()) {
      target = output->usableArea();
    } else if (view.tiled() && workspace != nullptr) {
      // A focus or move may have updated the scrolling offset and marked the layout stale. Flush it before reading the
      // final logical target, while leaving its visual transition animated.
      workspace->flushArrange();
      target = workspace->layout().targetBox(&view);
    } else {
      target.x = view.layoutTargetX();
      target.y = view.layoutTargetY();
    }

    if (target.width <= 0 || target.height <= 0) {
      target = outputBox;
    }
    wlr_box visible{};
    if (!wlr_box_intersection(&visible, &target, &outputBox)) {
      visible = outputBox;
    }
    warpToPreservingFocus(visible.x + visible.width / 2.0, visible.y + visible.height / 2.0);
    return true;
  }

  void Cursor::warpTo(double lx, double ly, bool allowFocusChange) {
    noteActivity();
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_warp(m_cursor, nullptr, lx, ly);
    m_server->notifyIdleActivity();
    processMotion(monotonicMsec(), oldX, oldY, allowFocusChange);
  }

  void Cursor::resetMode() {
    m_server->hideInsertHint();
    cancelDataDragEdgeScroll();
    View* view = grabbedView();
    if (std::holds_alternative<ScrollDragGrab>(m_grab)) {
      m_server->gestures()->endPointerScroll(true, 0);
    }
    const bool restoreDragPresentation = isDraggingView(view);
    if (auto* grab = std::get_if<MoveGrab>(&m_grab); grab != nullptr && grab->view != nullptr && grab->physics) {
      grab->view->endDragPhysics();
    }
    const auto* tiledResize = std::get_if<TiledResizeGrab>(&m_grab);
    Workspace* resizedWorkspace = tiledResize != nullptr ? tiledResize->workspace : nullptr;
    const bool restoreResizePresentation = std::holds_alternative<FloatingResizeGrab>(m_grab);
    if (std::holds_alternative<FloatingResizeGrab>(m_grab) && view != nullptr) {
      view->finishFloatingResize();
    }
    m_grab = PassthroughGrab{};
    m_grabButton = 0;
    if (restoreDragPresentation && view != nullptr) {
      view->restoreHomePresentation();
    }
    // Resize scaling belongs to the grab, including every sibling tile it resizes. Clear it before restoring clips:
    // an unchanged clip otherwise keeps the scaled source/destination until the client commits again.
    if (resizedWorkspace != nullptr) {
      for (View* resized : resizedWorkspace->allViews()) {
        if (resized->mapped() && resized->tiled()) {
          resized->resetPresentedSurface();
          resized->syncOwnedPresentation();
        }
      }
    } else if (restoreResizePresentation && view != nullptr && view->mapped()) {
      view->resetPresentedSurface();
      view->syncOwnedPresentation();
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

  void Cursor::cancelLayoutInteraction() {
    // An axis change keeps the same layout object, so cancelStaleTiledResize()
    // cannot see it; the session's edges would still mean the old orientation.
    if (std::holds_alternative<ScrollDragGrab>(m_grab) || std::holds_alternative<TiledResizeGrab>(m_grab)) {
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

    if (m_activeConstraint != nullptr && !constraintSurfaceActive()) {
      clearConstraint();
    }
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
    }

    double dx = event->delta_x;
    double dy = event->delta_y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      if (!confineDelta(&dx, &dy)) {
        return;
      }
    }

    confineRuleDelta(&dx, &dy);
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleMotionAbsolute(void* data) {
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();
    if (m_activeConstraint != nullptr && !constraintSurfaceActive()) {
      clearConstraint();
    }
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    double dx = lx - m_cursor->x;
    double dy = ly - m_cursor->y;
    if (m_activeConstraint != nullptr
        && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED
        && !confineDelta(&dx, &dy)) {
      return;
    }
    confineRuleDelta(&dx, &dy);
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
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
        m_server->dismissConfirmation();
      }
    }

    // Config mouse binds win over the overview and the built-in Mod+drag / Mod+resize grabs. Presses consumed here
    // swallow their paired release so clients never see an unmatched release.
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && isPassthrough()) {
      const uint32_t modifiers = m_server->keyboardModifiers();
      bool mouseBindExecuted = false;
      const std::optional<Keybind> bound = m_server->handleMouseBind(button, modifiers, &mouseBindExecuted);
      // Any press dismisses the cheatsheet, as any key press does, except one that just ran a cheatsheet action. Unlike
      // a key press, an unbound press is consumed: the overlay hides whatever sits under the cursor, so the click that
      // dismisses it must not also reach that surface.
      if (Cheatsheet* sheet = m_server->cheatsheet(); sheet != nullptr
          && sheet->visible()
          && !(bound.has_value() && mouseBindExecuted && isCheatsheetAction(bound->action))) {
        sheet->hide();
        if (!bound.has_value()) {
          m_swallowedButtons.push_back(button);
          return;
        }
      }
      if (bound.has_value()) {
        if (mouseBindExecuted
            && bound->action == KeybindAction::LayoutScrollDrag
            && m_server->gestures()->beginPointerScroll(m_cursor->x, m_cursor->y)) {
          setActiveConstraint(nullptr);
          m_grab = ScrollDragGrab{
              .button = button,
              .lastX = m_cursor->x,
              .lastY = m_cursor->y,
          };
          m_grabButton = button;
          setCompositorCursor("grabbing");
          clearPointerFocus();
          return;
        }
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
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      if (seat->drag == nullptr) {
        // The drag grab suppressed normal pointer motion. Re-run hit testing at
        // the unchanged position so the client can restore its hover cursor.
        processMotion(timeMsec, m_cursor->x, m_cursor->y);
      }
      return;
    }

    // An interactive pointer operation ends only when its initiating button is released.
    if (m_grabButton != 0 && button != m_grabButton) {
      if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_swallowedButtons.push_back(button);
        toggleDragTarget(button);
      }
      return;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (auto* grab = std::get_if<ScrollDragGrab>(&m_grab)) {
        if (button == grab->button) {
          m_server->gestures()->endPointerScroll(m_server->sessionLocked(), timeMsec);
          resetMode();
          // The grab cleared client focus on press and consumed every motion. Restore pointer delivery at the final
          // position without letting this synthetic pass override the column selected by the gesture.
          processMotion(timeMsec, m_cursor->x, m_cursor->y, false);
        }
        return;
      }
    }

    // A client that received the press owns the implicit grab, so its release
    // reaches it even though the overview now owns the pointer. Otherwise the
    // button stays down in that client for good.
    const bool releasesClientGrab = state == WL_POINTER_BUTTON_STATE_RELEASED && pointerFocusPinned();

    // Overview owns the pointer while it is up: cards are its own hit-test surface and the desktop underneath is inert.
    // Top/overlay layer surfaces (panels) stay fully interactive.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked() && !releasesClientGrab) {
      const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overviewPassthroughLayer(layer) && !overview->dragging()) {
        if (surface != nullptr) {
          setPointerFocus(surface, sx, sy, timeMsec);
        }
        m_server->seat()->notifyPointerModifiers();
        wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
        // The popup's xdg-shell grab already owns focus. Refocusing its parent layer would end the keyboard grab, whose
        // wlroots cancel handler also ends the pointer grab before the menu receives the matching release.
        if (pressed && !isXdgPopupSurface(surface)) {
          layer->focus();
        }
        return;
      }
      overview->handleButton(button, pressed, m_cursor->x, m_cursor->y, timeMsec);
      return;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (auto* grab = std::get_if<MoveGrab>(&m_grab)) {
        if (grab->pending) {
          resetMode();
        } else {
          finishMove();
        }
        return;
      }
      if (auto* grab = std::get_if<TiledResizeGrab>(&m_grab)) {
        if (grab->workspace != nullptr) {
          if (grab->workspace->focusedView() == grab->view) {
            grab->workspace->reevaluateFocusedColumn();
          }
          grab->workspace->markArrange(true);
        }
        resetMode();
        return;
      }
      if (std::holds_alternative<FloatingResizeGrab>(m_grab)) {
        resetMode();
        return;
      }
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), timeMsec, button, state);

      // After the final release, realign pointer focus (pinned by the implicit grab) with the surface under the cursor,
      // so a press without intervening motion targets it. The overview keeps the desktop inert, so focus goes nowhere.
      if (m_server->seat()->wlr()->pointer_state.button_count == 0) {
        const Overview* overview = m_server->overview();
        if (overview != nullptr && overview->active() && !m_server->sessionLocked()) {
          clearPointerFocus();
        } else {
          refreshPointerFocus();
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
      m_server->seat()->notifyPointerModifiers();
      wlr_seat_pointer_notify_button(seat, timeMsec, button, state);
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (m_server->sessionLocked()) {
      m_server->seat()->notifyPointerModifiers();
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

    // A press on a tab bar is the compositor's: no client sees it or its release. The left button selects the tab and
    // holds it, so dragging it past the threshold pulls it out of its column like any tiled window.
    if (const std::optional<TabHit> hit = tabBarHitAt(*m_server, m_cursor->x, m_cursor->y, surface, view, layer)) {
      const bool keyboardFree = m_server->exclusiveKeyboardLayer() == nullptr;
      Workspace* home = hit->tab->workspace();
      if (hit->part != TabBarPart::Tab) {
        // A cycle button selects the tab before or after the one on show, wrapping at the ends.
        if (button == BTN_LEFT && keyboardFree && home != nullptr) {
          if (View* target = home->tabs().cycleFrom(hit->tab, hit->part == TabBarPart::PreviousTab ? -1 : 1)) {
            m_server->focusView(target, FocusReason::PointerPress);
          }
        }
      } else if (button == BTN_LEFT && keyboardFree) {
        m_server->focusView(hit->tab, FocusReason::PointerPress);
        if (beginMove(hit->tab, button)) {
          return;
        }
      } else if (
          button == BTN_MIDDLE && keyboardFree && home != nullptr && home->layoutConfig().tabs.middleClickCloses
      ) {
        hit->tab->requestClose();
      }
      m_swallowedButtons.push_back(button);
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
      beginResize(view, view->tiled() ? 0 : floatResizeEdges(view), button);
      return;
    }
    // A window blocked by a modal dialog cannot use the press, so dragging it moves it the way Mod+drag does. Until the
    // pointer travels, the press is a plain click that lands the focus on the dialog.
    if (button == BTN_LEFT && view != nullptr && layer == nullptr && view->blockingDialog() != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      beginMove(view, button, true);
      return;
    }

    // Pointer focus must match the surface under the cursor before the button
    // event so wl_data_device drag serial validation succeeds.
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      setPointerFocus(surface, sx, sy, timeMsec);
    } else {
      clearPointerFocus();
    }

    m_server->seat()->notifyPointerModifiers();
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
      } else if (surface == nullptr || wlr_xwayland_surface_try_from_wlr_surface(surface) == nullptr) {
        // A press on an override-redirect X11 menu leaves the keyboard with the menu.
        wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
        m_server->refocusExplicit(m_server->outputFromWlr(wlrOutput));
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

    const bool shiftWheel = effective == WLR_MODIFIER_SHIFT && event->source != WL_POINTER_AXIS_SOURCE_FINGER;
    const bool boundShiftWheel = shiftWheel && std::ranges::any_of(config().keybinds, [&](const Keybind& bind) {
                                   return bind.submap == m_server->activeSubmap()
                                       && bind.wheel == eventDir
                                       && effective == (bind.modifiers | (bind.useMod ? m_server->modKey() : 0));
                                 });
    // Shift maps vertical wheel travel onto the horizontal axis. Explicit bindings and panels keep their input.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && (effective == 0 || (shiftWheel && !boundShiftWheel))) {
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (!overviewPassthroughLayer(layer)) {
        if (!overview->interactive()) {
          return;
        }
        if (event->source == WL_POINTER_AXIS_SOURCE_FINGER) {
          resetWheelAccumulation();
          // libinput already applies natural scrolling to axis events.
          overview->handleTouchpadAxis(
              event->pointer, isVertical, event->delta, event->time_msec, m_cursor->x, m_cursor->y
          );
          return;
        }
        const bool overviewVertical = isVertical && !shiftWheel;
        const int axis = overviewVertical ? 0 : 1;
        const double factor =
            overviewVertical ? config().overview.scrollFactorVertical : config().overview.scrollFactorHorizontal;
        m_wheelAccum[axis] +=
            (event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0)
            * factor;
        double& accumulated = m_wheelAccum[axis];
        while (std::abs(accumulated) >= 1.0) {
          overview->handleAxisNotch(overviewVertical, accumulated, m_cursor->x, m_cursor->y);
          accumulated -= std::copysign(1.0, accumulated);
        }
        return;
      }
    }

    // The axis is not going to the filmstrip: a modifier chord, a panel underneath, or no overview at all. Whatever
    // gesture was in flight has lost its input stream.
    if (Overview* overview = m_server->overview()) {
      overview->cancelNavigation();
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
    // An unmodified wheel over a tab bar steps through its tabs.
    if (!armed && effective == 0 && scrollTabBar(event, orientation)) {
      return;
    }
    if (!armed) {
      m_wheelAccum[orientation] = 0;
      const double scale = touchpadScrollFactor(event->pointer, isVertical);
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

  bool Cursor::scrollTabBar(const wlr_pointer_axis_event* event, int orientation) {
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    const std::optional<TabHit> hit = tabBarHitAt(*m_server, m_cursor->x, m_cursor->y, surface, view, layer);
    Workspace* workspace = hit ? hit->tab->workspace() : nullptr;
    if (workspace == nullptr || !workspace->layoutConfig().tabs.scrollSwitchesTabs) {
      return false;
    }
    double& accumulated = m_wheelAccum[orientation];
    accumulated +=
        event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
    while (std::abs(accumulated) >= 1.0) {
      const int direction = accumulated < 0 ? -1 : 1;
      accumulated -= direction;
      if (View* target = workspace->tabs().stepFrom(hit->tab, direction)) {
        m_server->focusView(target, FocusReason::PointerPress);
      }
    }
    return true;
  }

  void Cursor::handleFrame() {
    if (Overview* overview = m_server->overview()) {
      overview->handleTouchpadFrame();
    }
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
  }

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
    m_server->remapTouches();

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
        overview->handleButton(BTN_LEFT, true, lx, ly, event->time_msec);
        overview->handleButton(BTN_LEFT, false, lx, ly, event->time_msec);
      }
      return;
    }

    if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
      // A tap on a tab bar selects that tab, as a left click does.
      if (const std::optional<TabHit> hit = tabBarHitAt(*m_server, lx, ly, surface, view, layer)) {
        if (hit->part != TabBarPart::Tab) {
          Workspace* home = hit->tab->workspace();
          if (View* target = home != nullptr
                  ? home->tabs().cycleFrom(hit->tab, hit->part == TabBarPart::PreviousTab ? -1 : 1)
                  : nullptr) {
            m_server->focusView(target, FocusReason::PointerPress);
          }
        } else {
          m_server->focusView(hit->tab, FocusReason::PointerPress);
        }
        return;
      }
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
    if (!surfaceLocalCoordinates(m_server->scene(), point->surface, surfaceScale(point->surface), lx, ly, &sx, &sy)) {
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
    forwardEffectPointer();
    if (auto* grab = std::get_if<ScrollDragGrab>(&m_grab)) {
      if (m_server->sessionLocked()) {
        m_server->gestures()->endPointerScroll(true, timeMsec);
        resetMode();
      } else {
        m_server->gestures()->updatePointerScroll(m_cursor->x - grab->lastX, m_cursor->y - grab->lastY, timeMsec);
        grab->lastX = m_cursor->x;
        grab->lastY = m_cursor->y;
      }
      return;
    }
    // Overview owns motion: cards follow a drag, panels keep passthrough, and
    // the inert desktop underneath never receives enter/motion or hover focus.
    if (Overview* overview = m_server->overview(); overview != nullptr
        && overview->active()
        && !m_server->sessionLocked()
        && m_server->seat()->wlr()->drag == nullptr) {
      overview->handleMotion(m_cursor->x, m_cursor->y, timeMsec);
      if (overview->dragging()) {
        clearPointerFocus();
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (overviewPassthroughLayer(layer) && surface != nullptr) {
        setPointerFocus(surface, sx, sy, timeMsec);
        wlr_seat_pointer_notify_motion(m_server->seat()->wlr(), timeMsec, sx, sy);
        return;
      }
      clearPointerFocus();
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      return;
    }

    if (auto* grab = std::get_if<MoveGrab>(&m_grab)) {
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
          beginDrag(*grab);
        }
        processMove();
        updateDropTarget();
        updateDataDragEdgeScroll();
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
    updateDataDragEdgeScroll();
    if (seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      // Keep an implicit grab in the coordinate space established by the press. Re-resolving against the scene
      // would turn compositor-driven window animation into apparent pointer travel and make small clicks look like
      // client drags.
      const double scale = surfaceScale(seat->pointer_state.focused_surface);
      const double sx = seat->pointer_state.sx + ((m_cursor->x - oldX) * scale);
      const double sy = seat->pointer_state.sy + ((m_cursor->y - oldY) * scale);
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
      setPointerFocus(surface, sx, sy, timeMsec);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else {
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      clearPointerFocus();
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
      // `focused` in the workspaces payload is the active workspace of the cursor's output, so crossing heads changes
      // it even when no workspace activates: a switch to a workspace already active elsewhere only warps the cursor.
      m_server->scheduleIpcWorkspacesEvent();
      if (allowFocusChange
          && config().input.focus.followsMouse
          && !m_server->sessionLocked()
          && m_server->exclusiveKeyboardLayer() == nullptr) {
        m_server->refocusExplicit(m_server->outputFromWlr(pointerOutput));
      }
    }
  }

  View* Cursor::hoverFocus(
      View* view, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer, double oldX, double oldY
  ) {
    if (m_server->seat()->wlr()->drag != nullptr
        || m_server->sessionLocked()
        || *layer != nullptr
        || view == nullptr
        || !view->mapped()) {
      return view;
    }
    // Consume the invalidation only after every hover-focus eligibility gate. PointerHover never arms it, so layout
    // motion caused by this focus cannot turn into another focus on the next input event.
    const bool refocus = m_hoverFocusInvalidated;
    m_hoverFocusInvalidated = false;
    wlr_surface* oldSurface = nullptr;
    double oldSx = 0;
    double oldSy = 0;
    View* oldView = m_server->viewAt(oldX, oldY, &oldSurface, &oldSx, &oldSy);
    // Keyboard focus is an escape from rule confinement. While the pointer remains inside that window, an
    // invalidated hover must not immediately focus it again. A real re-entry (or a click) can still focus it.
    const bool entered = view != oldView || (refocus && !ruleConstraintApplies(*view));
    // Workspace focus is remembered independently from the seat. A pinned window from another workspace can own the
    // seat while this view remains its active workspace's remembered focus, so only seat-global activation makes this
    // handoff redundant.
    const bool alreadyFocused = view->activated();
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
    watch->cursor->setCursorSurface(event->surface, event->hotspot_x, event->hotspot_y, event->seat_client->client);
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
      clearPointerFocusOverridingGrab();
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
      wlr_surface* focused = state->v2->focused_surface;
      surfaceLocalCoordinates(m_server->scene(), focused, surfaceScale(focused), m_cursor->x, m_cursor->y, &sx, &sy);
      wlr_tablet_v2_tablet_tool_notify_motion(state->v2, sx, sy);
      forwardEffectPointer();
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
    forwardEffectPointer();
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
      state->inProximity = true;
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
    state->inProximity = false;
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
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr) {
      resetMode();
      return;
    }
    grab->view->setDragPosition(
        static_cast<int>(m_cursor->x - grab->offsetX), static_cast<int>(m_cursor->y - grab->offsetY)
    );
    if (grab->physics) {
      grab->view->moveDragPhysics(m_cursor->x - grab->lastX, m_cursor->y - grab->lastY);
      grab->lastX = m_cursor->x;
      grab->lastY = m_cursor->y;
    }
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
    view->applyDragPresentation();
  }

  void Cursor::beginDrag(MoveGrab& grab) {
    grab.pending = false;
    if (grab.sourceWorkspace != nullptr) {
      grab.sourceWorkspace->layoutDetach(grab.view);
    }
    grab.view->enterDragPresentation();
    grab.physics = grab.view->beginDragPhysics(grab.offsetX, grab.offsetY);
    grab.lastX = m_cursor->x;
    grab.lastY = m_cursor->y;
  }

  void Cursor::updateDropTarget() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || grab->pending) {
      return;
    }
    // Only a drop back into the strip has a target to draw and choose.
    if (grab->target != DragTarget::Tiled) {
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

  void Cursor::finishMove() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || !grab->view->mapped()) {
      resetMode();
      return;
    }
    View* view = grab->view;
    if (grab->physics) {
      view->endDragPhysics();
    }
    // Where the drag left the window. Read before the state change: becoming
    // floating re-places the window at its remembered origin, immediately when
    // position animations are off.
    const int dropX = view->sceneTree()->node.x;
    const int dropY = view->sceneTree()->node.y;
    // State first, placement second: a window that refused the drag's target
    // (a fullscreen window cannot be pinned) is still tiled here and drops back
    // into the layout rather than staying detached.
    applyDragTarget(*grab);
    if (view->tiled()) {
      finishTileMove();
    } else {
      finishFloatMove(dropX, dropY);
    }
  }

  void Cursor::applyDragTarget(const MoveGrab& grab) {
    View* view = grab.view;
    switch (grab.target) {
    case DragTarget::Tiled:
      if (!view->tiled()) {
        // finishTileMove inserts it at the drop target, so skip the layout
        // placement setFloating would do on its own.
        view->setFloating(false, false, View::TilePlacement::Detached);
      }
      break;
    case DragTarget::Floating:
      if (view->pinned()) {
        view->setPinned(false, false);
      }
      if (view->tiled()) {
        view->setFloating(true, false);
      }
      break;
    case DragTarget::Pinned:
      // Pinning a tiled window floats it and remembers to re-tile on unpin.
      view->setPinned(true, false);
      break;
    }
  }

  void Cursor::finishTileMove() {
    m_server->hideInsertHint();
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr) {
      resetMode();
      return;
    }
    View* view = grab->view;
    Workspace* target = grab->drop.workspace != nullptr ? grab->drop.workspace : grab->sourceWorkspace;
    if (target == nullptr && view != nullptr) {
      target = view->workspace();
    }
    if (view != nullptr && view->mapped() && target != nullptr) {
      applyDrop(
          *m_server, *view, *target, grab->drop, grab->sourceWidth.has_value() ? &*grab->sourceWidth : nullptr,
          /*animate=*/true
      );
    }
    resetMode();
  }

  void Cursor::finishFloatMove(int x, int y) {
    View* view = grabbedView();
    if (view == nullptr || !view->mapped()) {
      resetMode();
      return;
    }

    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      scratchpad->finishMove(view, output);
    } else if (output != nullptr && output->workspaceGroup() != nullptr) {
      if (Workspace* target = output->workspaceGroup()->active(); view->workspace() != target) {
        view->moveToWorkspace(target);
        target->exitFullscreenForIncomingView(view);
      }
    }
    // Drag presentation moves only the scene node. Commit its final position
    // after any workspace transfer so later restores use the dropped origin.
    view->setPosition(x, y);
    view->rememberFloatingPosition();

    resetMode();
    m_server->focusView(view, FocusReason::DragDrop);
  }

  void Cursor::toggleDragTarget(uint32_t button) {
    // The drag owns its initiating button, so the other main button is the one
    // free to retarget it.
    const uint32_t toggleButton = m_grabButton == BTN_LEFT ? BTN_RIGHT : BTN_LEFT;
    if (button != toggleButton) {
      return;
    }
    const WindowDragToggle toggle = config().input.windowDragToggle;
    if (toggle == WindowDragToggle::None) {
      return;
    }
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr || !grab->view->mapped()) {
      return;
    }
    // A scratchpad window has no layout to be dropped into.
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(grab->view)) {
      return;
    }
    if (grab->pending) {
      beginDrag(*grab);
    }

    if (toggle == WindowDragToggle::Pinned) {
      grab->target = grab->target == DragTarget::Pinned ? grab->unpinned : DragTarget::Pinned;
    } else {
      grab->target = grab->target == DragTarget::Tiled ? DragTarget::Floating : DragTarget::Tiled;
    }

    if (grab->target == DragTarget::Tiled) {
      processMove();
      updateDropTarget();
      return;
    }
    m_server->hideInsertHint();
    retargetDragSize(*grab);
  }

  void Cursor::retargetDragSize(MoveGrab& grab) {
    View* view = grab.view;
    const auto [width, height] = view->floatingRestoreSize();
    if (width <= 0 || height <= 0) {
      processMove();
      return;
    }
    // Keep the pointer's grip on the window: it stays over the same fraction of
    // the window it grabbed, whatever the drop resizes it to.
    const int presentedWidth = view->presentation().width();
    const int presentedHeight = view->presentation().height();
    if (presentedWidth > 0 && presentedHeight > 0) {
      grab.offsetX *= static_cast<double>(width) / presentedWidth;
      grab.offsetY *= static_cast<double>(height) / presentedHeight;
    }
    view->requestFloatingSize(width, height);
    view->beginResizeAnimation(width, height);
    if (grab.physics) {
      view->setDragPhysicsGrab(grab.offsetX, grab.offsetY);
    }
    processMove();
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
    const SizeHints hints = grab->view->sizeHints();

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
    const wlr_box& geo = view->geometryBox();
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
      const wlr_box& geo = view->geometryBox();
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
    Workspace* workspace = view->workspace();
    const wlr_box box = workspace->presentedTiledBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    wlr_box reachable = box;
    if (workspace->group() != nullptr && workspace->group()->output() != nullptr) {
      const wlr_box usable = workspace->usableArea();
      if (!wlr_box_intersection(&reachable, &box, &usable)) {
        return 0;
      }
    }
    uint32_t edges =
        workspace->layout().sanitizeResizeEdges(view, resizeEdgesForPoint(reachable, m_cursor->x, m_cursor->y));
    // Advertise an edge that extends past the output from the reachable
    // output boundary, since the pointer cannot approach the real edge.
    const double left = reachable.x;
    const double right = reachable.x + reachable.width;
    const double top = reachable.y;
    const double bottom = reachable.y + reachable.height;
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
    applyClientCursor();
    refreshPointerFocus();
  }

  bool Cursor::pointerFocusPinned() const {
    const wlr_seat* seat = m_server->seat()->wlr();
    // A client drag owns the seat grab and moves its own focus, so it is not an
    // implicit grab.
    return seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr;
  }

  bool Cursor::pointerContentsStale(const wlr_surface* surface, double sx, double sy) const {
    const wlr_seat* seat = m_server->seat()->wlr();
    if (surface != seat->pointer_state.focused_surface) {
      return true;
    }
    // Compare what the client receives, as wlroots does before sending motion.
    return surface != nullptr
        && (wl_fixed_from_double(sx) != wl_fixed_from_double(seat->pointer_state.sx)
            || wl_fixed_from_double(sy) != wl_fixed_from_double(seat->pointer_state.sy));
  }

  void Cursor::setPointerFocus(wlr_surface* surface, double sx, double sy, uint32_t timeMsec) {
    if (surface == nullptr) {
      clearPointerFocus();
      return;
    }
    if (pointerFocusPinned()) {
      return;
    }
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface == seat->pointer_state.focused_surface) {
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    }
  }

  void Cursor::clearPointerFocus() {
    if (pointerFocusPinned()) {
      return;
    }
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
  }

  void Cursor::clearPointerFocusOverridingGrab() { wlr_seat_pointer_clear_focus(m_server->seat()->wlr()); }

  void Cursor::refreshPointerFocus() {
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    const bool stale = !pointerFocusPinned() && pointerContentsStale(surface, sx, sy);
    setPointerFocus(surface, sx, sy, monotonicMsec());
    if (stale) {
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    }
  }

  void Cursor::refreshPointerContents(const Output* output) {
    wlr_seat* seat = m_server->seat()->wlr();
    if (output == nullptr
        || m_cursorHidden
        || std::ranges::any_of(m_tools, [](const auto& tool) { return tool->inProximity; })
        || wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y) != output->wlr()) {
      return;
    }
    if (seat->drag != nullptr) {
      updateDataDragEdgeScroll();
      if (m_server->sessionLocked()) {
        wlr_seat_pointer_notify_clear_focus(seat);
        wlr_seat_pointer_notify_frame(seat);
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
      if (surface != nullptr) {
        const uint32_t timeMsec = monotonicMsec();
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
      } else {
        wlr_seat_pointer_notify_clear_focus(seat);
      }
      wlr_seat_pointer_notify_frame(seat);
      return;
    }
    if (tiledMoveDragActive()) {
      // The compositor owns drop targeting during a window move; client pointer focus stays suspended.
      updateDataDragEdgeScroll();
      updateDropTarget();
      return;
    }
    if (!isPassthrough() || seat->pointer_state.button_count != 0) {
      return;
    }
    // Content still in motion would flicker hover state on every frame. The next press resolves it regardless.
    if (!m_server->sessionLocked()
        && ((m_server->overview() != nullptr && m_server->overview()->active())
            || m_server->animationsActiveFor(output)
            || (m_server->gestures() != nullptr && m_server->gestures()->movingContent()))) {
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (!pointerContentsStale(surface, sx, sy)) {
      return;
    }
    if (surface != nullptr) {
      setPointerFocus(surface, sx, sy, monotonicMsec());
    } else {
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      clearPointerFocus();
    }
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    updateConstraintForSurface(surface);
    updateInteractiveCursor(view);
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
    if (std::holds_alternative<MoveGrab>(m_grab)) {
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
    const wlr_box usable = grab->workspace->tiledArea();
    grab->session->applyDelta(m_cursor->x - grab->startX, m_cursor->y - grab->startY, usable);
    grab->view->setMaximizedState(false);
    grab->workspace->markArrange(false);
  }

} // namespace umbriel
