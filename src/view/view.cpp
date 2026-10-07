#include "view/view.h"

#include "config/config.h"
#include "config/resolve.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/surface_blur.h"
#include "server/server.h"
extern "C" {
#include <umbrielfx/render/effect.h>
}
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <array>
#include <ranges>
#include <utility>
#include <variant>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  namespace {
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

    // How dark the shade over the parent of an open modal dialog gets.
    constexpr double kModalParentDim = 0.3;

    wl_client* clientOf(const wlr_xdg_toplevel* toplevel) { return wl_resource_get_client(toplevel->resource); }
  } // namespace

  namespace view_detail {
    void removeListener(wl_listener& listener) {
      if (listener.link.next != nullptr) {
        wl_list_remove(&listener.link);
        listener.link = {};
      }
    }

    bool scratchpadOwnsOpeningGeometry() {
      const auto& scratchpad = config().animation.scratchpad;
      return scratchpad.fullscreen || scratchpad.maximize || (scratchpad.scale > 0.0 && scratchpad.scale <= 1.0);
    }

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
        const OutputRule* owner = nullptr;
        if (const auto* position = std::get_if<WorkspaceIndex>(&*rule.defaultWorkspace)) {
          owner = uniqueFixedWorkspaceOwner(config(), position->value - 1);
        } else if (const auto* name = std::get_if<WorkspaceName>(&*rule.defaultWorkspace)) {
          WorkspaceGroup* match = nullptr;
          bool ambiguous = false;
          for (const auto& output : server.outputs()) {
            WorkspaceGroup* group = output->workspaceGroup();
            if (group == nullptr || group->workspaceNamed(name->value) == nullptr) {
              continue;
            }
            if (match != nullptr) {
              ambiguous = true;
            } else {
              match = group;
            }
          }
          if (!ambiguous && match != nullptr) {
            return match;
          }
          if (ambiguous && fallback != nullptr && fallback->workspaceNamed(name->value) != nullptr) {
            return fallback;
          }
        }
        if (owner != nullptr) {
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
        Workspace* ruleTarget = nullptr;
        if (const auto* position = std::get_if<WorkspaceIndex>(&*rule.defaultWorkspace)) {
          ruleTarget = group->workspaceAtClamped(position->value - 1);
        } else if (const auto* name = std::get_if<WorkspaceName>(&*rule.defaultWorkspace)) {
          ruleTarget = group->workspaceNamed(name->value);
        }
        if (ruleTarget != nullptr) {
          target = ruleTarget;
        }
      }
      return target;
    }
  } // namespace view_detail

  using view_detail::removeListener;
  using view_detail::windowRuleWorkspace;
  using view_detail::windowRuleWorkspaceGroup;

  View::View(Server& server, wlr_xdg_toplevel* toplevel)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_toplevel(toplevel) {
    initCommon();
  }

  View::View(Server& server, wlr_xwayland_surface* xsurface)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_xsurface(xsurface) {
    initCommon();
  }

  void View::initCommon() {
    wlr_surface* surface = rootSurface();
    m_server->registerAnimatable(this);
    // Register map/unmap listeners BEFORE creating the scene tree so our handlers fire before wlroots' internal unmap
    // handler disables the surface subtree (needed for close-animation buffer snapshot).
    m_map.notify = onMap;
    wl_signal_add(&surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&surface->events.unmap, &m_unmap);
    m_rootSurfaceDestroy.notify = onRootSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &m_rootSurfaceDestroy);

    m_sceneTree = wlr_scene_tree_create(m_server->xdgTree());
    m_decoration.createShadow(m_sceneTree);
    // The content tree's direct child is the root surface's subsurface tree for both roles; the effect, clip, and
    // crossfade paths find the surface there.
    if (m_toplevel != nullptr) {
      m_contentTree = wlr_scene_xdg_surface_create(m_sceneTree, m_toplevel->base);
      m_toplevel->base->data = m_contentTree;
    } else {
      m_contentTree = wlr_scene_tree_create(m_sceneTree);
      wlr_scene_subsurface_tree_create(m_contentTree, surface);
      m_xsurface->data = m_contentTree;
    }
    // Both carry the tag: popups and constraints find the view through the role's `data`, layer walks through the
    // frame.
    m_sceneTree->node.data = sceneNodeData(this);
    m_contentTree->node.data = sceneNodeData(this);
    wlr_scene_node_set_enabled(&m_sceneTree->node, false);
    m_presentation.createBackdrop(m_contentTree);

    // A scene-node capture uses the node's scene as its render source. Keep a
    // second scene containing only the client surfaces so transparency cannot
    // reveal the wallpaper, another window, or compositor effects.
    m_captureScene = wlr_scene_create();
    if (m_captureScene != nullptr) {
      m_captureScene->restack_xwayland_surfaces = false;
      if (m_toplevel != nullptr) {
        wlr_scene_xdg_surface_create(&m_captureScene->tree, m_toplevel->base);
      } else {
        wlr_scene_subsurface_tree_create(&m_captureScene->tree, surface);
      }
    }
    if (m_xsurface != nullptr) {
      // Until the first configure places it, the window is drawn at the scale of the output its X position is on.
      applyXwaylandScale(m_server->xwayland()->outputs().regionAtX(m_xsurface->x, m_xsurface->y).scale);
    }
    notifyOutputScale();

    // The surface watcher must run before the general commit handler so a
    // newly committed content type participates in initial window rules.
    watchViewSurfaceTree(surface);
    m_commit.notify = onCommit;
    wl_signal_add(&surface->events.commit, &m_commit);
    m_clientCommit.notify = onClientCommit;
    wl_signal_add(&surface->events.client_commit, &m_clientCommit);

    m_requestMove.notify = onRequestMove;
    m_requestResize.notify = onRequestResize;
    m_requestMaximize.notify = onRequestMaximize;
    m_requestFullscreen.notify = onRequestFullscreen;
    m_setParent.notify = onSetParent;
    m_setTitle.notify = onSetTitle;
    m_setAppId.notify = onSetAppId;
    if (m_toplevel != nullptr) {
      m_destroy.notify = onDestroy;
      wl_signal_add(&m_toplevel->events.destroy, &m_destroy);
      wl_signal_add(&m_toplevel->events.request_move, &m_requestMove);
      wl_signal_add(&m_toplevel->events.request_resize, &m_requestResize);
      wl_signal_add(&m_toplevel->events.request_maximize, &m_requestMaximize);
      wl_signal_add(&m_toplevel->events.request_fullscreen, &m_requestFullscreen);
      wl_signal_add(&m_toplevel->events.set_parent, &m_setParent);
      wl_signal_add(&m_toplevel->events.set_title, &m_setTitle);
      wl_signal_add(&m_toplevel->events.set_app_id, &m_setAppId);
      // wlroots prepares the first capabilities event before exposing the toplevel. Adjust that pending event directly,
      // because the public setter schedules a configure and the surface is not initialized until its initial commit.
      m_toplevel->scheduled.wm_capabilities &= ~WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
    } else {
      // The X11 window's owner tears the view down through roleDestroyed() when the surface dissociates.
      wl_signal_add(&m_xsurface->events.request_move, &m_requestMove);
      wl_signal_add(&m_xsurface->events.request_resize, &m_requestResize);
      wl_signal_add(&m_xsurface->events.request_maximize, &m_requestMaximize);
      wl_signal_add(&m_xsurface->events.request_fullscreen, &m_requestFullscreen);
      wl_signal_add(&m_xsurface->events.set_parent, &m_setParent);
      wl_signal_add(&m_xsurface->events.set_title, &m_setTitle);
      wl_signal_add(&m_xsurface->events.set_class, &m_setAppId);
      m_xRequestConfigure.notify = onXwaylandRequestConfigure;
      wl_signal_add(&m_xsurface->events.request_configure, &m_xRequestConfigure);
      m_xRequestActivate.notify = onXwaylandRequestActivate;
      wl_signal_add(&m_xsurface->events.request_activate, &m_xRequestActivate);
      m_xSetHints.notify = onXwaylandSetHints;
      wl_signal_add(&m_xsurface->events.set_hints, &m_xSetHints);
    }

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
          .title = title(),
          .app_id = appId(),
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
    clearLaunchPlacement(true, true);
    if (m_effectSelectionIdle != nullptr) {
      wl_event_source_remove(m_effectSelectionIdle);
      m_effectSelectionIdle = nullptr;
    }
    if (m_acceptClientMaximizeIdle != nullptr) {
      wl_event_source_remove(m_acceptClientMaximizeIdle);
      m_acceptClientMaximizeIdle = nullptr;
    }
    m_server->unregisterAnimatable(this);
    m_effects.detach();
    clearViewSurfaceWatches();
    releaseDialog();
    setWorkspace(nullptr);
    // A view deleted while its role lives on (server teardown) must not leave the role pointing at freed scene nodes.
    if (m_map.link.next != nullptr) {
      if (m_toplevel != nullptr) {
        m_toplevel->base->data = nullptr;
      } else {
        m_xsurface->data = nullptr;
      }
    }
    for (wl_listener* listener :
         {&m_map, &m_unmap, &m_commit, &m_clientCommit, &m_destroy, &m_requestMove, &m_requestResize,
          &m_requestMaximize, &m_requestFullscreen, &m_setParent, &m_setTitle, &m_setAppId, &m_xRequestConfigure,
          &m_xRequestActivate, &m_xSetHints}) {
      removeListener(*listener);
    }
    if (m_rootSurfaceDestroy.link.next != nullptr) {
      wl_list_remove(&m_rootSurfaceDestroy.link);
      m_rootSurfaceDestroy.link.next = nullptr;
      m_rootSurfaceDestroy.link.prev = nullptr;
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
    if (m_sceneTree != nullptr) {
      // The attachment's nodes hang under the frame: release them first so their owner never destroys a freed node.
      m_chromeAttachment.reset();
      m_decoration.poolShadow(m_sceneTree, nullptr, 0, 0, false);
      wlr_scene_node_destroy(&m_sceneTree->node);
      m_sceneTree = nullptr;
    }
  }

  wlr_scene_tree* View::captureTree() const { return m_captureScene != nullptr ? &m_captureScene->tree : nullptr; }

  void View::moveToWorkspace(Workspace* workspace, bool attachToLayout) {
    moveToWorkspace(workspace, attachToLayout, LayoutAttachOrigin::ExistingView);
  }

  void View::moveToWorkspace(Workspace* workspace, bool attachToLayout, LayoutAttachOrigin origin) {
    const bool wasDisplaced = m_displacedHome.has_value();
    m_displacedHome.reset();
    setWorkspace(workspace, attachToLayout, origin);
    if (wasDisplaced) {
      m_server->scheduleDisplacedViewRestore();
    }
  }

  void View::setWorkspace(Workspace* workspace, bool attachToLayout) {
    setWorkspace(workspace, attachToLayout, LayoutAttachOrigin::ExistingView);
  }

  void View::setWorkspace(Workspace* workspace, bool attachToLayout, LayoutAttachOrigin origin) {
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
      m_workspace->addView(this, attachToLayout, origin);
    } else {
      // A pinned view normally hangs below an output-owned clipping root. Park its frame on the server-owned pinned
      // root before the last output is destroyed, then addView() can rehome it when an output returns. Leaving it under
      // the dying output frees nodes that the live View still owns.
      if (m_pinned) {
        setSceneParent(m_server->pinnedTree());
      }
      setOnActiveWorkspace(true);
    }
    notifyOutputScale();
    if (m_mapped
        && !m_tiled
        && m_workspace != nullptr
        && (m_pendingFloatingWidthPx
            || m_pendingFloatingHeightPx
            || m_pendingFloatingWidth
            || m_pendingFloatingHeight
            || m_pendingFloatingPosition)) {
      auto [width, height] = floatingSize();
      const wlr_box usable = floatingUsableArea();
      const SizeHints hints = sizeHints();
      if (m_pendingFloatingWidthPx) {
        width = clampWidth(*m_pendingFloatingWidthPx, hints);
        m_pendingFloatingWidthPx.reset();
        m_pendingFloatingWidth.reset();
      } else if (m_pendingFloatingWidth && usable.width > 0) {
        width = clampWidth(floatingFractionSize(*m_pendingFloatingWidth, usable.width), hints);
        m_pendingFloatingWidth.reset();
      }
      if (m_pendingFloatingHeightPx) {
        height = clampHeight(*m_pendingFloatingHeightPx, hints);
        m_pendingFloatingHeightPx.reset();
        m_pendingFloatingHeight.reset();
      } else if (m_pendingFloatingHeight && usable.height > 0) {
        height = clampHeight(floatingFractionSize(*m_pendingFloatingHeight, usable.height), hints);
        m_pendingFloatingHeight.reset();
      }
      if (width > 0 && height > 0 && (scheduledSize().width != width || scheduledSize().height != height)) {
        requestFloatingSize(width, height);
        beginResizeAnimation(width, height);
      }
      if (m_pendingFloatingPosition) {
        if (const auto positioned = getFloatingPosition(usable, m_pendingFloatingPosition, std::array{width, height})) {
          animateTo(positioned->x, positioned->y);
          m_floating.rememberPositionFraction(*positioned, usable);
          m_pendingFloatingPosition.reset();
        }
      }
    }
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
    retargetModalShadesAfterMove();
  }

  Workspace* View::parentWorkspace(const ResolvedWindowRule& rule) const {
    if (rule.defaultWorkspace || rule.defaultOutput) {
      return nullptr;
    }
    const View* parent = shellParent();
    return parent != nullptr ? parent->m_workspace : nullptr;
  }

  bool View::attachToAvailableWorkspace(const ResolvedWindowRule& rule, LayoutAttachOrigin origin) {
    Workspace* target = parentWorkspace(rule);
    if (target == nullptr) {
      Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
      WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;
      target = windowRuleWorkspace(windowRuleWorkspaceGroup(*m_server, rule, preferredGroup), rule);
    }
    if (target == nullptr) {
      return false;
    }
    setWorkspace(target, false);
    if (m_workspace != target) {
      return false;
    }
    target->layoutAttach(this, rule.defaultScrollingExtent, rule.defaultScrollingExtentPx, origin);
    return true;
  }

  bool View::assignLaunchOrigin(
      std::string_view token, std::shared_ptr<WorkspaceLaunchAnchor> workspace, uint32_t timeoutMsec
  ) {
    Workspace* target = workspace != nullptr ? workspace->workspace : nullptr;
    if (token.empty()
        || target == nullptr
        || m_mapped
        || m_hasEverMapped
        || m_launchPlacementPending
        || m_launchToken.has_value()) {
      return false;
    }
    wl_event_source* timer = nullptr;
    if (timeoutMsec > 0) {
      timer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onLaunchPlacementTimeout, this);
      if (timer == nullptr || wl_event_source_timer_update(timer, static_cast<int>(timeoutMsec)) < 0) {
        if (timer != nullptr) {
          wl_event_source_remove(timer);
        }
        return false;
      }
    }
    m_launchWorkspace = std::move(workspace);
    m_launchToken = token;
    m_launchWorkspaceId = target->id();
    m_launchPlacementPending = true;
    m_launchPlacementTimer = timer;
    return true;
  }

  bool View::consumeLaunchActivation(std::string_view token) {
    if (!m_launchToken || *m_launchToken != token) {
      return false;
    }
    if (m_hasEverMapped && !m_mapped) {
      m_launchToken.reset();
      return false;
    }
    m_launchToken.reset();
    return true;
  }

  bool View::cancelLaunchOrigin(std::string_view token) {
    if (!m_launchToken || *m_launchToken != token) {
      return false;
    }
    clearLaunchPlacement(true, true);
    return true;
  }

  void View::clearLaunchPlacement(bool reconcile, bool clearToken) {
    Workspace* workspace = m_launchWorkspace != nullptr ? m_launchWorkspace->workspace : nullptr;
    WorkspaceGroup* group = workspace != nullptr ? workspace->group() : nullptr;
    if (m_launchPlacementTimer != nullptr) {
      wl_event_source_remove(m_launchPlacementTimer);
      m_launchPlacementTimer = nullptr;
    }
    m_launchWorkspace.reset();
    m_launchPlacementPending = false;
    m_launchWorkspaceId.clear();
    if (clearToken) {
      m_launchToken.reset();
    }
    if (reconcile && group != nullptr && !m_server->stopping()) {
      group->reconcileDynamic();
    }
  }

  void View::detachWorkspace() {
    m_workspace = nullptr;
    if (!m_hasEverMapped && m_launchPlacementPending) {
      clearLaunchPlacement(true, true);
    }
    m_openingScale = 1.0;
    m_openingSlide = 0;
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
    // A hidden tab stays hidden when its workspace comes back.
    const bool shown = active && !m_tabHidden;
    if (m_sceneTree != nullptr) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, shown);
    }
    m_decoration.setShadowEnabled(shown);
    if (!m_mapped) {
      return;
    }
    if (active) {
      enterForeignOutput();
    } else {
      // Output membership tracks the physical monitor, not the active workspace: a window on another workspace is still
      // on its output. Leaving the output here would make foreign-toplevel clients drop the window from their task
      // lists. Workspace-less overlays leave here, then their owner can
      // explicitly advertise a retained output assignment.
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

  void View::setTabHidden(bool hidden) {
    if (m_tabHidden == hidden) {
      return;
    }
    m_tabHidden = hidden;
    // A client that honours it stops rendering while its tab is hidden; the background frame timer still keeps one that
    // does not alive.
    setSuspendedState(hidden);
    if (m_sceneTree != nullptr) {
      // A revealed tab shows exactly when its workspace does.
      setNodeEnabled(!hidden && m_mapped && m_onActiveWorkspace);
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewPresentationChanged(this);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::setNodeEnabled(bool enabled) {
    enabled = enabled && !presentationSuppressed();
    wlr_scene_node_set_enabled(&m_sceneTree->node, enabled);
    m_decoration.setShadowEnabled(enabled);
    m_server->updateIdleInhibit();
  }

  void View::raiseToTop() { transientRoot()->raiseTransientTree(); }

  View* View::transientRoot() {
    View* root = this;
    while (View* parent = root->transientParent()) {
      root = parent;
    }
    return root;
  }

  View* View::shellParent() const {
    wlr_surface* parentSurface = nullptr;
    if (m_toplevel != nullptr) {
      if (m_toplevel->parent != nullptr && m_toplevel->parent->base != nullptr) {
        parentSurface = m_toplevel->parent->base->surface;
      }
    } else if (m_xsurface->parent != nullptr) {
      parentSurface = m_xsurface->parent->surface;
    }
    if (parentSurface == nullptr) {
      return nullptr;
    }
    View* parent = fromSurface(parentSurface);
    if (parent == this || parent == nullptr || !parent->m_mapped) {
      return nullptr;
    }
    return parent;
  }

  View* View::transientParent() const {
    if (!m_mapped) {
      return nullptr;
    }
    View* parent = shellParent();
    if (parent == nullptr) {
      return nullptr;
    }
    if (m_workspace != nullptr && parent->m_workspace == m_workspace) {
      return parent;
    }
    ScratchpadManager* scratchpad = m_server->scratchpadManager();
    if (scratchpad == nullptr || !scratchpad->contains(this) || !scratchpad->contains(parent)) {
      return nullptr;
    }
    const std::string_view name = scratchpad->nameFor(this);
    return !name.empty() && scratchpad->nameFor(parent) == name ? parent : nullptr;
  }

  bool View::modalDialog() const {
    // X11 windows have neither xdg-dialog-v1 nor a Wayland client of their own to cross processes with.
    if (m_toplevel == nullptr || m_toplevel->parent == nullptr) {
      return false;
    }
    if (m_dialog != nullptr && m_dialog->modal) {
      return true;
    }
    return clientOf(m_toplevel) != clientOf(m_toplevel->parent);
  }

  void View::setDialog(wlr_xdg_dialog_v1* dialog) {
    releaseDialog();
    m_dialog = dialog;
    m_dialogSetModal.notify = onDialogSetModal;
    wl_signal_add(&dialog->events.set_modal, &m_dialogSetModal);
    m_dialogDestroy.notify = onDialogDestroy;
    wl_signal_add(&dialog->events.destroy, &m_dialogDestroy);
  }

  void View::releaseDialog() {
    if (m_dialog == nullptr) {
      return;
    }
    wl_list_remove(&m_dialogSetModal.link);
    wl_list_remove(&m_dialogDestroy.link);
    m_dialog = nullptr;
  }

  void View::syncModalDialogEntry() { m_server->registry().setModalDialog(this, m_mapped && modalDialog()); }

  void View::handleDialogModal() {
    syncModalDialogEntry();
    if (!m_mapped) {
      return;
    }
    syncOwnedPresentation();
    retargetModalShades();
    wlr_seat* seat = m_server->seat()->wlr();
    if (View* focused = fromSurface(seat->keyboard_state.focused_surface);
        focused != nullptr && focused->blockingDialog() != nullptr) {
      m_server->focusView(focused);
    }
  }

  bool View::blockedBy(const View& dialog) const {
    const View* parent = dialog.attachedParent();
    // A dialog blocks its parent even when it mapped first and took the parent on later, as its toolkit's grab does. A
    // parent that is itself a dialog is blocked only by a newer one, so the focus walk up the blocking dialogs ends.
    if (parent == this && (attachedParent() == nullptr || dialog.m_mapSerial > m_mapSerial)) {
      return true;
    }
    if (dialog.m_mapSerial < m_mapSerial) {
      return false;
    }
    // The application's own modal dialog is modal for every window it had open on the workspace, since that is what
    // its toolkit does. X11 windows all share the Xwayland connection, so the rule cannot tell X11 apps apart.
    if (clientOf(dialog.m_toplevel) == clientOf(parent->m_toplevel)) {
      return m_toplevel != nullptr
          && parent->m_workspace == m_workspace
          && clientOf(parent->m_toplevel) == clientOf(m_toplevel);
    }
    // A dialog from another process, a portal's chooser, reaches only the parent's own open dialogs: the one it was
    // opened from, when the application attached it to the main window instead.
    for (const View* ancestor = transientParent(); ancestor != nullptr; ancestor = ancestor->transientParent()) {
      if (ancestor == parent) {
        return true;
      }
    }
    return false;
  }

  View* View::blockingDialog() const {
    View* newest = nullptr;
    for (View* dialog : m_server->registry().modalDialogs()) {
      // The surface is already unmapped when a closing dialog hands focus back to its parent.
      if (dialog == this
          || dialog->attachedParent() == nullptr
          || !dialog->m_toplevel->base->surface->mapped
          || !blockedBy(*dialog)) {
        continue;
      }
      if (newest == nullptr || dialog->m_mapSerial > newest->m_mapSerial) {
        newest = dialog;
      }
    }
    return newest;
  }

  View* View::attachedParent() const { return modalDialog() ? transientParent() : nullptr; }

  View* View::attachedRoot() {
    View* root = this;
    while (View* parent = root->attachedParent()) {
      root = parent;
    }
    return root;
  }

  void View::centerModalDialogs() {
    // Overview cards are not places a dialog can sit over; the layout re-centers it when the overview closes.
    if (const Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      return;
    }
    for (View* dialog : m_server->registry().modalDialogs()) {
      if (dialog->attachedParent() == this) {
        dialog->syncOwnedPresentation();
      }
    }
  }

  void View::retargetModalShade(bool animate) {
    const double target = m_mapped && blockingDialog() != nullptr ? kModalParentDim : 0.0;
    const auto& animation = config().animation;
    const auto& dim = animation.dimUnfocused;
    if (animate && animation.enabled && dim.enabled) {
      m_modalShade.retarget(target, dim.durationMs, dim.curve);
      scheduleFrame();
    } else {
      m_modalShade.snap(target);
    }
    syncModalShade();
  }

  void View::retargetModalShades() {
    for (const auto& view : m_server->registry().all()) {
      if (view.get() != this) {
        view->retargetModalShade(true);
      }
    }
  }

  void View::retargetModalShadesAfterMove() {
    if (!m_mapped || m_server->registry().modalDialogs().empty()) {
      return;
    }
    retargetModalShade(true);
    retargetModalShades();
  }

  void View::syncModalShade() {
    const auto alpha = static_cast<float>(m_modalShade.current());
    // Premultiplied black at `alpha`, drawn over the content and rounded with it.
    const std::array<float, 4> color{0.0F, 0.0F, 0.0F, alpha};
    if (m_modalShadeRect == nullptr) {
      if (alpha <= 0.0F) {
        return;
      }
      m_modalShadeRect = wlr_scene_rect_create(m_sceneTree, 0, 0, color.data());
      // Hit-testing has to land on the shaded window, so its dialog gets the click and a drag moves it.
      m_modalShadeRect->accepts_input = false;
    }
    wlr_scene_rect_set_color(m_modalShadeRect, color.data());
    wlr_scene_rect_set_size(m_modalShadeRect, m_presentedBox.width, m_presentedBox.height);
    wlr_scene_rect_set_corner_radius(m_modalShadeRect, surfaceRadius());
    wlr_scene_node_set_enabled(&m_modalShadeRect->node, alpha > 0.0F);
    wlr_scene_node_raise_to_top(&m_modalShadeRect->node);
  }

  bool View::inheritScratchpadFromParent(bool restoreTiled) {
    ScratchpadManager* scratchpad = m_server->scratchpadManager();
    if (!m_mapped
        || scratchpad == nullptr
        || scratchpad->contains(this)
        || m_initialRules.defaultScratchpad
        || m_initialRules.defaultWorkspace
        || m_initialRules.defaultPinned.value_or(false)) {
      return false;
    }
    View* parent = shellParent();
    if (parent == nullptr) {
      return false;
    }
    Workspace* restoreWorkspace = m_workspace;
    Output* restoreOutput = restoreWorkspace != nullptr && restoreWorkspace->group() != nullptr
        ? restoreWorkspace->group()->output()
        : currentOutput();
    return scratchpad->assignFromParent(
        this, parent,
        ScratchpadManager::AutomaticAdmission{
            .restoreOutput = restoreOutput,
            .restoreWorkspace = restoreWorkspace,
            .focusOrigin = scratchpad->outputFor(parent),
            .restoreTiled = restoreTiled,
            .updateRestoreLocation = true,
        }
    );
  }

  void View::syncTransientSceneParent() {
    if (!m_mapped
        || (m_workspace == nullptr && !m_inScratchpad)
        || m_pinned
        || m_server->cursor()->isDraggingView(this)) {
      return;
    }

    wlr_scene_tree* target = homeTree();
    if (View* parent = transientParent()) {
      wlr_scene_tree* parentTree = parent->m_sceneTree->node.parent;
      Output* output = currentOutput();
      const bool parentElevated = parentTree == m_server->dragTree()
          || (m_workspace != nullptr && parentTree == m_workspace->fullscreenTree())
          || (m_inScratchpad && parentTree == m_server->fullscreenTree())
          || (output != nullptr && parentTree == output->pinnedRoot());
      if (parentElevated) {
        target = parentTree;
      }
    }

    if (m_sceneTree->node.parent == target) {
      return;
    }
    setSceneParent(target);
  }

  void View::raiseTransientTree() {
    syncTransientSceneParent();
    wlr_scene_node_raise_to_top(&m_sceneTree->node);

    const auto views = m_server->registry().all();
    for (const auto& view : std::views::reverse(views)) {
      View* child = view.get();
      if (child != this && child->transientParent() == this) {
        child->raiseTransientTree();
      }
    }
  }

  void View::setInScratchpad(bool scratchpad) {
    if (m_inScratchpad == scratchpad) {
      return;
    }
    m_inScratchpad = scratchpad;
    setBorderFocused(m_borderFocusedState);
    refreshStateRuleEffects();
    retargetModalShadesAfterMove();
  }

  void View::setSceneParent(wlr_scene_tree* parent) {
    wlr_scene_node_reparent(&m_sceneTree->node, parent);
    syncShadowPool();
    // Another parent can put the window elsewhere in layout coordinates.
    if (m_xsurface != nullptr && m_mapped) {
      syncXwaylandConfigure();
    }
  }

  void View::syncShadowPool() {
    wlr_scene_tree* pool = m_workspace != nullptr && m_sceneTree->node.parent == m_workspace->viewLayer(true)
        ? m_workspace->tileShadowLayer()
        : nullptr;
    m_decoration.poolShadow(m_sceneTree, pool, m_sceneTree->node.x, m_sceneTree->node.y, m_sceneTree->node.enabled);
  }

  void View::setScenePosition(int x, int y) {
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
    m_decoration.setShadowPosition(x, y);
    if (m_xsurface != nullptr) {
      syncXwaylandConfigure();
    }
  }

  void View::applySeatFocus(bool withKeyboard) {
    // Mechanism only. Policy lives in Server::focusView; do not call directly
    // from input/event code.
    if (!m_onActiveWorkspace && !m_pinned) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = rootSurface();
    // Always clear other views first: the previous seat surface may be a layer, so
    // deactivating only that surface can leave another window's focus border on.
    m_server->deactivateViews(this);

    raiseToTop();
    if (!scheduledActivated()) {
      setActivatedState(true);
    } else {
      reclaimXwaylandFocus();
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
    m_server->notifyKeyboardEnter(surface);
  }

  void View::scheduleFrame() {
    if (Output* output = currentOutput()) {
      wlr_output_schedule_frame(output->wlr());
    }
  }

  bool View::layoutFullscreen() const { return scheduledFullscreen(); }

  pid_t View::pid() const {
    if (m_xsurface != nullptr) {
      return m_xsurface->pid > 0 ? m_xsurface->pid : -1;
    }
    return surfaceClientPid(rootSurface());
  }

  int View::onLaunchPlacementTimeout(void* data) {
    auto* self = static_cast<View*>(data);
    self->clearLaunchPlacement(true, true);
    return 0;
  }

  void View::onDialogSetModal(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_dialogSetModal);
    self->handleDialogModal();
  }

  void View::onDialogDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_dialogDestroy);
    // Destroying the object undoes what it asked for, so the dialog is modal no longer unless it crosses processes.
    self->releaseDialog();
    self->handleDialogModal();
  }

  void View::onSetTitle(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setTitle);
    self->handleSetTitle();
  }

  void View::onSetAppId(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setAppId);
    self->handleSetAppId();
  }

  wlr_box View::targetBox() const {
    // A window that mapped in this same dispatch has its arrange still pending, so its slot is missing or stale.
    const bool inLayout = m_workspace != nullptr && m_workspace->layout().columnOf(this) >= 0;
    if (inLayout) {
      m_workspace->flushArrange();
    }
    if (scheduledFullscreen()) {
      // Fullscreen takes the output's size; the strip still places a tiled one at its column.
      const wlr_box area = fullscreenArea();
      return {layoutTargetX(), layoutTargetY(), area.width, area.height};
    }
    if (inLayout) {
      return m_workspace->presentedTiledBox(this);
    }
    // A float's scheduled size is the one a maximize or resize is taking it to, and the client's own once it settled.
    const wlr_box& geometry = geometryBox();
    const int width = scheduledSize().width > 0 ? scheduledSize().width : geometry.width;
    const int height = scheduledSize().height > 0 ? scheduledSize().height : geometry.height;
    return {layoutTargetX(), layoutTargetY(), width, height};
  }

  Output* View::currentOutput() const {
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      return m_workspace->group()->output();
    }
    if (m_server != nullptr && m_server->scratchpadManager() != nullptr) {
      if (Output* output = m_server->scratchpadManager()->outputFor(this)) {
        return output;
      }
    }
    // Other floating views with no workspace use their scene coordinates.
    if (m_sceneTree != nullptr && m_server != nullptr && m_server->outputLayout() != nullptr) {
      wlr_output* wlrOut = wlr_output_layout_output_at(
          m_server->outputLayout(), m_sceneTree->node.x + (currentSize().width / 2),
          m_sceneTree->node.y + (currentSize().height / 2)
      );
      if (wlrOut != nullptr) {
        return m_server->outputFromWlr(wlrOut);
      }
    }
    return m_server->outputFromWlr(m_server->preferredOutput());
  }

  void View::notifyOutputScale() {
    Output* output = currentOutput();
    if (output == nullptr || rootSurface() == nullptr) {
      return;
    }
    forEachSurface(&Output::notifySurfaceScaleIter, output);
    forEachPopupSurface(&Output::notifySurfaceScaleIter, output);
    // Chrome that renders text follows the scale too.
    layoutChromeAttachment(m_chromeContentWidth, m_chromeContentHeight);
  }

  void View::unconstrainPopup(wlr_xdg_popup* popup) {
    if (popup == nullptr || m_sceneTree == nullptr || m_toplevel == nullptr) {
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
    const wlr_box& geometry = geometryBox();
    const wlr_box box{
        .x = m_tiled ? geometry.x : target.x - lx + geometry.x,
        .y = target.y - ly + geometry.y,
        .width = m_tiled ? geometry.width : target.width,
        .height = target.height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
  }

  View* View::fromSurface(wlr_surface* surface) {
    // Both roles point their `data` at the view's content tree, which carries the view's tag.
    const auto viewFromContentTree = [](void* data) -> View* {
      auto* tree = static_cast<wlr_scene_tree*>(data);
      if (tree == nullptr) {
        return nullptr;
      }
      SceneNode* node = sceneNodeFrom(tree->node.data);
      if (node == nullptr || node->kind != SceneNodeKind::View) {
        return nullptr;
      }
      return static_cast<View*>(node);
    };
    wlr_surface* walk = surface;
    while (walk != nullptr) {
      if (wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(walk)) {
        return viewFromContentTree(toplevel->base->data);
      }
      // Override-redirect X11 windows carry no content tree and resolve to no view.
      if (wlr_xwayland_surface* xsurface = wlr_xwayland_surface_try_from_wlr_surface(walk)) {
        return viewFromContentTree(xsurface->data);
      }
      if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(walk)) {
        walk = popup->parent;
        continue;
      }
      break;
    }
    return nullptr;
  }

  wlr_scene_tree* View::homeTree() const {
    const bool fs = scheduledFullscreen();
    if (m_workspace != nullptr) {
      return fs ? m_workspace->fullscreenTree() : m_workspace->viewLayer(m_tiled);
    }
    if (m_inScratchpad) {
      return fs ? m_server->fullscreenTree() : m_server->scratchpadTree();
    }
    return fs ? m_server->fullscreenTree() : m_server->xdgTree();
  }

  void View::deferActivation(bool trusted) {
    // A trusted launch request wins if clients race multiple tokens during role creation. An untrusted request must
    // not downgrade it before the first buffer arrives.
    if (trusted || !m_deferredActivationTrusted.has_value()) {
      m_deferredActivationTrusted = trusted;
    }
  }

  void View::setXdgTag(std::string_view tag) {
    if (m_xdgTag == tag) {
      return;
    }
    m_xdgTag = std::string(tag);
    m_server->scheduleIpcWindowsEvent();
    if (m_mapped) {
      applyDynamicRules();
    }
  }
} // namespace umbriel
