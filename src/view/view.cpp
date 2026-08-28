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

    // A scene-node capture uses the node's scene as its render source. Keep a
    // second scene containing only the client surfaces so transparency cannot
    // reveal the wallpaper, another window, or compositor effects.
    m_captureScene = wlr_scene_create();
    if (m_captureScene != nullptr) {
      m_captureScene->restack_xwayland_surfaces = false;
      wlr_scene_xdg_surface_create(&m_captureScene->tree, m_toplevel->base);
    }
    notifyOutputScale();

    m_commit.notify = onCommit;
    wl_signal_add(&m_toplevel->base->surface->events.commit, &m_commit);
    watchOpacitySurfaceTree(m_toplevel->base->surface);
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
    if (m_acceptClientMaximizeIdle != nullptr) {
      wl_event_source_remove(m_acceptClientMaximizeIdle);
      m_acceptClientMaximizeIdle = nullptr;
    }
    m_server->unregisterAnimatable(this);
    clearOpacitySurfaceWatches();
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
    if (m_captureScene != nullptr) {
      wlr_scene_node_destroy(&m_captureScene->tree.node);
      m_captureScene = nullptr;
    }
  }

  wlr_scene_tree* View::captureTree() const { return m_captureScene != nullptr ? &m_captureScene->tree : nullptr; }

  void View::moveToWorkspace(Workspace* workspace, bool attachToLayout) {
    const bool wasDisplaced = m_displacedHome.has_value();
    m_displacedHome.reset();
    setWorkspace(workspace, attachToLayout);
    if (wasDisplaced) {
      m_server->scheduleDisplacedViewRestore();
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
      previous->removeView(this, !sameGroup);
    }
    m_workspace = workspace;
    if (m_workspace != nullptr) {
      m_workspace->addView(this, attachToLayout);
    } else {
      // A pinned view normally hangs below output-owned clipping roots. Park both of its scene branches on the
      // server-owned pinned roots before the last output is destroyed, then addView() can rehome them when an output
      // returns. Leaving either branch under the dying output frees nodes that the live View still owns.
      if (m_pinned) {
        wlr_scene_node_reparent(&m_sceneTree->node, m_server->pinnedTree());
        reparentShadow(m_server->pinnedShadowTree());
      }
      setOnActiveWorkspace(true);
    }
    notifyOutputScale();
    if (m_mapped) {
      if (m_workspace != nullptr) {
        enterForeignOutput();
      } else {
        // An unassigned view has no output to advertise. In particular, the preferred output may be the one currently
        // being destroyed, and foreign-toplevel output membership installs a bind listener that must be gone before
        // wlr_output_finish completes.
        leaveForeignOutput();
      }
    }
    if (m_mapped) {
      m_server->scheduleIpcWindowsEvent();
      if (previousOutput != nullptr) {
        previousOutput->updateVrr();
        previousOutput->updateHdr();
      }
      if (m_workspace != nullptr && m_workspace->group() != nullptr) {
        Output* output = m_workspace->group()->output();
        output->updateVrr();
        output->updateHdr();
      }
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewWorkspaceChanged(this);
    }
  }

  bool View::attachToAvailableWorkspace(const ResolvedWindowRule& rule) {
    Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;
    WorkspaceGroup* targetGroup = windowRuleWorkspaceGroup(*m_server, rule, preferredGroup);
    Workspace* target = windowRuleWorkspace(targetGroup, rule);
    if (target == nullptr) {
      return false;
    }
    setWorkspace(target, false);
    if (m_workspace != target) {
      return false;
    }
    target->layoutAttach(this, rule.defaultWidth);
    return true;
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
    if (Output* output = currentOutput()) {
      output->updateHdr();
    }
  }

  void View::setNodeEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_sceneTree->node, enabled);
    m_decoration.setShadowEnabled(enabled);
    m_server->updateIdleInhibit();
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

    if (!withKeyboard) {
      return;
    }

    // Re-focusing the popup's owning toplevel must preserve its active XDG
    // keyboard grab. Ending that grab tells wlroots to dismiss the popup.
    if (seat->keyboard_state.focused_surface == surface) {
      return;
    }

    // A popup can still own wlroots' keyboard grab when its dismissing click
    // reaches another view. Let that click transfer the seat immediately
    // instead of losing the enter to a popup that is about to disappear. A
    // data-device drag is different: it deliberately owns the grab until the
    // initiating button is released, and FocusManager replays the selected
    // view when that happens.
    if (seat->drag == nullptr && wlr_seat_keyboard_has_grab(seat)) {
      wlr_seat_keyboard_end_grab(seat);
    }
    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
      wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
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
    // Overshooting curves can push this out of range; wlr_scene_buffer_set_opacity asserts opacity is in [0, 1].
    m_fadeAlpha = std::clamp(alpha, 0.0F, 1.0F);
    float effective = effectiveOpacity();
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &effective
    );
    setBorderFocused(m_borderFocusedState);
    m_decoration.setAlpha(effective, m_fadeAlpha);
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

  void View::flushPendingEffectiveOpacity() {
    if (!m_effectiveOpacityCommitPending) {
      return;
    }
    applyEffectiveOpacity();
    m_effectiveOpacityCommitPending = false;
  }

  void View::watchOpacitySurfaceTree(wlr_surface* root) {
    if (root == nullptr) {
      return;
    }
    wlr_surface_for_each_surface(
        root,
        [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* data) {
          static_cast<View*>(data)->watchOpacitySurface(surface);
        },
        this
    );
  }

  void View::watchOpacitySurface(wlr_surface* surface) {
    if (surface == nullptr || std::ranges::any_of(m_opacitySurfaceWatches, [surface](const auto& watch) {
          return watch->surface == surface;
        })) {
      return;
    }
    auto watch = std::make_unique<OpacitySurfaceWatch>();
    watch->view = this;
    watch->surface = surface;
    watch->commit.notify = onOpacitySurfaceCommit;
    wl_signal_add(&surface->events.commit, &watch->commit);
    watch->newSubsurface.notify = onOpacitySurfaceNewSubsurface;
    wl_signal_add(&surface->events.new_subsurface, &watch->newSubsurface);
    watch->destroy.notify = onOpacitySurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &watch->destroy);
    m_opacitySurfaceWatches.push_back(std::move(watch));
  }

  void View::clearOpacitySurfaceWatches() {
    for (const auto& watch : m_opacitySurfaceWatches) {
      wl_list_remove(&watch->commit.link);
      wl_list_remove(&watch->newSubsurface.link);
      wl_list_remove(&watch->destroy.link);
    }
    m_opacitySurfaceWatches.clear();
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
    // Buffer scale + crop is derived in applyPresentation (applyPresentedCrop) via syncViewPresentation below, so the
    // animated size and the presented crop are always applied together instead of fighting over dest_size.
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
    m_dragOpacity = config().appearance.dragOpacity;
    setFadeAlpha(m_fadeAlpha);
    m_effectiveOpacityCommitPending = false;
    if (m_pinned) {
      wlr_scene_node_place_above(&m_server->dragShadowTree()->node, &m_server->pinnedTree()->node);
      wlr_scene_node_place_above(&m_server->dragTree()->node, &m_server->dragShadowTree()->node);
    }
    wlr_scene_node_reparent(&m_sceneTree->node, m_server->dragTree());
    reparentShadow(m_server->dragShadowTree());
    setNodeEnabled(true);
    resetSurfaceClip();
    raiseToTop();
  }

  void View::restoreHomePresentation() {
    m_dragOpacity = 1.0F;
    m_effectiveOpacityCommitPending = false;
    setFadeAlpha(m_fadeAlpha);
    wlr_scene_node_place_below(&m_server->dragTree()->node, &m_server->dragIconTree()->node);
    wlr_scene_node_place_below(&m_server->dragShadowTree()->node, &m_server->dragTree()->node);
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
    const Overview* overview = m_server->overview();
    const bool presentedInOverview = overview != nullptr && overview->active() && m_workspace != nullptr;
    if (!m_mapped
        || (!m_onActiveWorkspace && !presentedInOverview)
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
    const auto& animation = config().animation;
    const auto& move = animation.windowsMove;
    if (!animation.enabled || !move.enabled) {
      m_presentation.setSize(width, height);
      m_presentation.snapTo(width, height);
      applyPresentedSize();
      return;
    }
    m_presentation.animateTo(width, height, move.durationMs, move.curve);
    scheduleFrame();
  }

  void View::setPosition(int x, int y) {
    m_posX.snap(x);
    m_posY.snap(y);
    m_positioned = true;
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
    m_decoration.setShadowPosition(x, y);
  }

  void View::snapPosition(int x, int y) { setPosition(x, y); }

  void View::animateFadeTo(float toAlpha, int durationMs, const AnimationCurve& curve) {
    m_fade.snap(m_fadeAlpha);
    m_fade.retarget(toAlpha, durationMs, curve);
    scheduleFrame();
  }

  void View::setDragPosition(int x, int y) {
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
    m_decoration.setShadowPosition(x, y);
  }

  void View::animateTo(int x, int y) {
    // First placement snaps: the node starts at the default (0,0) world origin, so animating would fly the window
    // across the layout on open. The fade-in covers the appear instead.
    const Overview* overview = m_server->overview();
    const bool presentedInOverview = overview != nullptr && overview->active() && m_workspace != nullptr;
    if (!m_mapped || (!m_onActiveWorkspace && !presentedInOverview) || !m_positioned) {
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
    const auto& animation = config().animation;
    const auto& move = animation.windowsMove;
    if (!animation.enabled || !move.enabled) {
      setPosition(x, y);
      return;
    }
    // Animate from wherever the node visually is, not from the last target.
    m_posX.snap(fromX);
    m_posX.retarget(x, move.durationMs, move.curve);
    m_posY.snap(fromY);
    m_posY.retarget(y, move.durationMs, move.curve);
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
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPresentationChanged(this);
      }
      active = m_posX.animating() || m_posY.animating();
    }

    if (m_presentation.tick(nowMsec)) {
      applyPresentedSize();
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPresentationChanged(this);
      }
      if (sizeAnimating()) {
        active = true;
      } else {
        finishSizeAnimation();
      }
    }

    if (m_fade.tick(nowMsec)) {
      setFadeAlpha(static_cast<float>(m_fade.current()));
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPresentationChanged(this);
      }
      active = active || m_fade.animating();
    }
    if (m_focusDim.tick(nowMsec)) {
      setFadeAlpha(m_fadeAlpha);
      active = active || m_focusDim.animating();
    }

    if (m_borderColorAnim.tick(nowMsec)) {
      m_decoration.setBorderRawColor(m_borderColorAnim.current(), effectiveOpacity());
      active = active || m_borderColorAnim.animating();
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
    return active;
  }

  bool View::animatesOn(const Output* output) const {
    const Workspace* workspace = m_workspace;
    if (workspace != nullptr && workspace->group() != nullptr) {
      return workspace->group()->output() == output;
    }
    // Scratchpad views have no workspace; fall back to the output we are physically on.
    return currentOutput() == output;
  }

  bool View::hasActiveAnimations() const {
    return m_posX.animating()
        || m_posY.animating()
        || sizeAnimating()
        || m_fade.animating()
        || m_borderColorAnim.animating()
        || m_focusDim.animating()
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

  void View::onOpacitySurfaceCommit(wl_listener* listener, void* /*data*/) {
    OpacitySurfaceWatch* watch;
    watch = wl_container_of(listener, watch, commit);
    if (watch->view->effectiveOpacity() < 1.0F) {
      watch->view->m_effectiveOpacityCommitPending = true;
      watch->view->scheduleFrame();
    }
  }

  void View::onOpacitySurfaceNewSubsurface(wl_listener* listener, void* data) {
    OpacitySurfaceWatch* watch;
    watch = wl_container_of(listener, watch, newSubsurface);
    auto* subsurface = static_cast<wlr_subsurface*>(data);
    watch->view->watchOpacitySurfaceTree(subsurface->surface);
  }

  void View::onOpacitySurfaceDestroy(wl_listener* listener, void* /*data*/) {
    OpacitySurfaceWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    View* view = watch->view;
    wl_list_remove(&watch->commit.link);
    wl_list_remove(&watch->newSubsurface.link);
    wl_list_remove(&watch->destroy.link);
    std::erase_if(view->m_opacitySurfaceWatches, [watch](const auto& candidate) { return candidate.get() == watch; });
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

  void View::onAcceptClientMaximizeRequests(void* data) {
    auto* self = static_cast<View*>(data);
    self->m_acceptClientMaximizeIdle = nullptr;
    self->m_acceptClientMaximizeRequests = self->m_mapped;
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

  void View::restoreFloatingPosition(bool rememberRestored) {
    if (m_tiled) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (const std::optional<FloatingPoint> origin = m_floating.restoredOrigin(usable)) {
      animateTo(origin->x, origin->y);
      if (rememberRestored) {
        // Re-anchor on the new usable area so a second deliberate cross-output move lands proportionally again.
        m_floating.rememberPositionFraction(*origin, usable);
      }
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
    return decorated() && !m_toplevel->scheduled.fullscreen
        ? nestedRadius(config().appearance.cornerRadius, borderInset())
        : 0;
  }

  void View::setBorderFocused(bool focused) {
    const bool focusChanged = m_borderFocusedState != focused;
    m_borderFocusedState = focused;

    const auto& animation = config().animation;
    const auto& dim = animation.dimUnfocused;
    if (focusChanged || !m_focusDimInitialized) {
      m_focusDimInitialized = true;
      const double target = focused || !m_mapped || !animation.enabled || !dim.enabled ? 1.0 : 1.0 - dim.dim;
      if (m_mapped && focusChanged && animation.enabled && dim.enabled) {
        m_focusDim.retarget(target, dim.durationMs, dim.curve);
        scheduleFrame();
      } else {
        m_focusDim.snap(target);
      }
      setFadeAlpha(m_fadeAlpha);
    }

    const auto& targetBase = m_scratchpadBorder
        ? (focused ? config().appearance.scratchpadBorderFocused : config().appearance.scratchpadBorderUnfocused)
        : (focused ? config().appearance.borderFocused : config().appearance.borderUnfocused);

    const auto& border = animation.border;
    if (m_mapped && focusChanged && animation.enabled && border.enabled) {
      m_borderColorAnim.retarget(targetBase, border.durationMs, border.curve);
      scheduleFrame();
    } else {
      m_borderColorAnim.snap(targetBase);
      m_decoration.setBorderColor(focused, m_scratchpadBorder, effectiveOpacity());
    }

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

  void View::applyCornerRadius() {
    // Apps that draw through subsurfaces (Firefox renders all of its chrome and web content into one desynchronized
    // MozContainer subsurface) leave their content square unless those buffers round too. Every buffer under the
    // toplevel's surface tree is visited: the main surface rounds unconditionally (its quad is the content box in every
    // clipped and animated state), a subsurface rounds only the corners where its quad, already cropped to the content
    // box by setSurfaceTreeClip, actually reaches a content-box corner, so an interior subsurface (embedded video)
    // stays square. Popups are excluded: their surface is its own root.
    const int radius = surfaceRadius();
    // A tiled view's committed geometry lags the layout, so the presented size is the box the corners must match; a
    // float has no such lag. Same rule as updateShadow().
    const wlr_box& geometry = m_toplevel->base->geometry;
    struct Ctx {
      View* view;
      int radius;
      int contentWidth;
      int contentHeight;
      int treeX;
      int treeY;
    } ctx{
        this,
        radius,
        m_tiled && m_presentation.width() > 0 ? m_presentation.width() : geometry.width,
        m_tiled && m_presentation.height() > 0 ? m_presentation.height() : geometry.height,
        m_sceneTree->node.x,
        m_sceneTree->node.y,
    };
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int sx, int sy, void* data) {
          auto* ctx = static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr
              || wlr_surface_get_root_surface(sceneSurface->surface) != ctx->view->m_toplevel->base->surface) {
            return;
          }
          if (sceneSurface->surface == ctx->view->m_toplevel->base->surface) {
            wlr_scene_buffer_set_corner_radii(buffer, corner_radii_all(ctx->radius));
            return;
          }
          // The iterator accumulates positions from the node it was handed, so subtracting the tree's own position
          // yields tree-local coordinates. The xdg scene helper places the surface tree at (-geometry.x, -geometry.y),
          // which puts the content box at the tree origin.
          const int x = sx - ctx->treeX;
          const int y = sy - ctx->treeY;
          const int w = buffer->dst_width > 0 ? buffer->dst_width : sceneSurface->surface->current.width;
          const int h = buffer->dst_height > 0 ? buffer->dst_height : sceneSurface->surface->current.height;
          const bool left = x == 0;
          const bool top = y == 0;
          const bool right = x + w == ctx->contentWidth;
          const bool bottom = y + h == ctx->contentHeight;
          const int r = ctx->radius;
          // Always set, zeros included: a subsurface that moved or resized off a corner must lose its stale radius.
          wlr_scene_buffer_set_corner_radii(
              buffer,
              corner_radii_new(
                  top && left ? r : 0, top && right ? r : 0, bottom && right ? r : 0, bottom && left ? r : 0
              )
          );
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
        effectiveOpacity(), m_fadeAlpha
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
    const int borderTotal = borderInset();
    m_decoration.updateShadow(
        contentWidth, contentHeight, borderTotal, decorated() ? config().appearance.cornerRadius : 0
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
    m_focusDimInitialized = false;
    setBorderFocused(false);
    updateBorderGeometry();
    applyCornerRadius();
    applyDynamicRules();
    updateShadow();
    reloadBackdropColor();
  }

  void View::beginCloseAnimation() {
    const auto& animation = config().animation;
    if (!m_mapped
        || !m_onActiveWorkspace
        || !animation.enabled
        || !animation.windowsOut.enabled
        || m_server->sessionLocked()
        || (m_server->overview() != nullptr && m_server->overview()->active())) {
      return;
    }

    Output* output =
        m_workspace != nullptr && m_workspace->group() != nullptr ? m_workspace->group()->output() : currentOutput();
    if (output == nullptr) {
      return;
    }

    // Under the output's clipped root, so a snapshot of a view straddling the shared edge stays contained while it
    // fades. Server::removeOutput purges this output's snapshots before the Output is destroyed.
    wlr_scene_tree* snap = wlr_scene_tree_create(output->viewRoot());
    if (snap == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&snap->node, m_sceneTree->node.x, m_sceneTree->node.y);

    std::vector<BorderSnapshot> snapBorders;
    m_decoration.snapshotBorders(snap, m_borderFocusedState, snapBorders);

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
          wlr_scene_buffer_set_transfer_function(copy, src->transfer_function);
          wlr_scene_buffer_set_primaries(copy, src->primaries);
          wlr_scene_buffer_set_luminance_multiplier(copy, src->luminance_multiplier);
          wlr_scene_buffer_set_color_encoding(copy, src->color_encoding);
          wlr_scene_buffer_set_color_range(copy, src->color_range);
          ++c->buffersCopied;
        },
        &ctx
    );

    if (ctx.buffersCopied == 0) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }

    m_server->animateCloseSnapshot(output, snap, std::move(snapBorders));
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
    // Scratchpad / floating views with no workspace: find the output containing the view's scene coordinates.
    if (m_sceneTree != nullptr && m_server != nullptr && m_server->outputLayout() != nullptr) {
      wlr_output* wlrOut = wlr_output_layout_output_at(
          m_server->outputLayout(), m_sceneTree->node.x + (m_toplevel ? m_toplevel->current.width / 2 : 0),
          m_sceneTree->node.y + (m_toplevel ? m_toplevel->current.height / 2 : 0)
      );
      if (wlrOut != nullptr) {
        return m_server->outputFromWlr(wlrOut);
      }
    }
    return m_server->outputFromWlr(m_server->preferredOutput());
  }

  std::optional<bool> View::tearingRuleOverride() { return resolvedRules().allowTearing; }

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

  void View::resetSurfaceClip() {
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
      setPosition(fullArea.x, fullArea.y);
    }

    const wlr_box target{
        m_sceneTree->node.x,
        m_sceneTree->node.y,
        fullArea.width,
        fullArea.height,
    };
    applyPresentation(target);
  }

  void View::applyPresentation(const wlr_box& target) {
    updateFullscreenPresentation(target.width, target.height);
    if (m_toplevel->current.fullscreen) {
      // The whole tile: the output's clipped root scissors whatever hangs over the shared edge.
      m_presentation.setBackdropBox(0, 0, target.width, target.height);
    }
    const wlr_box& geometry = m_toplevel->base->geometry;
    // Stay inside the tile while geometry lags configure (Electron often stays wide).
    const wlr_box content{
        .x = target.x,
        .y = target.y,
        .width = presentedWidth(target),
        .height = presentedHeight(target),
    };
    m_presentedBox = content;
    trackPresentedSize(content.width, content.height);

    // The presented box in surface coordinates. The fullscreen offsets center a buffer that does not match the tile.
    // Popup children stay unclipped in setSurfaceTreeClip so context menus can extend past the window edge.
    const wlr_box surfaceClip{
        .x = geometry.x - m_presentation.offsetX(),
        .y = geometry.y - m_presentation.offsetY(),
        .width = content.width,
        .height = content.height,
    };
    setSurfaceTreeClip(&surfaceClip);
    applyCornerRadius();
    if (sizeAnimating() || sizeGrabActive()) {
      // The clip crops 1:1 in surface coordinates and caps the destination at the committed surface size, so it cannot
      // express an animated or interactive presented size. Program the buffer directly; the clip above keeps the buffer
      // node positioned at the visible box origin.
      applyPresentedCrop(content, surfaceClip);
    }
    updateBorderGeometry(content.width, content.height);
    updateShadow();
    updateBlur(content.width, content.height);
  }

  void View::handleMap() {
    m_mapped = true;
    m_acceptClientMaximizeRequests = config().general.honorRestoredMaximize;
    m_acceptClientMaximizeIdle =
        wl_event_loop_add_idle(wl_display_get_event_loop(m_server->display()), onAcceptClientMaximizeRequests, this);
    if (m_acceptClientMaximizeIdle == nullptr) {
      kLog.error("failed to register opening maximize idle source");
      m_acceptClientMaximizeRequests = true;
    }
    m_server->scheduleIpcWindowsEvent();
    m_tiled = looksTiled(m_toplevel);
    const wlr_box& mapGeo = m_toplevel->base->geometry;
    m_presentation.setSize(mapGeo.width, mapGeo.height);
    resetSurfaceClip();

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

    showDecorations(!m_toplevel->scheduled.fullscreen);

    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this, rule.defaultWidth);
    } else if (!attachToAvailableWorkspace(rule)) {
      setOnActiveWorkspace(true);
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
    if (!m_server->sessionLocked() && rule.defaultFocused.value_or(true)) {
      m_server->focusView(this);
    }
    if (m_onActiveWorkspace) {
      const auto& animation = config().animation;
      const auto& open = animation.windowsIn;
      if (!animation.enabled || !open.enabled || open.style == "none") {
        setFadeAlpha(1.0F);
        m_fade.snap(1.0);
      } else {
        setFadeAlpha(0.0F);
        m_fade.snap(0.0);
        m_fade.retarget(1.0, open.durationMs, open.curve);

        if (open.style == "popin" || open.style == "zoom") {
          const int targetW = m_presentation.width();
          const int targetH = m_presentation.height();
          if (targetW > 0 && targetH > 0) {
            const double scale = open.style == "zoom" ? 0.5 : open.scale;
            const int startW = std::max(1, static_cast<int>(targetW * scale));
            const int startH = std::max(1, static_cast<int>(targetH * scale));
            const int targetX = m_sceneTree->node.x;
            const int targetY = m_sceneTree->node.y;
            const int startX = targetX + (targetW - startW) / 2;
            const int startY = targetY + (targetH - startH) / 2;

            m_presentation.setSize(startW, startH);
            m_presentation.animateTo(targetW, targetH, open.durationMs, open.curve);
            wlr_scene_node_set_position(&m_sceneTree->node, startX, startY);
            m_posX.snap(startX);
            m_posY.snap(startY);
            m_posX.retarget(targetX, open.durationMs, open.curve);
            m_posY.retarget(targetY, open.durationMs, open.curve);
          }
        } else if (open.style == "slide") {
          const int targetX = m_sceneTree->node.x;
          const int targetY = m_sceneTree->node.y;
          const int startY = targetY + 60;
          wlr_scene_node_set_position(&m_sceneTree->node, targetX, startY);
          m_posX.snap(targetX);
          m_posY.snap(startY);
          m_posY.retarget(targetY, open.durationMs, open.curve);
        }
        scheduleFrame();
      }
    }

    // Opening state is compositor-owned. Clients may restore a saved maximized
    // flag during this transition; only an explicit window rule overrides the
    // layout's initial size.
    const bool ruleMaximized = rule.defaultMaximize && *rule.defaultMaximize;
    const bool restoredMaximized = config().general.honorRestoredMaximize && m_toplevel->requested.maximized;
    if (ruleMaximized || restoredMaximized) {
      setMaximized(true);
    }

    // After default_maximize so maximize-to-edges wins the column, but before
    // fullscreen: setFullscreen leaves and restores the maximize-to-edges state.
    if (rule.defaultMaximizeToEdges && *rule.defaultMaximizeToEdges) {
      setMaximizedToEdges(true);
    }

    // Fullscreen after workspace + focus so the view lands in the right place.
    if (rule.defaultFullscreen && *rule.defaultFullscreen) {
      setFullscreen(true);
    }

    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewMapped(this);
    }
    m_server->updateIdleInhibit();
    if (Output* output = currentOutput()) {
      output->updateHdr();
    }
    if (m_displacedHome) {
      // Snapshot peers retain their member ids while this view is unmapped.
      // Replay their shared structure now that this member is visible again.
      m_server->scheduleDisplacedViewRestore();
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
    // Choose the layout neighbor while this view still belongs to the layout. Waiting for destroy loses that position,
    // and focus-follows-mouse used to replace it with whichever survivor happened to sit under the stationary pointer.
    if (m_workspace != nullptr && m_workspace->focusedView() == this) {
      View* replacement = m_workspace->focusReplacementForRemoval(this);
      if (replacement != nullptr) {
        if (m_workspace->active() && !m_server->sessionLocked()) {
          m_server->focusView(replacement, FocusReason::Directional);
        } else {
          m_workspace->setFocusedView(replacement);
        }
      } else {
        m_workspace->setFocusedView(nullptr);
      }
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
    m_acceptClientMaximizeRequests = false;
    if (m_acceptClientMaximizeIdle != nullptr) {
      wl_event_source_remove(m_acceptClientMaximizeIdle);
      m_acceptClientMaximizeIdle = nullptr;
    }
    m_server->updateIdleInhibit();
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      Output* output = m_workspace->group()->output();
      output->updateVrr();
      output->updateHdr();
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
    if (m_displacedHome) {
      m_server->scheduleDisplacedViewRestore();
    }
  }

  void View::handleCommit() {
    if (m_captureScene != nullptr) {
      // Restrict the capture to the xdg window geometry. Client subsurfaces
      // remain visible, while buffer content outside the declared window is
      // excluded. Window-owned popups are separate children and remain part
      // of the isolated scene.
      wlr_scene_subsurface_tree_set_clip(&m_captureScene->tree.node, &m_toplevel->base->geometry);
    }
    if (m_toplevel->base->initial_commit) {
      // Resolve window rules early to influence initial tiled/float decision and size.
      const ResolvedWindowRule rule = resolvedRules();
      const bool wantTiled = rule.defaultFloating ? !*rule.defaultFloating : looksTiled(m_toplevel);

      // Resolve the workspace this view will attach to, so the output and layout that will actually arrange it are the
      // ones that size the first configure.
      Workspace* target = m_workspace;
      Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
      WorkspaceGroup* targetGroup = target != nullptr
          ? target->group()
          : windowRuleWorkspaceGroup(*m_server, rule, preferred != nullptr ? preferred->workspaceGroup() : nullptr);
      if (target == nullptr) {
        target = windowRuleWorkspace(targetGroup, rule);
      }
      Output* targetOutput = targetGroup != nullptr ? targetGroup->output() : preferred;

      wlr_xdg_toplevel_set_tiled(
          m_toplevel, wantTiled ? WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT : 0
      );

      if (m_toplevel->requested.fullscreen) {
        // xwayland-satellite can request fullscreen while creating the xdg role, before the initial surface commit.
        // The request event is too early to configure, but wlroots preserves it in requested state for us to honor now.
        wlr_xdg_toplevel_set_fullscreen(m_toplevel, true);
        wlr_box fullArea{};
        wlr_output* initialOutput = targetOutput != nullptr ? targetOutput->wlr() : m_server->preferredOutput();
        if (initialOutput != nullptr) {
          wlr_output_layout_get_box(m_server->outputLayout(), initialOutput, &fullArea);
        }
        if (fullArea.width > 0 && fullArea.height > 0) {
          wlr_xdg_toplevel_set_size(m_toplevel, fullArea.width, fullArea.height);
        }
      } else if (wantTiled) {
        wlr_box usable = targetOutput != nullptr
            ? targetOutput->usableArea()
            : m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
        if (targetOutput != nullptr && (usable.width <= 0 || usable.height <= 0)) {
          wlr_output_layout_get_box(m_server->outputLayout(), targetOutput->wlr(), &usable);
        }
        if (usable.width <= 0 || usable.height <= 0) {
          usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
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

        const Layout::InitialSize initial =
            layout.initialSize(usable, rule.defaultWidth, target != nullptr ? target->focusedView() : nullptr);
        const int width = rule.defaultSize ? (*rule.defaultSize)[0] : initial.width;
        wlr_xdg_toplevel_set_size(m_toplevel, width, initial.height);
      } else {
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
        m_workspace->snapVisible(this);
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
    updateForeignState();
    if (Output* output = currentOutput()) {
      output->updateHdr();
    }
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

    clearOpacitySurfaceWatches();

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
    if (!m_toplevel->base->initialized || !m_mapped || !m_acceptClientMaximizeRequests) {
      return;
    }
    if (m_tiled && m_workspace != nullptr) {
      if (maximizeRequestTargetsEdges(m_maximizedToEdges)) {
        setMaximizedToEdges(m_toplevel->requested.maximized);
        return;
      }
      setMaximized(m_toplevel->requested.maximized);
      return;
    }
    setMaximized(m_toplevel->requested.maximized);
  }

  void View::setMaximizedToEdges(bool maximized) {
    if (!maximized) {
      m_restoreMaximizedToEdges = false;
    }
    if (!m_toplevel->base->initialized || maximized == m_maximizedToEdges) {
      return;
    }
    if (maximized && m_toplevel->scheduled.fullscreen) {
      setFullscreen(false);
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
    }
    cancelSizeAnimation();

    m_maximizedToEdges = maximized;
    bool columnFullWidth = false;
    if (!maximized && m_tiled && m_workspace != nullptr && !m_toplevel->scheduled.fullscreen) {
      const int column = m_workspace->layout().columnOf(this);
      columnFullWidth = column >= 0 && m_workspace->layout().isFullWidth(column);
    }
    if (m_tiled) {
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized || columnFullWidth);
    } else {
      setMaximized(maximized);
    }
    showDecorations(!maximized && !m_toplevel->scheduled.fullscreen);
    if (m_workspace != nullptr) {
      m_workspace->snapVisible(this);
      m_workspace->markArrange(true);
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
    const FullscreenRequestDisposition disposition = m_deferredUnfullscreen.observeClientRequest(
        requested, m_toplevel->scheduled.activated, m_toplevel->scheduled.fullscreen
    );

    if (disposition == FullscreenRequestDisposition::Acknowledge) {
      // Wine spams set_fullscreen while already fullscreen. Acknowledge without the visible reparent, scroll snap, and
      // arrange churn that a full setFullscreen() would run. Observing this newer request also clears any parked
      // unfullscreen, so activation cannot apply stale client intent.
      wlr_xdg_surface_schedule_configure(m_toplevel->base);
      return;
    }

    if (disposition == FullscreenRequestDisposition::Park) {
      // Wine games commonly unfullscreen when they lose focus. Park that request briefly instead of ripping the game
      // out of the fullscreen strip; xdg or foreign activation consumes it, while expiry preserves fullscreen.
      kLog.debug(
          "request_fullscreen parked for deactivated '{}'", m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?"
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
        m_workspace->snapVisible(this);
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

  void View::applyDeferredUnfullscreen() {
    if (!m_deferredUnfullscreen.takeOnActivation() || !m_toplevel->base->initialized) {
      return;
    }
    if (m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen) {
      kLog.debug(
          "deferred unfullscreen applied on activation for '{}'",
          m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?"
      );
      setFullscreen(false);
    }
  }

  void View::toggleFloating() { setFloating(m_tiled); }

  void View::restorePinnedSceneParent() {
    if (!m_pinned) {
      return;
    }
    Output* output = currentOutput();
    // Ordering stays on the server-level trees; only the content hangs under the output's clipped roots.
    wlr_scene_node_place_above(&m_server->pinnedShadowTree()->node, &m_server->fullscreenTree()->node);
    wlr_scene_node_place_above(&m_server->pinnedTree()->node, &m_server->pinnedShadowTree()->node);
    wlr_scene_node_reparent(&m_sceneTree->node, output != nullptr ? output->pinnedRoot() : m_server->pinnedTree());
    reparentShadow(output != nullptr ? output->pinnedShadowRoot() : m_server->pinnedShadowTree());
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
      m_workspace->snapVisible(this);
      m_workspace->markArrange(false);
    }
    updateForeignState();
  }

  void View::setFullscreen(bool fullscreen) {
    m_deferredUnfullscreen.clear();
    kLog.debug(
        "set_fullscreen '{}' [{}] -> {} (tiled={}, ws_active={})",
        m_toplevel->app_id != nullptr ? m_toplevel->app_id : "?", static_cast<const void*>(this), fullscreen, m_tiled,
        m_workspace != nullptr && m_workspace->active()
    );
    if (fullscreen && m_maximizedToEdges) {
      setMaximizedToEdges(false);
      m_restoreMaximizedToEdges = true;
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
        m_workspace->snapVisible(this);
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
        } else {
          m_workspace->snapVisible(this);
        }
        m_workspace->markArrange(!m_xwayland);
      } else if (!restoreFloating) {
        placeInUsableArea();
      }
    } else {
      m_pendingUnfullscreenSize = false;
      m_unfullscreenGraceStartMsec = 0;
    }
    if (!fullscreen && m_restoreMaximizedToEdges) {
      m_restoreMaximizedToEdges = false;
      setMaximizedToEdges(true);
    }
    updateForeignState();
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      Output* output = m_workspace->group()->output();
      output->updateVrr();
      output->updateHdr();
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

    if (changedInitialRule(rule.defaultMaximizeToEdges, initiallyApplied.defaultMaximizeToEdges)
        && *rule.defaultMaximizeToEdges
        && !m_maximizedToEdges) {
      setMaximizedToEdges(true);
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
    if (m_mapped) {
      m_server->refreshOutputPolicies();
    }
  }

} // namespace umbriel
