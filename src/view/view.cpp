#include "view/view.h"

#include "config/config.h"
#include "config/resolve.h"
#include "config/store.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/maximize.h"
#include "view/output_clip.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {
  namespace {
    constexpr Logger kLog("view");

    // How long the layout withholds the column size after an unfullscreen configure (sent with size 0x0).
    // xwayland-satellite acks configures immediately, so acks prove nothing; the X11 client's re-request arrives
    // through a full X round trip (observed 310-330 ms for a loaded game). A client that truly accepts windowed mode
    // exits the grace early by committing a geometry different from its fullscreen one.
    constexpr uint64_t kUnfullscreenGraceMsec = 1000;

    bool looksTiled(const wlr_xdg_toplevel* toplevel) {
      const auto& state = toplevel->current;
      const bool fixedWidth = state.max_width > 0 && state.min_width == state.max_width;
      const bool fixedHeight = state.max_height > 0 && state.min_height == state.max_height;
      return toplevel->parent == nullptr && !fixedWidth && !fixedHeight;
    }

    template <typename T>
    bool changedInitialRule(const std::optional<T>& current, const std::optional<T>& initiallyApplied) {
      return current.has_value() && current != initiallyApplied;
    }

    bool sceneNodeShowsSurface(wlr_scene_node* node, wlr_surface* surface) {
      switch (node->type) {
      case WLR_SCENE_NODE_BUFFER: {
        wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
        return sceneSurface != nullptr && sceneSurface->surface == surface;
      }
      case WLR_SCENE_NODE_TREE: {
        wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
        wlr_scene_node* child = nullptr;
        wl_list_for_each(child, &tree->children, link) {
          if (sceneNodeShowsSurface(child, surface)) {
            return true;
          }
        }
        return false;
      }
      default:
        return false;
      }
    }

    // Direct child of the xdg scene tree that holds the toplevel subsurface tree.
    wlr_scene_node* toplevelSurfaceTreeNode(wlr_scene_tree* xdgTree, wlr_surface* mainSurface) {
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &xdgTree->children, link) {
        if (child->type == WLR_SCENE_NODE_TREE && sceneNodeShowsSurface(child, mainSurface)) {
          return child;
        }
      }
      return nullptr;
    }
    WorkspaceGroup* windowRuleWorkspaceGroup(Server& server, const ResolvedWindowRule& rule, WorkspaceGroup* fallback) {
      Output* targetOutput = nullptr;
      if (rule.defaultOutput) {
        targetOutput = server.outputFromName(*rule.defaultOutput);
      } else if (rule.defaultWorkspace) {
        const auto index = static_cast<size_t>(*rule.defaultWorkspace - 1);
        if (const OutputRule* owner = uniqueFixedWorkspaceOwner(config(), index)) {
          targetOutput = server.outputFromName(owner->name);
        }
      }
      return targetOutput != nullptr && targetOutput->workspaceGroup() != nullptr ? targetOutput->workspaceGroup()
                                                                                  : fallback;
    }

    Workspace* windowRuleWorkspace(WorkspaceGroup* group, const ResolvedWindowRule& rule) {
      if (group == nullptr) {
        return nullptr;
      }
      Workspace* target = group->active();
      if (rule.defaultWorkspace) {
        if (Workspace* ruleTarget = group->workspaceAtClamped(static_cast<size_t>(*rule.defaultWorkspace - 1))) {
          target = ruleTarget;
        }
      }
      return target;
    }

  } // namespace

  View::View(Server& server, wlr_xdg_toplevel* toplevel)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_toplevel(toplevel),
        m_xwayland(server.isXwaylandSurface(toplevel->base->surface)) {
    m_server->registerAnimatable(this);
    // Register map/unmap listeners BEFORE creating the scene tree so our handlers fire before wlroots' internal unmap
    // handler disables the surface subtree (needed for close-animation buffer snapshot).
    m_map.notify = onMap;
    wl_signal_add(&m_toplevel->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_toplevel->base->surface->events.unmap, &m_unmap);

    m_sceneTree = wlr_scene_xdg_surface_create(m_server->xdgTree(), m_toplevel->base);
    m_sceneTree->node.data = sceneNodeData(this);
    m_toplevel->base->data = m_sceneTree;
    wlr_scene_node_set_enabled(&m_sceneTree->node, false);
    m_presentation.createBackdrop(m_sceneTree);
    notifyOutputScale();

    m_commit.notify = onCommit;
    wl_signal_add(&m_toplevel->base->surface->events.commit, &m_commit);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_toplevel->events.destroy, &m_destroy);

    m_requestMove.notify = onRequestMove;
    wl_signal_add(&m_toplevel->events.request_move, &m_requestMove);
    m_requestResize.notify = onRequestResize;
    wl_signal_add(&m_toplevel->events.request_resize, &m_requestResize);
    m_requestMaximize.notify = onRequestMaximize;
    wl_signal_add(&m_toplevel->events.request_maximize, &m_requestMaximize);
    m_requestFullscreen.notify = onRequestFullscreen;
    wl_signal_add(&m_toplevel->events.request_fullscreen, &m_requestFullscreen);
    m_setTitle.notify = onSetTitle;
    wl_signal_add(&m_toplevel->events.set_title, &m_setTitle);
    m_setAppId.notify = onSetAppId;
    wl_signal_add(&m_toplevel->events.set_app_id, &m_setAppId);

    if (wlr_foreign_toplevel_manager_v1* manager = m_server->foreignToplevelManager()) {
      m_foreign = wlr_foreign_toplevel_handle_v1_create(manager);
      if (m_foreign != nullptr) {
        m_foreign->data = this;
        m_foreignActivate.notify = onForeignActivate;
        wl_signal_add(&m_foreign->events.request_activate, &m_foreignActivate);
        m_foreignClose.notify = onForeignClose;
        wl_signal_add(&m_foreign->events.request_close, &m_foreignClose);
        m_foreignDestroy.notify = onForeignDestroy;
        wl_signal_add(&m_foreign->events.destroy, &m_foreignDestroy);
        updateForeignIdentity();
        updateForeignState();
      }
    }

    if (wlr_ext_foreign_toplevel_list_v1* list = m_server->extForeignToplevelList()) {
      const wlr_ext_foreign_toplevel_handle_v1_state state = {
          .title = m_toplevel->title,
          .app_id = m_toplevel->app_id,
      };
      m_extForeign = wlr_ext_foreign_toplevel_handle_v1_create(list, &state);
      if (m_extForeign != nullptr) {
        m_extForeign->data = this;
        m_extForeignDestroy.notify = onExtForeignDestroy;
        wl_signal_add(&m_extForeign->events.destroy, &m_extForeignDestroy);
      }
    }
  }

  View::~View() {
    m_server->unregisterAnimatable(this);
    setWorkspace(nullptr);
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_requestMove.link);
      wl_list_remove(&m_requestResize.link);
      wl_list_remove(&m_requestMaximize.link);
      wl_list_remove(&m_requestFullscreen.link);
      wl_list_remove(&m_setTitle.link);
      wl_list_remove(&m_setAppId.link);
    }
    if (m_foreign != nullptr) {
      leaveForeignOutput();
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }
    if (m_extForeign != nullptr) {
      wl_list_remove(&m_extForeignDestroy.link);
      wlr_ext_foreign_toplevel_handle_v1_destroy(m_extForeign);
      m_extForeign = nullptr;
    }
    if (m_captureSource != nullptr) {
      wl_list_remove(&m_captureSourceDestroy.link);
      m_captureSource = nullptr;
    }
  }

  void View::setWorkspace(Workspace* workspace, bool attachToLayout) {
    if (workspace != nullptr
        && m_server->scratchpadManager() != nullptr
        && m_server->scratchpadManager()->contains(this)) {
      return;
    }
    if (m_workspace == workspace) {
      return;
    }
    Output* previousOutput =
        m_workspace != nullptr && m_workspace->group() != nullptr ? m_workspace->group()->output() : nullptr;
    if (m_workspace != nullptr) {
      Workspace* previous = m_workspace;
      const bool sameGroup = workspace != nullptr && workspace->group() == previous->group();
      m_workspace = nullptr;
      // Keep an empty destination alive until this view has been attached.
      // addView() reconciles the group after the transfer is complete.
      previous->removeView(this, std::nullopt, !sameGroup);
    }
    m_workspace = workspace;
    if (m_workspace != nullptr) {
      m_workspace->addView(this, attachToLayout);
    } else {
      setOnActiveWorkspace(true);
    }
    notifyOutputScale();
    if (m_mapped && m_onActiveWorkspace) {
      enterForeignOutput();
    }
    if (m_mapped) {
      m_server->scheduleIpcWindowsEvent();
      if (previousOutput != nullptr) {
        previousOutput->updateVrr();
      }
      if (m_workspace != nullptr && m_workspace->group() != nullptr) {
        m_workspace->group()->output()->updateVrr();
      }
    }
  }

  void View::detachWorkspace() {
    reparentShadow(nullptr);
    m_workspace = nullptr;
    setOnActiveWorkspace(true);
  }

  void View::setOnActiveWorkspace(bool active) {
    if (m_pinned) {
      return;
    }
    if (m_onActiveWorkspace == active) {
      return;
    }
    m_onActiveWorkspace = active;
    if (m_sceneTree != nullptr) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, active);
    }
    m_decoration.setShadowEnabled(active);
    if (!m_mapped) {
      return;
    }
    if (active) {
      enterForeignOutput();
    } else {
      // Output membership tracks the physical monitor, not the active workspace: a window on another workspace is still
      // on its output. Leaving the output here would make foreign-toplevel clients drop the window from their task
      // lists. Scratchpad windows have no workspace at all, so they genuinely leave.
      if (m_workspace == nullptr) {
        leaveForeignOutput();
      }
      setForeignActivated(false);
      setBorderFocused(false);
    }
  }

  void View::setNodeEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_sceneTree->node, enabled);
    m_decoration.setShadowEnabled(enabled);
  }

  void View::raiseToTop() {
    wlr_scene_node_raise_to_top(&m_sceneTree->node);
    m_decoration.raiseShadowToTop();
  }

  void View::setScratchpadBorder(bool scratchpad) {
    if (m_scratchpadBorder == scratchpad) {
      return;
    }
    m_scratchpadBorder = scratchpad;
    setBorderFocused(m_borderFocusedState);
  }

  void View::reparentShadow(wlr_scene_tree* shadowLayer) {
    m_decoration.reparentShadow(shadowLayer, m_sceneTree->node.x, m_sceneTree->node.y, m_sceneTree->node.enabled);
    updateShadow();
  }

  void View::applySeatFocus(bool withKeyboard) {
    // Mechanism only. Policy lives in Server::focusView; do not call directly
    // from input/event code.
    if (!m_onActiveWorkspace && !m_pinned) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = m_toplevel->base->surface;
    // Always clear other views first: the previous seat surface may be a layer, so
    // deactivating only that surface can leave another window's focus border on.
    m_server->deactivateViews(this);

    raiseToTop();
    if (!m_toplevel->scheduled.activated) {
      wlr_xdg_toplevel_set_activated(m_toplevel, true);
    }
    setBorderFocused(true);
    setForeignActivated(true);

    if (!withKeyboard || seat->keyboard_state.focused_surface == surface) {
      return;
    }
    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
  }

  void View::cancelPositionAnimation() {
    // Freeze wherever the node currently sits; callers use this to take over
    // positioning (drags, layout snaps) without a jump.
    m_posX.snap(m_sceneTree->node.x);
    m_posY.snap(m_sceneTree->node.y);
  }

  void View::scheduleFrame() {
    if (Output* output = currentOutput()) {
      wlr_output_schedule_frame(output->wlr());
    }
  }

  void View::setFadeAlpha(float alpha) {
    m_fadeAlpha = alpha;
    float effective = effectiveOpacity();
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &effective
    );
    setBorderFocused(m_borderFocusedState);
    m_decoration.setAlpha(effective);
  }

  void View::applyEffectiveOpacity() {
    if (m_sceneTree == nullptr) {
      return;
    }
    float effective = effectiveOpacity();
    if (effective >= 1.0F) {
      return;
    }
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &effective
    );
  }

  void View::cancelFadeAnimation() {
    m_fade.snap(1.0);
    setFadeAlpha(1.0F);
  }

  int View::presentedWidth(const wlr_box& target) const {
    if (sizeGrabActive()) {
      return target.width;
    }
    if (sizeAnimating()) {
      return m_presentation.width();
    }
    if (m_toplevel->current.fullscreen) {
      return target.width;
    }
    return std::min(m_toplevel->base->geometry.width, target.width);
  }

  void View::trackPresentedSize(int width, int height) { m_presentation.track(width, height); }

  int View::presentedHeight(const wlr_box& target) const {
    if (sizeGrabActive()) {
      return target.height;
    }
    if (sizeAnimating()) {
      return m_presentation.height();
    }
    if (m_toplevel->current.fullscreen) {
      return target.height;
    }
    return std::min(m_toplevel->base->geometry.height, target.height);
  }

  // Fullscreen chrome follows committed state. Transitions may scale the old buffer, while settled mismatched buffers
  // remain centered and cropped without distortion.

  void View::updateFullscreenPresentation(int width, int height) {
    m_presentation.updateFullscreen(
        m_toplevel->current.fullscreen, width, height, toplevelSurfaceTreeNode(m_sceneTree, m_toplevel->base->surface),
        m_toplevel->base->geometry
    );
  }

  void View::applyPresentedCrop(const wlr_box& content, const wlr_box& surfaceClip) {
    m_presentation.applyCrop(m_sceneTree, m_toplevel->base->surface, m_toplevel->base->geometry, content, surfaceClip);
  }

  void View::resetPresentedSurface() {
    m_presentation.resetCrop(m_sceneTree, m_toplevel->base->surface);
    // Clear the subsurface clip so the next syncViewPresentation re-applies the resting clip through a real
    // reconfigure: an unchanged clip box early-outs and would leave the animated src/dst behind.
    setSurfaceTreeClip(nullptr);
  }

  void View::applyPresentedSize() {
    // Buffer scale + crop is derived in setOutputClip (applyPresentedCrop) via syncViewPresentation below, so the
    // animated size and the output clip are always applied together instead of fighting over dest_size.
    const int width = m_presentation.width();
    const int height = m_presentation.height();
    updateBorderGeometry(width, height);
    if (!m_toplevel->scheduled.fullscreen) {
      updateShadow(width, height);
    }
    updateBlur(width, height);
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
    }
  }

  void View::finishSizeAnimation() {
    const wlr_box& geo = m_toplevel->base->geometry;
    m_presentation.setSize(geo.width, geo.height);
    resetPresentedSurface();
    updateBorderGeometry();
    updateBlur();
    updateShadow();
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
    }
  }

  void View::cancelSizeAnimation() {
    if (!sizeAnimating()) {
      return;
    }
    const wlr_box& geo = m_toplevel->base->geometry;
    m_presentation.snapTo(geo.width, geo.height);
    finishSizeAnimation();
  }

  bool View::sizeGrabActive() const {
    const Cursor* cursor = m_server->cursor();
    if (cursor == nullptr) {
      return false;
    }
    return cursor->grabbedView() == this || cursor->isResizingWorkspace(m_workspace);
  }

  void View::enterDragPresentation() {
    cancelPositionAnimation();
    m_dragOpacity = kDragOpacity;
    setFadeAlpha(m_fadeAlpha);
    m_dragOpacityCommitPending = false;
    wlr_scene_node_reparent(&m_sceneTree->node, m_server->dragTree());
    reparentShadow(m_server->dragShadowTree());
    setNodeEnabled(true);
    clearOutputClip();
    raiseToTop();
  }

  void View::restoreHomePresentation() {
    m_dragOpacity = 1.0F;
    m_dragOpacityCommitPending = false;
    setFadeAlpha(m_fadeAlpha);
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(this)) {
      scratchpad->restorePresentation(this);
      return;
    }
    if (m_pinned) {
      restorePinnedSceneParent();
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
      }
      return;
    }

    wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
    if (m_workspace == nullptr) {
      reparentShadow(nullptr);
      setNodeEnabled(m_mapped && m_onActiveWorkspace);
      return;
    }
    reparentShadow(m_workspace->shadowLayer());
    setNodeEnabled(m_mapped && m_onActiveWorkspace);
    m_workspace->restackFloatingViews();
    if (m_mapped) {
      m_workspace->syncViewPresentation(this);
    }
  }

  void View::beginResizeAnimation(int width, int height, bool allowFullscreen) {
    if (!m_mapped
        || !m_onActiveWorkspace
        || (m_workspace == nullptr && !allowFullscreen)
        || (!allowFullscreen && (m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen))
        || width <= 0
        || height <= 0) {
      return;
    }
    // Nothing presented yet (first map): the fade-in covers the appear.
    if (m_presentation.width() <= 0 || m_presentation.height() <= 0) {
      return;
    }
    if (sizeGrabActive() || (width == m_presentation.width() && height == m_presentation.height())) {
      return;
    }
    if (m_presentation.targeting(width, height)) {
      return;
    }
    m_presentation.animateTo(width, height, config().appearance.animationMs);
    scheduleFrame();
  }

  void View::clipForSnapMove(int x, int y) {
    if (!m_mapped
        || m_workspace == nullptr
        || m_workspace->group() == nullptr
        || m_workspace->group()->output() == nullptr) {
      return;
    }
    // Drags intentionally span outputs; entering the neighbor is the point.
    if (m_server->cursor() != nullptr && m_server->cursor()->isDraggingView(this)) {
      return;
    }
    const int oldX = m_sceneTree->node.x;
    const int oldY = m_sceneTree->node.y;
    const wlr_box& geo = m_toplevel->base->geometry;
    if ((oldX == x && oldY == y) || geo.width <= 0 || geo.height <= 0) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_workspace->group()->output()->wlr(), &outputBox);
    if (wlr_box_empty(&outputBox)) {
      return;
    }
    // The union of the content footprint at both endpoints bounds every box the
    // move can produce; inside the home output means no neighbor can be grazed.
    const wlr_box moveUnion{
        std::min(oldX, x),
        std::min(oldY, y),
        geo.width + std::max(x, oldX) - std::min(x, oldX),
        geo.height + std::max(y, oldY) - std::min(y, oldY),
    };
    wlr_box contained{};
    if (wlr_box_intersection(&contained, &moveUnion, &outputBox) && wlr_box_equal(&contained, &moveUnion)) {
      return;
    }
    // Clip to the region visible on the home output at BOTH endpoints (surface-local output box at node position p is
    // outputBox - p + geo offset). The caller re-derives the exact clip right after the move.
    const wlr_box atOld{outputBox.x - oldX + geo.x, outputBox.y - oldY + geo.y, outputBox.width, outputBox.height};
    const wlr_box atNew{outputBox.x - x + geo.x, outputBox.y - y + geo.y, outputBox.width, outputBox.height};
    wlr_box pre{};
    if (!wlr_box_intersection(&pre, &atOld, &atNew)) {
      // Not visible at both endpoints at once: park the clip off-surface (an
      // empty box would mean "unclip" to wlroots and flash the full buffer).
      constexpr int kFarAway = 1 << 20;
      pre = {-kFarAway, -kFarAway, 1, 1};
    }
    setSurfaceTreeClip(&pre);
  }

  void View::setPosition(int x, int y) {
    m_posX.snap(x);
    m_posY.snap(y);
    m_positioned = true;
    clipForSnapMove(x, y);
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
    m_decoration.setShadowPosition(x, y);
  }

  void View::setDragPosition(int x, int y) {
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
    m_decoration.setShadowPosition(x, y);
  }

  void View::animateTo(int x, int y) {
    // First placement snaps: the node starts at the default (0,0) world origin, so animating would fly the window
    // across the layout on open. The fade-in covers the appear instead.
    if (!m_mapped || !m_onActiveWorkspace || !m_positioned) {
      setPosition(x, y);
      return;
    }
    const int fromX = m_sceneTree->node.x;
    const int fromY = m_sceneTree->node.y;
    if (fromX == x && fromY == y) {
      m_posX.snap(x);
      m_posY.snap(y);
      return;
    }
    // Animate from wherever the node visually is, not from the last target.
    m_posX.snap(fromX);
    m_posX.retarget(x, config().appearance.animationMs);
    m_posY.snap(fromY);
    m_posY.retarget(y, config().appearance.animationMs);
    scheduleFrame();
  }

  bool View::tickAnimations(uint64_t nowMsec) {
    bool active = false;

    // Disable sibling size animations during a tiled resize, so they do not
    // trail the pointer while clients acknowledge successive configures.
    const Cursor* cursor = m_server->cursor();
    if (cursor != nullptr && cursor->isResizingWorkspace(m_workspace) && sizeAnimating()) {
      cancelSizeAnimation();
    }

    const bool movedX = m_posX.tick(nowMsec);
    const bool movedY = m_posY.tick(nowMsec);
    if (movedX || movedY) {
      const int cx = static_cast<int>(std::lround(m_posX.current()));
      const int cy = static_cast<int>(std::lround(m_posY.current()));
      wlr_scene_node_set_position(&m_sceneTree->node, cx, cy);
      m_decoration.setShadowPosition(cx, cy);
      // Clips are derived from the node's current position; refresh them as the
      // node moves or partial-visibility trims land displaced.
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
      }
      active = m_posX.animating() || m_posY.animating();
    }

    if (m_presentation.tick(nowMsec)) {
      applyPresentedSize();
      if (sizeAnimating()) {
        active = true;
      } else {
        finishSizeAnimation();
      }
    }

    if (m_fade.tick(nowMsec)) {
      setFadeAlpha(static_cast<float>(m_fade.current()));
      active = active || m_fade.animating();
    }
    // Unfullscreen grace: the compositor asked the client to leave fullscreen with a size-0x0 configure. A compliant
    // client commits its own windowed geometry (handleCommit ends the grace and tiles it); a client that re-requests
    // fullscreen cancels it in setFullscreen. Expiry means the client ignored the state change entirely: some game
    // engines only react to an actual resize, and resizing them permanently breaks their X11 mouse mapping, so
    // re-assert fullscreen instead of poking them with the column size.
    if (m_pendingUnfullscreenSize) {
      if (m_unfullscreenGraceStartMsec == 0) {
        m_unfullscreenGraceStartMsec = nowMsec;
      }
      if (nowMsec - m_unfullscreenGraceStartMsec >= kUnfullscreenGraceMsec) {
        m_pendingUnfullscreenSize = false;
        m_unfullscreenGraceStartMsec = 0;
        if (m_tiled && !m_toplevel->scheduled.fullscreen) {
          kLog.debug(
              "unfullscreen grace expired without compliance for '{}'; re-asserting fullscreen",
              m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?"
          );
          setFullscreen(true);
        }
      } else {
        active = true;
      }
    }
    // The scene helper may process a surface commit after View::handleCommit.
    // Reapply on the ensuing frame, after every commit listener has run.
    if (m_dragOpacityCommitPending) {
      applyEffectiveOpacity();
      m_dragOpacityCommitPending = false;
    }
    return active;
  }

  bool View::animatesOn(const Output* output) const {
    const Workspace* workspace = m_workspace;
    return workspace != nullptr && workspace->group() != nullptr && workspace->group()->output() == output;
  }

  bool View::hasActiveAnimations() const {
    return m_posX.animating()
        || m_posY.animating()
        || sizeAnimating()
        || m_fade.animating()
        || m_pendingUnfullscreenSize;
  }

  bool View::layoutFullscreen() const { return m_toplevel->scheduled.fullscreen || m_pendingUnfullscreenSize; }

  void View::onMap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void View::onUnmap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void View::onCommit(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void View::onDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void View::onRequestMove(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMove);
    self->handleRequestMove();
  }

  void View::onRequestResize(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_requestResize);
    self->handleRequestResize(data);
  }

  void View::onRequestMaximize(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMaximize);
    self->handleRequestMaximize();
  }

  void View::onRequestFullscreen(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestFullscreen);
    self->handleRequestFullscreen();
  }

  void View::onSetTitle(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setTitle);
    self->handleSetTitle();
  }

  void View::onSetAppId(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setAppId);
    self->handleSetAppId();
  }

  wlr_box View::floatingUsableArea() const {
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      return m_workspace->group()->output()->usableArea();
    }
    return m_server->usableAreaAt(m_sceneTree->node.x, m_sceneTree->node.y);
  }

  void View::clampFloatingPosition() {
    if (m_tiled
        || !m_mapped
        || m_toplevel->scheduled.fullscreen
        || m_toplevel->scheduled.maximized
        || sizeGrabActive()
        || m_posX.animating()
        || m_posY.animating()) {
      return;
    }
    if (Cursor* cursor = m_server->cursor(); cursor != nullptr && cursor->isDraggingView(this)) {
      return;
    }

    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = m_toplevel->base->geometry;
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return;
    }

    const FloatingPoint clamped =
        clampFloatingOrigin({.x = m_sceneTree->node.x, .y = m_sceneTree->node.y}, geo, usable);
    if (clamped.x != m_sceneTree->node.x || clamped.y != m_sceneTree->node.y) {
      setPosition(clamped.x, clamped.y);
    }
  }

  void View::rememberFloatingPosition() {
    if (m_tiled) {
      return;
    }
    m_floating.rememberPositionFraction({m_sceneTree->node.x, m_sceneTree->node.y}, floatingUsableArea());
  }

  void View::restoreFloatingPosition() {
    if (m_tiled) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (const std::optional<FloatingPoint> origin = m_floating.restoredOrigin(usable)) {
      animateTo(origin->x, origin->y);
      // Re-anchor on the new usable area so a second cross-output move lands
      // proportionally again instead of reusing a stale fraction.
      m_floating.rememberPositionFraction(*origin, usable);
      return;
    }
    clampFloatingPosition();
  }

  bool View::centerFloating() {
    if (!m_mapped || m_tiled || m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = m_toplevel->base->geometry;
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    const FloatingPoint origin = centeredOrigin(usable, geo.width, geo.height);
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  void View::placeInUsableArea(const std::optional<WindowPosition>& position) {
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }

    // Floats keep their own size; only center within the usable area.
    const wlr_box& geo = m_toplevel->base->geometry;
    const int width = geo.width > 0 ? geo.width : usable.width;
    const int height = geo.height > 0 ? geo.height : usable.height;
    FloatingPoint origin = centeredOrigin(usable, width, height);
    if (position) {
      origin = {.x = usable.x + position->x, .y = usable.y + position->y};
      switch (position->anchor) {
      case WindowPositionAnchor::TopLeft:
        break;
      case WindowPositionAnchor::TopRight:
        origin.x = usable.x + usable.width - width - position->x;
        break;
      case WindowPositionAnchor::BottomLeft:
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::BottomRight:
        origin.x = usable.x + usable.width - width - position->x;
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::Top:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        break;
      case WindowPositionAnchor::Bottom:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::Left:
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      case WindowPositionAnchor::Right:
        origin.x = usable.x + usable.width - width - position->x;
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      case WindowPositionAnchor::Center:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      }
      origin = clampFloatingOrigin(origin, {.x = 0, .y = 0, .width = width, .height = height}, usable);
      m_floating.rememberPositionFraction(origin, usable);
    }
    m_positioned = true;
    wlr_scene_node_set_position(&m_sceneTree->node, origin.x, origin.y);
    m_decoration.setShadowPosition(origin.x, origin.y);
  }

  bool View::decorated() const { return m_decoration.bordersVisible(); }

  int View::borderInset() const { return decorated() ? config().appearance.totalBorderWidth() : 0; }

  int View::surfaceRadius() const {
    return decorated() && !m_toplevel->scheduled.fullscreen ? config().appearance.cornerRadius : 0;
  }

  void View::setBorderFocused(bool focused) {
    const bool focusChanged = m_borderFocusedState != focused;
    m_borderFocusedState = focused;
    m_decoration.setBorderColor(focused, m_scratchpadBorder, m_fadeAlpha * m_dragOpacity);
    if (focusChanged && m_mapped) {
      applyDynamicRules();
    }
  }

  void View::setUrgent(bool urgent) {
    if (m_urgent == urgent) {
      return;
    }
    m_urgent = urgent;
    if (m_workspace != nullptr) {
      m_workspace->updateUrgent();
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::applyCornerRadius() { applyCornerRadii(corner_radii_all(surfaceRadius())); }

  void View::applyCornerRadii(fx_corner_radii corners) {
    // Every buffer under the tree has to be visited, but only the toplevel's own
    // surface should round: subsurfaces are separate scene buffers.
    struct Ctx {
      View* view;
      fx_corner_radii corners;
    } ctx{this, corners};
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto* ctx = static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr || sceneSurface->surface != ctx->view->m_toplevel->base->surface) {
            return;
          }
          wlr_scene_buffer_set_corner_radii(buffer, ctx->corners);
        },
        &ctx
    );
  }

  void View::updateBlur() {
    const wlr_box& geometry = m_toplevel->base->geometry;
    updateBlur(geometry.width, geometry.height);
  }

  void View::updateBlur(int contentWidth, int contentHeight) {
    const wlr_box nodeBox{0, 0, contentWidth, contentHeight};
    m_decoration.updateBlur(
        m_sceneTree, m_toplevel->base->surface, nodeBox, m_toplevel->base->geometry, surfaceRadius(), nullptr,
        effectiveOpacity()
    );
  }

  void View::updateShadow() {
    if (m_toplevel->scheduled.fullscreen || m_maximizedToEdges) {
      m_decoration.hideShadow();
      return;
    }
    // A tiled view's committed geometry lags the layout, so the presented size is
    // the one the shadow must match; a float has no such lag.
    const wlr_box& geometry = m_toplevel->base->geometry;
    updateShadow(
        m_tiled && m_presentation.width() > 0 ? m_presentation.width() : geometry.width,
        m_tiled && m_presentation.height() > 0 ? m_presentation.height() : geometry.height
    );
  }

  void View::updateShadow(int contentWidth, int contentHeight) {
    const int inset = borderInset();
    m_decoration.updateShadow(
        contentWidth, contentHeight, inset, decorated() ? expandedRadius(config().appearance.cornerRadius, inset) : 0
    );
  }

  void View::showDecorations(bool enabled) {
    m_decoration.ensureBorders(m_sceneTree);
    m_decoration.setBordersEnabled(enabled);
    updateBorderGeometry();
    applyCornerRadius();
    updateBlur();
    updateShadow();
  }

  void View::updateBorderGeometry() {
    const wlr_box& geometry = m_toplevel->base->geometry;
    updateBorderGeometry(geometry.width, geometry.height);
  }

  void View::updateBorderGeometry(int contentWidth, int contentHeight) {
    m_decoration.updateBorderGeometry(contentWidth, contentHeight);
  }

  void View::refreshConfigChrome() {
    setBorderFocused(false);
    updateBorderGeometry();
    applyCornerRadius();
    applyDynamicRules();
    updateShadow();
    reloadBackdropColor();
  }

  void View::beginCloseAnimation() {
    if (!m_mapped
        || !m_onActiveWorkspace
        || m_server->sessionLocked()
        || (m_server->overview() != nullptr && m_server->overview()->active())) {
      return;
    }

    wlr_scene_tree* snap = wlr_scene_tree_create(m_server->xdgTree());
    if (snap == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&snap->node, m_sceneTree->node.x, m_sceneTree->node.y);

    // Collect border rects for the snapshot.
    std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>> snapRects;

    m_decoration.snapshotBorders(snap, m_borderFocusedState, snapRects);

    // Copy surface buffers.
    struct CopyCtx {
      wlr_scene_tree* snap;
      int rootX;
      int rootY;
      int buffersCopied;
    };
    CopyCtx ctx{snap, m_sceneTree->node.x, m_sceneTree->node.y, 0};
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* src, int sx, int sy, void* data) {
          auto* c = static_cast<CopyCtx*>(data);
          if (src->buffer == nullptr || !src->node.enabled) {
            return;
          }
          wlr_scene_buffer* copy = wlr_scene_buffer_create(c->snap, src->buffer);
          if (copy == nullptr) {
            return;
          }
          wlr_scene_node_set_position(&copy->node, sx - c->rootX, sy - c->rootY);
          if (src->dst_width > 0 && src->dst_height > 0) {
            wlr_scene_buffer_set_dest_size(copy, src->dst_width, src->dst_height);
          }
          if (src->src_box.width > 0 && src->src_box.height > 0) {
            wlr_scene_buffer_set_source_box(copy, &src->src_box);
          }
          wlr_scene_buffer_set_transform(copy, src->transform);
          wlr_scene_buffer_set_corner_radii(copy, src->corners);
          wlr_scene_buffer_set_opacity(copy, src->opacity);
          ++c->buffersCopied;
        },
        &ctx
    );

    if (ctx.buffersCopied == 0) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }

    Output* output =
        m_workspace != nullptr && m_workspace->group() != nullptr ? m_workspace->group()->output() : nullptr;
    if (output == nullptr) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }
    m_server->animateCloseSnapshot(output, snap, std::move(snapRects));
    wlr_output_schedule_frame(output->wlr());
  }

  void View::setSurfaceTreeClip(const wlr_box* clip) {
    // Clip only the toplevel subsurface tree. Calling set_clip on the xdg root also stamps that clip onto popup
    // children (wrong coords → cut-off menus), and clearing clip on border/popup trees asserts when they have no
    // subsurface tree.
    if (wlr_scene_node* surfaceNode = toplevelSurfaceTreeNode(m_sceneTree, m_toplevel->base->surface)) {
      wlr_scene_subsurface_tree_set_clip(surfaceNode, clip);
    } else if (clip == nullptr) {
      wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, nullptr);
    }
    // A clip change runs wlroots' scene surface reconfigure, which resets the scene-buffer opacity (to the client
    // alpha, 1.0 without wp_alpha_modifier). This runs in the render path after the animation tick, so re-apply our
    // fade/rule opacity or the frame renders fully opaque (the fade then only survives on frames whose clip is
    // unchanged, seen as transparent flashes).
    applyEffectiveOpacity();
  }

  Output* View::currentOutput() const {
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      return m_workspace->group()->output();
    }
    return m_server->outputFromWlr(m_server->preferredOutput());
  }

  void View::notifyOutputScale() {
    Output* output = currentOutput();
    if (output == nullptr || m_toplevel == nullptr || m_toplevel->base == nullptr) {
      return;
    }
    wlr_xdg_surface_for_each_surface(m_toplevel->base, &Output::notifySurfaceScaleIter, output);
    wlr_xdg_surface_for_each_popup_surface(m_toplevel->base, &Output::notifySurfaceScaleIter, output);
  }

  void View::unconstrainPopup(wlr_xdg_popup* popup) {
    if (popup == nullptr || m_sceneTree == nullptr) {
      return;
    }
    Output* output = currentOutput();
    if (output == nullptr) {
      return;
    }

    wlr_box target = output->usableArea();
    if (target.width <= 0 || target.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &target);
    }
    if (target.width <= 0 || target.height <= 0) {
      return;
    }

    int lx = 0;
    int ly = 0;
    if (!wlr_scene_node_coords(&m_sceneTree->node, &lx, &ly)) {
      return;
    }

    // wlroots supports flip, slide, and resize adjustments from the client's xdg-positioner. For tiled views, constrain
    // horizontally to the window geometry, so a nested menu at the right edge flips or slides left even when the tile
    // itself is flush with the output edge. Vertically, use the output working area. Floating popups can use the whole
    // area. The box is in root toplevel surface coordinates. The xdg scene root is positioned at the window geometry,
    // not at the surface origin.
    const wlr_box& geometry = m_toplevel->base->geometry;
    const wlr_box box{
        .x = m_tiled ? geometry.x : target.x - lx + geometry.x,
        .y = target.y - ly + geometry.y,
        .width = m_tiled ? geometry.width : target.width,
        .height = target.height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
  }

  View* View::fromSurface(wlr_surface* surface) {
    wlr_surface* walk = surface;
    while (walk != nullptr) {
      if (wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(walk)) {
        auto* tree = static_cast<wlr_scene_tree*>(toplevel->base->data);
        if (tree == nullptr) {
          return nullptr;
        }
        SceneNode* node = sceneNodeFrom(tree->node.data);
        if (node == nullptr || node->kind != SceneNodeKind::View) {
          return nullptr;
        }
        return static_cast<View*>(node);
      }
      if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(walk)) {
        walk = popup->parent;
        continue;
      }
      break;
    }
    return nullptr;
  }

  void View::clearOutputClip() {
    // Fullscreen must not keep a copied tile clip (that freezes usable-area size and leaves a bar-sized gap). Use
    // scheduled (not current): on leave, scheduled clears immediately while current lags until the client acks.
    const bool fullscreen = m_toplevel->scheduled.fullscreen;
    const wlr_box& geometry = m_toplevel->base->geometry;
    trackPresentedSize(geometry.width, geometry.height);
    if (!fullscreen && !m_tiled) {
      syncFloatingSurfaceClip();
      applyCornerRadius();
      updateBorderGeometry();
      return;
    }
    const wlr_box* clip = (!fullscreen && m_tiled) ? &m_toplevel->base->geometry : nullptr;
    setSurfaceTreeClip(clip);
    applyCornerRadius();
    updateBorderGeometry();
    m_decoration.clearShadowOutputClip();
    updateBlur();
    updateShadow();
  }

  void View::requestFloatingSize(int width, int height) {
    m_floating.recordSizeRequest(wlr_xdg_toplevel_set_size(m_toplevel, width, height));
  }
  void View::beginFloatingResize(uint32_t edges) {
    const wlr_box& geo = m_toplevel->base->geometry;
    m_floating.beginResize(
        {.x = m_sceneTree->node.x + geo.x, .y = m_sceneTree->node.y + geo.y, .width = geo.width, .height = geo.height},
        edges
    );
    syncFloatingResizePosition();
  }

  void View::resizeFloating(int width, int height) {
    syncFloatingResizePosition();
    requestFloatingSize(width, height);
  }

  void View::finishFloatingResize() { m_floating.endResize(); }

  void View::syncFloatingResizePosition() {
    if (!m_floating.anchor()) {
      return;
    }
    const wlr_box& geo = m_toplevel->base->geometry;
    const FloatingPoint content = anchoredContentOrigin(*m_floating.anchor(), m_floating.edges(), geo);
    setPosition(content.x - geo.x, content.y - geo.y);
  }

  void View::adoptFloatingClientSize() {
    if (m_tiled || !m_mapped || m_toplevel->scheduled.fullscreen || m_toplevel->scheduled.maximized) {
      return;
    }
    wlr_xdg_surface* base = m_toplevel->base;
    if (!m_floating.retireSizeRequestIfSettled(base->current.configure_serial)) {
      return;
    }
    const wlr_box& geo = base->geometry;
    if (geo.width <= 0 || geo.height <= 0) {
      return;
    }
    if (m_toplevel->scheduled.width != geo.width || m_toplevel->scheduled.height != geo.height) {
      // Once the latest compositor size request is committed, a floating
      // client owns its size. Direct assignment avoids an echo configure.
      m_toplevel->scheduled.width = geo.width;
      m_toplevel->scheduled.height = geo.height;
      clampFloatingPosition();
    }
  }

  void View::syncFloatingSurfaceClip() {
    if (m_tiled || m_toplevel->scheduled.fullscreen) {
      return;
    }
    const wlr_box& geo = m_toplevel->base->geometry;
    int width = m_toplevel->scheduled.width;
    int height = m_toplevel->scheduled.height;
    if (width <= 0) {
      width = m_toplevel->current.width;
    }
    if (height <= 0) {
      height = m_toplevel->current.height;
    }
    // Electron often keeps a wide buffer while tiled; without a clip, toggling float
    // would suddenly show the full surface.
    if (width > 0 && height > 0 && (geo.width > width || geo.height > height)) {
      const wlr_box clip{geo.x, geo.y, std::min(geo.width, width), std::min(geo.height, height)};
      setSurfaceTreeClip(&clip);
    } else {
      setSurfaceTreeClip(nullptr);
    }
    m_decoration.clearShadowOutputClip();
    updateBlur();
    updateShadow();
  }

  wlr_scene_tree* View::homeTree() const {
    const bool fs = m_toplevel->scheduled.fullscreen;
    if (m_workspace != nullptr) {
      return fs ? m_workspace->fullscreenTree() : m_workspace->viewLayer(m_tiled);
    }
    return fs ? m_server->fullscreenTree() : m_server->xdgTree();
  }

  void View::applyFullscreenLayout(bool animate) {
    Output* output = nullptr;
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      output = m_workspace->group()->output();
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    wlr_output* wlrOutput = output != nullptr ? output->wlr() : m_server->preferredOutput();
    wlr_box fullArea{};
    wlr_output_layout_get_box(m_server->outputLayout(), wlrOutput, &fullArea);
    if (fullArea.width <= 0 || fullArea.height <= 0) {
      return;
    }
    if (m_toplevel->scheduled.width != fullArea.width || m_toplevel->scheduled.height != fullArea.height) {
      wlr_xdg_toplevel_set_size(m_toplevel, fullArea.width, fullArea.height);
    }
    if (animate) {
      beginResizeAnimation(fullArea.width, fullArea.height, true);
      animateTo(fullArea.x, fullArea.y);
    } else if (!m_posX.animating() && !m_posY.animating()) {
      clipForSnapMove(fullArea.x, fullArea.y);
      setPosition(fullArea.x, fullArea.y);
    }

    const wlr_box target{
        m_sceneTree->node.x,
        m_sceneTree->node.y,
        fullArea.width,
        fullArea.height,
    };
    wlr_box intersection{};
    if (wlr_box_intersection(&intersection, &target, &fullArea)) {
      setOutputClip(&intersection, target, fullArea);
    } else {
      setOutputClip(nullptr, target, fullArea);
    }
  }

  void View::setOutputClip(const wlr_box* screenIntersection, const wlr_box& target, const wlr_box& outputBox) {
    updateFullscreenPresentation(target.width, target.height);
    if (m_toplevel->current.fullscreen && screenIntersection != nullptr) {
      m_presentation.setBackdropBox(
          screenIntersection->x - target.x, screenIntersection->y - target.y, screenIntersection->width,
          screenIntersection->height
      );
    }
    const wlr_box& geometry = m_toplevel->base->geometry;
    // Stay inside the tile while geometry lags configure (Electron often stays wide).
    const wlr_box content{
        .x = target.x,
        .y = target.y,
        .width = presentedWidth(target),
        .height = presentedHeight(target),
    };
    trackPresentedSize(content.width, content.height);
    const int border = borderInset();
    wlr_box decorated = content;
    decorated.x -= border;
    decorated.y -= border;
    decorated.width += 2 * border;
    decorated.height += 2 * border;
    wlr_box decoratedVisible{};
    if (screenIntersection == nullptr
        || !wlr_box_intersection(&decoratedVisible, &decorated, &outputBox)
        || decoratedVisible.width <= 0
        || decoratedVisible.height <= 0) {
      setNodeEnabled(false);
      return;
    }

    wlr_box contentVisible{};
    const bool contentOnOutput = wlr_box_intersection(&contentVisible, &content, &outputBox);
    const bool decoratedFullyVisible = wlr_box_equal(&decoratedVisible, &decorated);
    if (contentOnOutput) {
      const wlr_box surfaceClip =
          surfaceClipForOutput(geometry, content, contentVisible, m_presentation.offsetX(), m_presentation.offsetY());
      // Crop the toplevel surface to the visible tile; popup children are unclipped in
      // setSurfaceTreeClip so context menus can extend past the window edge.
      setSurfaceTreeClip(&surfaceClip);
      applyCornerRadii(cornerRadiiForVisible(content, contentVisible, corner_radii_all(surfaceRadius())));
      if (sizeAnimating() || sizeGrabActive()) {
        // The clip crops 1:1 in surface coordinates and caps the destination at the committed surface size, so it
        // cannot express an animated or interactive presented size. Program the buffer directly; the clip above keeps
        // the buffer node positioned at the visible box origin.
        applyPresentedCrop(content, surfaceClip);
      }
    } else {
      // Only the border/decoration remains on this output. Hide the surface with a non-empty clip placed outside the
      // surface box: wlroots treats an empty clip box as "remove the clip" and would re-enable the full-size buffer,
      // flashing the surface onto the neighbor output for a frame (end of a workspace slide). A non-intersecting clip
      // instead disables the buffer node.
      constexpr int kFarAway = 1 << 20;
      const wlr_box offSurface{-kFarAway, -kFarAway, 1, 1};
      setSurfaceTreeClip(&offSurface);
    }
    if (decoratedFullyVisible) {
      updateBorderGeometry(content.width, content.height);
    } else {
      m_decoration.clipBorders(target, outputBox, content.width, content.height);
    }
    m_decoration.setShadowOutputClip(outputBox);
    updateShadow();

    const wlr_box nodeBox{0, 0, content.width, content.height};
    if (!contentOnOutput) {
      m_decoration.hideBlur();
      return;
    }

    const int bleed = std::max(0, config().appearance.blur.radius) * std::max(1, config().appearance.blur.passes);
    const wlr_box blurClip = blurClipForOutput(nodeBox, contentVisible, outputBox, target, bleed);
    if (blurClip.width <= 0 || blurClip.height <= 0) {
      m_decoration.hideBlur();
      return;
    }
    m_decoration.updateBlur(
        m_sceneTree, m_toplevel->base->surface, nodeBox, geometry, surfaceRadius(), &blurClip, effectiveOpacity()
    );
  }

  void View::handleMap() {
    m_mapped = true;
    m_server->scheduleIpcWindowsEvent();
    m_tiled = looksTiled(m_toplevel);
    const wlr_box& mapGeo = m_toplevel->base->geometry;
    m_presentation.setSize(mapGeo.width, mapGeo.height);
    clearOutputClip();

    // Resolve window rules and apply one-shot effects. Copied, not referenced: the calls below can reach
    // setBorderFocused and re-resolve into the same cache slot, which would change this value underneath the code still
    // using it.
    const ResolvedWindowRule rule = resolvedRules();
    m_initialRules = rule;
    if (rule.defaultFloating) {
      m_tiled = !*rule.defaultFloating;
    }
    // Unsettled when any rule uses a title pattern: the first handleSetTitle after map re-applies disruptive effects
    // with the real title, even if the client mapped with a placeholder.
    m_initialRulesSettled = !anyWindowRuleHasTitlePattern(config());

    showDecorations(!m_toplevel->current.fullscreen);

    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this, rule.defaultWidth);
    } else {
      Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
      WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;
      WorkspaceGroup* targetGroup = windowRuleWorkspaceGroup(*m_server, rule, preferredGroup);
      if (Workspace* target = windowRuleWorkspace(targetGroup, rule)) {
        setWorkspace(target, /*attachToLayout=*/false);
        target->layoutAttach(this, rule.defaultWidth);
      } else {
        setOnActiveWorkspace(true);
      }
    }
    if (rule.defaultPinned && *rule.defaultPinned) {
      setPinned(true, false);
    }
    if (!m_tiled) {
      // The initial commit already applied default_size. Re-requesting it here
      // races the client's first content-driven resize.
      placeInUsableArea(rule.defaultPosition);
      // Enable + clip the float against its home output now that per-output
      // visibility is resolved data-side (no per-render-pass pass to do it).
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
      }
    }

    // Apply rule opacity: flush to scene buffers immediately so views on
    // inactive workspaces get the correct opacity when they become visible.
    if (rule.opacity) {
      m_ruleOpacity = static_cast<float>(*rule.opacity);
      setFadeAlpha(m_fadeAlpha);
    }

    updateForeignIdentity();
    updateForeignState();
    if (m_onActiveWorkspace && !m_server->sessionLocked() && rule.defaultFocused.value_or(true)) {
      m_server->focusView(this);
    }
    if (m_onActiveWorkspace) {
      setFadeAlpha(0.0F);
      m_fade.snap(0.0);
      m_fade.retarget(1.0, std::max(1, config().appearance.animationMs / 2));
      scheduleFrame();
    }

    // Tiled maximize is compositor-owned so client restore-state churn cannot
    // resize a column. Window rules and floating client requests still apply.
    const bool ruleMaximized = rule.defaultMaximize && *rule.defaultMaximize;
    if (ruleMaximized || (!m_tiled && m_toplevel->requested.maximized)) {
      setMaximized(true);
    } else if (m_tiled && m_toplevel->requested.maximized) {
      setMaximizedToEdges(true);
    }

    // Fullscreen after workspace + focus so the view lands in the right place.
    if (rule.defaultFullscreen && *rule.defaultFullscreen) {
      setFullscreen(true);
    }

    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewMapped(this);
    }
  }

  void View::handleUnmap() {
    setUrgent(false);
    m_maximizedToEdges = false;
    m_hasFullscreenRestoreBox = false;
    if (m_pinned) {
      m_pinned = false;
      m_restoreTiledAfterUnpin = false;
      if (m_workspace != nullptr) {
        wlr_scene_node_reparent(&m_sceneTree->node, m_workspace->viewLayer(false));
        reparentShadow(m_workspace->shadowLayer());
        setOnActiveWorkspace(m_workspace->active());
      }
    }
    if (m_server->scratchpadManager() != nullptr) {
      m_server->scratchpadManager()->remove(this);
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewUnmapped(this);
    }
    beginCloseAnimation();
    cancelFadeAnimation();
    cancelSizeAnimation();
    cancelPositionAnimation();
    m_decoration.setBordersEnabled(false);
    m_decoration.hideEffects();
    m_presentation.setBackdropEnabled(false);
    if (m_toplevel->current.fullscreen || m_toplevel->scheduled.fullscreen) {
      // Move out of the fullscreen layer back to the normal workspace/xdg tree.
      wlr_scene_node_reparent(&m_sceneTree->node, m_workspace ? m_workspace->viewLayer(m_tiled) : m_server->xdgTree());
    }
    m_mapped = false;
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      m_workspace->group()->output()->updateVrr();
    }
    m_server->scheduleIpcWindowsEvent();
    m_positioned = false;
    if (m_workspace != nullptr) {
      m_workspace->layoutDetach(this, m_workspace->scrollingLayout() != nullptr);
    }
    leaveForeignOutput();
    setForeignActivated(false);
    if (!m_server->cursor()->isPassthrough()) {
      m_server->cursor()->resetMode();
    }
    m_initialRulesSettled = false;
    m_initialRules = {};
    m_ruleOpacity = 1.0F;
    m_hasMaximizeRestoreBox = false;
    m_floating.clearSizeRequest();
  }

  void View::handleCommit() {
    if (m_toplevel->base->initial_commit) {
      // Resolve window rules early to influence initial tiled/float decision and size.
      const ResolvedWindowRule rule = resolvedRules();
      const bool wantTiled = rule.defaultFloating ? !*rule.defaultFloating : looksTiled(m_toplevel);

      if (wantTiled) {
        wlr_xdg_toplevel_set_tiled(m_toplevel, WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
        const wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);

        // Resolve the workspace this view will attach to, so the layout that
        // will actually arrange it is the one that sizes the first configure.
        Workspace* target = m_workspace;
        if (target == nullptr) {
          if (Output* out = m_server->outputFromWlr(m_server->preferredOutput())) {
            if (WorkspaceGroup* group = out->workspaceGroup()) {
              target = group->active();
              if (rule.defaultWorkspace) {
                if (Workspace* ruleTarget =
                        group->workspaceAtClamped(static_cast<size_t>(*rule.defaultWorkspace - 1))) {
                  target = ruleTarget;
                }
              }
            }
          }
        }

        // No workspace yet (no output, or none active): fall back to a throwaway layout built from the global config,
        // so the sizing rule stays the layout's either way.
        const ResolvedLayoutConfig globalConfig =
            target != nullptr ? ResolvedLayoutConfig{} : resolveGlobalLayout(config());
        std::unique_ptr<Layout> fallbackLayout;
        if (target == nullptr) {
          fallbackLayout = createLayout(globalConfig.mode);
          fallbackLayout->setConfig(&globalConfig);
        }
        const Layout& layout = target != nullptr ? target->layout() : *fallbackLayout;

        const Layout::InitialSize initial = layout.initialSize(usable, rule.defaultWidth);
        const int width = rule.defaultSize ? (*rule.defaultSize)[0] : initial.width;
        wlr_xdg_toplevel_set_size(m_toplevel, width, initial.height);
      } else {
        wlr_xdg_toplevel_set_tiled(m_toplevel, 0);
        const XdgSizeHints hints = xdgSizeHints(m_toplevel);
        if (rule.defaultSize) {
          requestFloatingSize(
              clampXdgWidth((*rule.defaultSize)[0], hints), clampXdgHeight((*rule.defaultSize)[1], hints)
          );
        } else {
          requestFloatingSize(0, 0);
        }
      }
    }
    if (!sizeAnimating()) {
      const wlr_box& geometry = m_toplevel->base->geometry;
      if (m_decoration.borderGeometryStale(geometry.width, geometry.height)) {
        updateBorderGeometry();
      }
    }
    applyCornerRadius();
    // Layout-assigned size changes start their presentation animation in Workspace::arrange. Client commits are not
    // resize requests: Chromium can change its geometry while keeping the same configure, and retargeting to that
    // geometry lets a tiled surface escape its assigned box.
    if (m_mapped
        && m_tiled
        && m_onActiveWorkspace
        && m_workspace != nullptr
        && !m_toplevel->scheduled.fullscreen
        && !m_toplevel->current.fullscreen
        && sizeGrabActive()) {
      // During interactive resize, track geometry so no spurious animation
      // replays the drag when the grab ends and mode returns to Passthrough.
      const wlr_box& geometry = m_toplevel->base->geometry;
      if (geometry.width > 0 && geometry.height > 0) {
        if (sizeAnimating()) {
          cancelSizeAnimation();
        }
        m_presentation.setSize(geometry.width, geometry.height);
      }
    }
    // The client committed a geometry other than its fullscreen one while the unfullscreen grace ran: it accepted
    // windowed mode, so the layout may assign the column size now instead of waiting out the grace.
    if (m_pendingUnfullscreenSize
        && !m_toplevel->current.fullscreen
        && !wlr_box_equal(&m_toplevel->base->geometry, &m_unfullscreenGeometry)) {
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
      if (m_mapped && m_tiled && m_workspace != nullptr) {
        m_workspace->markArrange(true);
      }
    }
    // Re-apply output clip after configure ack so Super+F / resize sizes show
    // without needing a workspace switch (clip boxes are copied, not live).
    if (m_mapped && m_tiled && m_workspace != nullptr && m_workspace->active()) {
      m_workspace->syncViewPresentation(this);
    } else if (m_mapped && !m_tiled) {
      if (m_toplevel->scheduled.fullscreen && m_onActiveWorkspace) {
        // Keep fullscreen placement authoritative; the xdg scene helper just
        // reset the surface offset for this commit.
        applyFullscreenLayout();
      } else {
        syncFloatingResizePosition();
        adoptFloatingClientSize();
        if (!sizeAnimating()) {
          syncFloatingSurfaceClip();
        }
        // Enable + clip to the home output (previously done per render pass).
        if (m_workspace != nullptr) {
          m_workspace->syncViewPresentation(this);
        }
      }
    } else {
      updateBlur();
      updateShadow();
    }
    // Workspace presentation deliberately skips a dragged view so it remains unclipped across outputs. A client commit
    // resets scene-buffer opacity; defer restoration to the frame tick so every commit listener has run.
    if (m_dragOpacity < 1.0F) {
      m_dragOpacityCommitPending = true;
    }
    updateForeignState();
  }

  void View::handleDestroy() {
    cancelFadeAnimation();
    cancelPositionAnimation();
    leaveForeignOutput();
    if (m_foreign != nullptr) {
      wl_list_remove(&m_foreignActivate.link);
      wl_list_remove(&m_foreignClose.link);
      wl_list_remove(&m_foreignDestroy.link);
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }
    if (m_extForeign != nullptr) {
      wl_list_remove(&m_extForeignDestroy.link);
      wlr_ext_foreign_toplevel_handle_v1_destroy(m_extForeign);
      m_extForeign = nullptr;
    }
    if (m_captureSource != nullptr) {
      wl_list_remove(&m_captureSourceDestroy.link);
      m_captureSourceDestroy.link.next = nullptr;
      m_captureSource = nullptr;
    }

    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_requestMove.link);
    wl_list_remove(&m_requestResize.link);
    wl_list_remove(&m_requestMaximize.link);
    wl_list_remove(&m_requestFullscreen.link);
    wl_list_remove(&m_setTitle.link);
    wl_list_remove(&m_setAppId.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_commit.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_requestMove.link.next = nullptr;
    m_requestResize.link.next = nullptr;
    m_requestMaximize.link.next = nullptr;
    m_requestFullscreen.link.next = nullptr;
    m_setTitle.link.next = nullptr;
    m_setAppId.link.next = nullptr;
    m_sceneTree->node.data = nullptr;
    m_toplevel->base->data = nullptr;
    m_server->removeView(this);
  }

  void View::handleRequestMove() { m_server->cursor()->beginMove(this); }

  void View::handleRequestResize(void* data) {
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server->cursor()->beginResize(this, event->edges);
  }

  void View::setMaximized(bool maximized) {
    if (m_tiled && m_workspace != nullptr) {
      if (m_maximizedToEdges) {
        setMaximizedToEdges(false);
      }
      const int column = m_workspace->layout().columnOf(this);
      if (column >= 0 && m_workspace->layout().isFullWidth(column) != maximized) {
        m_workspace->layout().toggleFullWidth(column);
      }
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
      if (maximized) {
        m_workspace->ensureFocusedVisible();
      }
      m_workspace->markArrange(false);
      updateForeignState();
      return;
    }

    const bool wasMaximized = m_toplevel->scheduled.maximized;
    if (maximized && !wasMaximized) {
      m_floating.clearSizeRequest();
      const wlr_box& geometry = m_toplevel->base->geometry;
      m_maximizeRestoreBox = {
          .x = m_sceneTree->node.x,
          .y = m_sceneTree->node.y,
          .width = geometry.width,
          .height = geometry.height,
      };
      m_hasMaximizeRestoreBox = geometry.width > 0 && geometry.height > 0;

      const wlr_box usable = floatingUsableArea();
      if (usable.width > 0 && usable.height > 0) {
        wlr_xdg_toplevel_set_size(m_toplevel, usable.width, usable.height);
        wlr_scene_node_set_position(&m_sceneTree->node, usable.x, usable.y);
        m_decoration.setShadowPosition(usable.x, usable.y);
      }
    } else if (!maximized && wasMaximized && m_hasMaximizeRestoreBox) {
      requestFloatingSize(m_maximizeRestoreBox.width, m_maximizeRestoreBox.height);
      wlr_scene_node_set_position(&m_sceneTree->node, m_maximizeRestoreBox.x, m_maximizeRestoreBox.y);
      m_decoration.setShadowPosition(m_maximizeRestoreBox.x, m_maximizeRestoreBox.y);
      m_hasMaximizeRestoreBox = false;
    }
    wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
    syncFloatingSurfaceClip();
    updateForeignState();
  }

  void View::handleRequestMaximize() {
    if (!m_toplevel->base->initialized || !m_mapped) {
      return;
    }
    if (m_tiled && m_workspace != nullptr) {
      const int column = m_workspace->layout().columnOf(this);
      const bool columnFullWidth = column >= 0 && m_workspace->layout().isFullWidth(column);
      if (maximizeRequestTargetsEdges(m_maximizedToEdges, columnFullWidth, m_toplevel->requested.maximized)) {
        setMaximizedToEdges(m_toplevel->requested.maximized);
        return;
      }
      wlr_xdg_toplevel_set_maximized(m_toplevel, columnFullWidth);
      updateForeignState();
      return;
    }
    setMaximized(m_toplevel->requested.maximized);
  }

  void View::setMaximizedToEdges(bool maximized) {
    if (!m_toplevel->base->initialized || maximized == m_maximizedToEdges) {
      return;
    }
    if (maximized && m_toplevel->scheduled.fullscreen) {
      setFullscreen(false);
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
    }
    cancelSizeAnimation();
    if (m_tiled && m_workspace != nullptr && maximized) {
      const int column = m_workspace->layout().columnOf(this);
      if (column >= 0 && m_workspace->layout().isFullWidth(column)) {
        m_workspace->layout().clearFullWidthState(column);
      }
    }

    m_maximizedToEdges = maximized;
    if (m_tiled) {
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
    } else {
      setMaximized(maximized);
    }
    showDecorations(!maximized && !m_toplevel->scheduled.fullscreen);
    if (m_workspace != nullptr) {
      if (maximized) {
        m_workspace->ensureFocusedVisible();
      }
      m_workspace->markArrange(false);
    }
    updateForeignState();
  }

  void View::toggleMaximizedToEdges() { setMaximizedToEdges(!m_maximizedToEdges); }

  void View::handleRequestFullscreen() {
    if (!m_toplevel->base->initialized) {
      return;
    }
    kLog.debug(
        "request_fullscreen '{}' [{}]: {}", m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?",
        static_cast<const void*>(this), m_toplevel->requested.fullscreen
    );

    const bool requested = m_toplevel->requested.fullscreen;

    // Redundant request (wine spams set_fullscreen while already fullscreen): ack with a configure, but skip the
    // reparent/scroll-snap/arrange churn that a full setFullscreen() would run: that churn is visible flicker.
    if (requested == m_toplevel->scheduled.fullscreen) {
      wlr_xdg_surface_schedule_configure(m_toplevel->base);
      return;
    }

    // Wine unfullscreens games when they lose focus (minimize-on-focus-loss). Honoring that rips the game out of the
    // fullscreen strip the moment the user scrolls away. Deny unfullscreen from deactivated windows; the scheduled
    // configure re-asserts the fullscreen state (spec-compliant).
    if (!requested && !m_toplevel->scheduled.activated) {
      kLog.debug(
          "request_fullscreen denied for deactivated '{}'", m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?"
      );
      wlr_xdg_surface_schedule_configure(m_toplevel->base);
      return;
    }

    // Honor the client's requested state (not a blind toggle).
    setFullscreen(requested);
    if (!requested && m_pendingUnfullscreenSize) {
      // The client itself asked for windowed mode: no compliance grace is
      // needed, size it into its column right away.
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
      if (m_tiled && m_workspace != nullptr) {
        m_workspace->markArrange(true);
      }
    }
  }

  void View::toggleFullscreen() {
    if (!m_toplevel->base->initialized) {
      return;
    }
    setFullscreen(!m_toplevel->scheduled.fullscreen);
  }

  void View::toggleFloating() { setFloating(m_tiled); }

  void View::restorePinnedSceneParent() {
    if (!m_pinned) {
      return;
    }
    wlr_scene_node_place_above(&m_server->pinnedShadowTree()->node, &m_server->fullscreenTree()->node);
    wlr_scene_node_place_above(&m_server->pinnedTree()->node, &m_server->pinnedShadowTree()->node);
    wlr_scene_node_reparent(&m_sceneTree->node, m_server->pinnedTree());
    reparentShadow(m_server->pinnedShadowTree());
    setNodeEnabled(true);
    raiseToTop();
  }

  void View::togglePinned() { setPinned(!m_pinned, true); }

  void View::setPinned(bool pinned, bool focus) {
    if (!m_mapped
        || !m_toplevel->base->initialized
        || (pinned && (m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen))
        || pinned == m_pinned) {
      return;
    }
    if (pinned) {
      m_restoreTiledAfterUnpin = m_tiled;
      if (m_tiled) {
        setFloating(true, false);
      }
      m_pinned = true;
      restorePinnedSceneParent();
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
      }
      if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
        wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
      }
      if (focus) {
        m_server->focusView(this);
      }
      return;
    }

    const bool restoreTiled = m_restoreTiledAfterUnpin;
    m_restoreTiledAfterUnpin = false;
    m_pinned = false;
    if (restoreTiled) {
      setFloating(false, focus);
      return;
    }
    if (m_workspace != nullptr) {
      wlr_scene_node_reparent(&m_sceneTree->node, m_workspace->viewLayer(false));
      reparentShadow(m_workspace->shadowLayer());
      setOnActiveWorkspace(m_workspace->active());
      m_workspace->syncFloatingStack(this);
      m_workspace->syncViewPresentation(this);
    }
    setNodeEnabled(m_onActiveWorkspace);
  }

  void View::setFloating(bool floating, bool focus) {
    if (!m_mapped || !m_toplevel->base->initialized) {
      return;
    }
    kLog.debug(
        "set_floating '{}' [{}] -> {} (tiled={}, pinned={}, fs={})",
        m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?", static_cast<const void*>(this), floating, m_tiled,
        m_pinned, m_toplevel->scheduled.fullscreen
    );
    if (!floating && m_server->scratchpadManager() != nullptr && m_server->scratchpadManager()->contains(this)) {
      return;
    }
    // A no-op request must stay a no-op: unfullscreening or cancelling the size animation here would let a redundant
    // "make tiled" call rip a fullscreen game out of its state (the game re-requests, the compositor re-grants, and
    // every cycle reflows the strip).
    const bool wantTiled = !floating;
    if (m_tiled == wantTiled) {
      return;
    }
    if (m_maximizedToEdges) {
      setMaximizedToEdges(false);
    }
    if (!floating && m_pinned) {
      m_pinned = false;
      m_restoreTiledAfterUnpin = false;
      if (m_workspace != nullptr) {
        wlr_scene_node_reparent(&m_sceneTree->node, m_workspace->viewLayer(false));
        reparentShadow(m_workspace->shadowLayer());
        setOnActiveWorkspace(m_workspace->active());
      }
    }
    cancelSizeAnimation();
    const bool fullscreen = m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen;
    // Only the float direction leaves fullscreen (it owns its own scene tree). Re-tiling a fullscreen view keeps the
    // state and re-inserts it as a fullscreen column: dropping it first configures the client to a regular column size
    // for the instant before it re-requests fullscreen, and game engines latch that transient windowed size for their
    // input mapping, leaving hover and clicks dead outside it (X geometry recovers, the engine's notion does not).
    if (floating && fullscreen) {
      setFullscreen(false);
      // Remember to restore on the next re-tile. Set after setFullscreen,
      // which clears the flag on every leave-fullscreen path.
      m_refullscreenOnTile = true;
      // The float path requests its own size below; the tiled-column size
      // deferral set by setFullscreen(false) does not apply to floats.
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
    }
    // Consume the memory: a client that itself left fullscreen while floating cleared it (setFullscreen(false) below
    // via its request), so this only fires for a float episode the client still considers fullscreen.
    const bool refullscreen = !floating && !fullscreen && m_refullscreenOnTile;
    m_refullscreenOnTile = floating && m_refullscreenOnTile;

    if (floating) {
      int keepWidth = 0;
      int keepHeight = 0;
      if (m_floating.size()) {
        keepWidth = (*m_floating.size())[0];
        keepHeight = (*m_floating.size())[1];
      } else {
        // First-time floats prefer the last acked or scheduled configure size,
        // then fall back to the layout target and committed geometry.
        keepWidth = m_toplevel->current.width;
        keepHeight = m_toplevel->current.height;
        if (keepWidth <= 0 || keepHeight <= 0) {
          keepWidth = m_toplevel->scheduled.width;
          keepHeight = m_toplevel->scheduled.height;
        }
        if ((keepWidth <= 0 || keepHeight <= 0) && m_workspace != nullptr) {
          const wlr_box target = m_workspace->layout().targetBox(this);
          if (target.width > 0 && target.height > 0) {
            const XdgSizeHints hints = xdgSizeHints(m_toplevel);
            keepWidth = clampXdgWidth(target.width, hints);
            keepHeight = clampXdgHeight(target.height, hints);
          }
        }
        if (keepWidth <= 0 || keepHeight <= 0) {
          const wlr_box& geo = m_toplevel->base->geometry;
          keepWidth = geo.width;
          keepHeight = geo.height;
        }
      }
      if (m_workspace != nullptr) {
        const int column = m_workspace->layout().columnOf(this);
        if (column >= 0 && m_workspace->layout().isFullWidth(column)) {
          m_workspace->layout().clearFullWidthState(column);
          wlr_xdg_toplevel_set_maximized(m_toplevel, false);
        }
        m_workspace->layoutDetach(this);
      }
      const int keepX = m_sceneTree->node.x;
      const int keepY = m_sceneTree->node.y;
      m_tiled = false;
      if (m_workspace != nullptr) {
        wlr_scene_node_reparent(&m_sceneTree->node, m_workspace->viewLayer(m_tiled));
        m_workspace->syncFloatingStack(this);
      }
      // Do not clear xdg tiled edges: GTK/Qt often resize (CSD / preferred size) when
      // tiled state is dropped. Floating is a compositor layout concern.
      if (keepWidth > 0
          && keepHeight > 0
          && (m_toplevel->scheduled.width != keepWidth || m_toplevel->scheduled.height != keepHeight)) {
        requestFloatingSize(keepWidth, keepHeight);
      }
      beginResizeAnimation(keepWidth, keepHeight);
      const wlr_box usable = floatingUsableArea();
      int floatX = keepX + 50;
      int floatY = keepY + 50;
      if (const auto restored = m_floating.restoredOrigin(usable)) {
        floatX = restored->x;
        floatY = restored->y;
      }
      if (usable.width > 0 && usable.height > 0 && keepWidth > 0 && keepHeight > 0) {
        const int decoration = config().appearance.totalBorderWidth();
        const int minX = usable.x + decoration;
        const int minY = usable.y + decoration;
        const int maxX = usable.x + usable.width - decoration - keepWidth;
        const int maxY = usable.y + usable.height - decoration - keepHeight;
        floatX = std::clamp(floatX, minX, std::max(minX, maxX));
        floatY = std::clamp(floatY, minY, std::max(minY, maxY));
        m_floating.rememberPositionFraction({.x = floatX, .y = floatY}, usable);
      }
      animateTo(floatX, floatY);
      syncFloatingSurfaceClip();
      // Keep the focus ring when floating a tiled window.
      showDecorations(true);
      if (focus) {
        m_server->focusView(this);
      }
      updateForeignState();
      return;
    }

    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = m_toplevel->base->geometry;
    // A fullscreen geometry is not a floating size; remembering it would make
    // the next float episode restore output-sized dimensions.
    if (!fullscreen && geo.width > 0 && geo.height > 0) {
      m_floating.rememberSize(geo.width, geo.height);
    }
    m_floating.rememberPositionFraction({.x = m_sceneTree->node.x, .y = m_sceneTree->node.y}, usable);

    m_floating.clearSizeRequest();
    m_tiled = true;
    // Restore the fullscreen the float toggle dropped BEFORE the layout attach: arrange then sizes the column to the
    // full output instead of a regular column width, and the client sees no transient windowed configure. setFullscreen
    // also reparents and disables borders.
    if (refullscreen) {
      setFullscreen(true);
    }
    const bool wantFullscreen = fullscreen || refullscreen;
    if (m_workspace != nullptr) {
      wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
      m_workspace->syncFloatingStack(this);
    }
    wlr_xdg_toplevel_set_tiled(m_toplevel, WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
    wlr_xdg_toplevel_set_maximized(m_toplevel, false);
    m_decoration.ensureBorders(m_sceneTree);
    m_decoration.setBordersEnabled(!wantFullscreen);
    updateBorderGeometry();
    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this);
    }
    applyCornerRadius();
    updateShadow();
    if (focus) {
      m_server->focusView(this);
    }
    // setFullscreen(true) is not re-run on this path, so its scroll snap does not happen; without it the strip can rest
    // showing the neighbor column beside a viewport-wide fullscreen column.
    if (wantFullscreen && m_workspace != nullptr) {
      m_workspace->ensureFocusedVisible();
      m_workspace->markArrange(false);
    }
    updateForeignState();
  }

  void View::setFullscreen(bool fullscreen) {
    kLog.debug(
        "set_fullscreen '{}' [{}] -> {} (tiled={}, ws_active={})",
        m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?", static_cast<const void*>(this), fullscreen, m_tiled,
        m_workspace != nullptr && m_workspace->active()
    );
    if (fullscreen && m_maximizedToEdges) {
      setMaximizedToEdges(false);
    }
    if (fullscreen && !m_tiled && !m_toplevel->current.fullscreen) {
      const wlr_box& geometry = m_toplevel->base->geometry;
      m_fullscreenRestoreBox = {
          .x = m_sceneTree->node.x,
          .y = m_sceneTree->node.y,
          .width = geometry.width,
          .height = geometry.height,
      };
      m_hasFullscreenRestoreBox = geometry.width > 0 && geometry.height > 0;
    }
    const bool restoreFloating = !fullscreen && !m_tiled && m_hasFullscreenRestoreBox;
    // Any leave-fullscreen invalidates a pending float-toggle restore: the
    // float path re-sets the flag right after its own setFullscreen(false).
    if (!fullscreen) {
      m_refullscreenOnTile = false;
    }
    // Leaving column maximize when entering real fullscreen avoids a stale
    // widthFrac=1.0 column after the client leaves fullscreen.
    if (fullscreen && m_tiled && m_workspace != nullptr) {
      const int column = m_workspace->layout().columnOf(this);
      if (m_workspace->layout().isFullWidth(column)) {
        m_workspace->layout().clearFullWidthState(column);
        wlr_xdg_toplevel_set_maximized(m_toplevel, false);
      }
    }
    if (fullscreen) {
      if (m_pinned) {
        m_pinned = false;
        m_restoreTiledAfterUnpin = false;
        if (m_workspace != nullptr) {
          wlr_scene_node_reparent(&m_sceneTree->node, m_workspace->viewLayer(false));
          reparentShadow(m_workspace->shadowLayer());
          setOnActiveWorkspace(m_workspace->active());
        }
      }
      m_floating.clearSizeRequest();
    }
    cancelSizeAnimation();
    wlr_xdg_toplevel_set_fullscreen(m_toplevel, fullscreen);
    updateFullscreenPresentation(0, 0);
    if (fullscreen) {
      // scheduled.fullscreen is set; reparent to fullscreen layer.
      wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
      wlr_scene_node_raise_to_top(&m_sceneTree->node);
      // Snap scroll to the now viewport-wide column and reflow neighbors.
      if (m_workspace != nullptr) {
        m_workspace->ensureFocusedVisible();
        // arrange() sends the full-output size even when this workspace is hidden.
        m_workspace->markArrange(true);
      }
      if (!m_tiled || m_workspace == nullptr) {
        // Floating fullscreen is not part of the layout; size it directly.
        applyFullscreenLayout(true);
      }
    } else {
      wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
      if (!m_tiled && m_workspace != nullptr) {
        m_workspace->restackFloatingViews();
      }
      if (restoreFloating) {
        requestFloatingSize(m_fullscreenRestoreBox.width, m_fullscreenRestoreBox.height);
        beginResizeAnimation(m_fullscreenRestoreBox.width, m_fullscreenRestoreBox.height, true);
        animateTo(m_fullscreenRestoreBox.x, m_fullscreenRestoreBox.y);
      } else if (m_tiled) {
        m_hasFullscreenRestoreBox = false;
      }
    }
    m_decoration.setBordersEnabled(!fullscreen);
    applyCornerRadius();
    updateShadow();
    if (!fullscreen) {
      // scheduled.fullscreen is already false; arrange into usable area (exclusive zones).
      if (m_tiled && m_workspace != nullptr) {
        // xwayland-satellite clients only: send the unfullscreen with size 0x0 (client picks) and withhold the column
        // size for a grace period. Game engines behind satellite latch a transient windowed resize for input mapping
        // and never recover (hover and clicks go dead outside it even after the geometry returns), and they only notice
        // the state change when a resize pokes them, so on expiry the grace re-asserts fullscreen instead of resizing.
        // Wayland-native clients handle resizes fine and commonly keep their size on 0x0, which would wrongly bounce
        // them back to fullscreen; they keep the immediate column sizing.
        if (m_xwayland) {
          m_pendingUnfullscreenSize = true;
          m_unfullscreenGraceStartMsec = 0;
          m_unfullscreenGeometry = m_toplevel->base->geometry;
          wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
          // The grace countdown runs on frame ticks; make sure one is coming.
          scheduleFrame();
        }
        m_workspace->markArrange(!m_xwayland);
      } else if (!restoreFloating) {
        placeInUsableArea();
      }
    } else {
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
    }
    updateForeignState();
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      m_workspace->group()->output()->updateVrr();
    }
  }

  void View::applyWindowRules(const ResolvedWindowRule& initiallyApplied) {
    if (!m_mapped) {
      return;
    }
    // Copied, not referenced: the calls below can reach setBorderFocused and re-resolve into the same cache slot, which
    // would change this value underneath the code still using it.
    const ResolvedWindowRule rule = resolvedRules();

    // Identity can arrive after map. Apply a newly selected one-shot value, but
    // never replay a value already applied at map over the user's later state.
    if (changedInitialRule(rule.defaultFloating, initiallyApplied.defaultFloating)) {
      const bool wantFloat = *rule.defaultFloating;
      if (wantFloat != !m_tiled) {
        setFloating(wantFloat);
      }
    }

    const bool placementChanged = (rule.defaultOutput.has_value() || rule.defaultWorkspace.has_value())
        && (rule.defaultOutput != initiallyApplied.defaultOutput
            || rule.defaultWorkspace != initiallyApplied.defaultWorkspace);
    if (placementChanged && m_workspace != nullptr) {
      WorkspaceGroup* targetGroup = windowRuleWorkspaceGroup(*m_server, rule, m_workspace->group());
      Workspace* target = windowRuleWorkspace(targetGroup, rule);
      if (target != nullptr && target != m_workspace) {
        setWorkspace(target);
      }
    }

    if (changedInitialRule(rule.defaultPinned, initiallyApplied.defaultPinned)) {
      setPinned(*rule.defaultPinned, false);
    }

    ScrollingLayout* scrolling = m_workspace != nullptr ? m_workspace->scrollingLayout() : nullptr;
    if (changedInitialRule(rule.defaultWidth, initiallyApplied.defaultWidth) && m_tiled && scrolling != nullptr) {
      const int column = scrolling->columnOf(this);
      if (column >= 0) {
        scrolling->setWidthFraction(column, *rule.defaultWidth);
        m_workspace->markArrange();
      }
    }

    if (changedInitialRule(rule.defaultSize, initiallyApplied.defaultSize) && !m_tiled) {
      const XdgSizeHints hints = xdgSizeHints(m_toplevel);
      requestFloatingSize(clampXdgWidth((*rule.defaultSize)[0], hints), clampXdgHeight((*rule.defaultSize)[1], hints));
      placeInUsableArea();
    }

    if (changedInitialRule(rule.defaultPosition, initiallyApplied.defaultPosition) && !m_tiled) {
      placeInUsableArea(rule.defaultPosition);
    }

    if (changedInitialRule(rule.defaultFullscreen, initiallyApplied.defaultFullscreen)
        && *rule.defaultFullscreen
        && !m_toplevel->scheduled.fullscreen) {
      setFullscreen(true);
    }

    if (changedInitialRule(rule.defaultMaximize, initiallyApplied.defaultMaximize)
        && *rule.defaultMaximize
        && !m_toplevel->scheduled.maximized) {
      setMaximized(true);
    }

    // Dynamic effects are always safe to update. Reuse the resolution above
    // rather than running every rule regex a second time.
    applyDynamicRules(&rule);
  }

  const ResolvedWindowRule& View::resolvedRules() {
    const char* appId = m_toplevel->app_id;
    const char* title = m_toplevel->title;
    const std::string_view appIdView = appId != nullptr ? appId : "";
    const std::string_view titleView = title != nullptr ? title : "";
    const uint64_t generation = configStore().generation();

    if (m_rulesGeneration == generation
        && m_rulesFocused == m_borderFocusedState
        && m_rulesAppId == appIdView
        && m_rulesTitle == titleView) {
      return m_rules;
    }

    m_rules = resolveWindowRules(config(), appId, title, m_borderFocusedState);
    m_rulesGeneration = generation;
    m_rulesFocused = m_borderFocusedState;
    m_rulesAppId = appIdView;
    m_rulesTitle = titleView;
    return m_rules;
  }

  void View::applyDynamicRules(const ResolvedWindowRule* resolved) {
    const ResolvedWindowRule& rule = resolved != nullptr ? *resolved : resolvedRules();
    m_decoration.applyRule(rule);
    const float newOpacity = rule.opacity ? static_cast<float>(*rule.opacity) : 1.0F;
    if (newOpacity != m_ruleOpacity) {
      m_ruleOpacity = newOpacity;
      setFadeAlpha(m_fadeAlpha); // refresh effective opacity
    }
    updateBlur();
    // updateBlur creates the full node box. Re-apply the owning output's clip immediately, because focus, title, and
    // app-id rule refreshes do not necessarily produce a later surface commit or layout pass.
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
    }
  }

} // namespace umbriel
