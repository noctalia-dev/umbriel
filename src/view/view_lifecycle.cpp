#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "core/tracy.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/surface_blur.h"
#include "server/server.h"
#include "view/view.h"
extern "C" {
#include <umbrielfx/render/effect.h>
}
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <utility>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  namespace {
    constexpr Logger kLog("view");
    // Distance the built-in windows_in "slide" style starts an opener below its resting position.
    constexpr int kOpenSlidePx = 60;

    constexpr int contentTypePriority(ContentType type) {
      switch (type) {
      case ContentType::Game:
        return 3;
      case ContentType::Video:
        return 2;
      case ContentType::Photo:
        return 1;
      case ContentType::None:
        return 0;
      }
      return 0;
    }

    View* tiledViewAtLayoutPoint(Workspace& workspace, double lx, double ly) {
      for (const Column& column : workspace.layout().columns()) {
        for (View* candidate : column.views) {
          if (candidate == nullptr || !candidate->mapped() || !candidate->tiled()) {
            continue;
          }
          const wlr_box target = workspace.presentedTiledBox(candidate);
          if (target.width > 0 && target.height > 0 && wlr_box_contains_point(&target, lx, ly)) {
            return candidate;
          }
        }
      }
      return nullptr;
    }
  } // namespace

  using view_detail::removeListener;
  using view_detail::scratchpadOwnsOpeningGeometry;
  using view_detail::toplevelSurfaceTreeNode;
  using view_detail::windowRuleWorkspace;
  using view_detail::windowRuleWorkspaceGroup;

  void View::watchViewSurfaceTree(wlr_surface* root, wlr_subsurface* attachment) {
    if (root == nullptr) {
      return;
    }
    watchViewSurface(root, attachment);

    wlr_subsurface* child;
    wl_list_for_each(child, &root->current.subsurfaces_below, current.link) {
      watchViewSurfaceTree(child->surface, child);
    }
    wl_list_for_each(child, &root->current.subsurfaces_above, current.link) {
      watchViewSurfaceTree(child->surface, child);
    }
  }

  void View::watchViewSurface(wlr_surface* surface, wlr_subsurface* attachment) {
    if (surface == nullptr) {
      return;
    }

    const auto existing =
        std::ranges::find_if(m_viewSurfaceWatches, [surface](const auto& watch) { return watch->surface == surface; });
    if (existing != m_viewSurfaceWatches.end()) {
      ViewSurfaceWatch& watch = **existing;
      if (watch.subsurface == nullptr && attachment != nullptr) {
        watch.subsurface = attachment;
        watch.subsurfaceDestroy.notify = onViewSubsurfaceDestroy;
        wl_signal_add(&attachment->events.destroy, &watch.subsurfaceDestroy);
      }
      return;
    }

    auto watch = std::make_unique<ViewSurfaceWatch>();
    watch->view = this;
    watch->surface = surface;
    watch->subsurface = attachment;
    watch->contentType = m_server->surfaceContentType(surface);
    watch->commit.notify = onViewSurfaceCommit;
    wl_signal_add(&surface->events.commit, &watch->commit);
    watch->newSubsurface.notify = onViewSurfaceNewSubsurface;
    wl_signal_add(&surface->events.new_subsurface, &watch->newSubsurface);
    if (watch->subsurface != nullptr) {
      watch->subsurfaceDestroy.notify = onViewSubsurfaceDestroy;
      wl_signal_add(&watch->subsurface->events.destroy, &watch->subsurfaceDestroy);
    }
    watch->destroy.notify = onViewSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &watch->destroy);
    m_viewSurfaceWatches.push_back(std::move(watch));
  }

  void View::clearViewSurfaceWatches() {
    for (const auto& watch : m_viewSurfaceWatches) {
      wl_list_remove(&watch->commit.link);
      wl_list_remove(&watch->newSubsurface.link);
      if (watch->subsurface != nullptr) {
        wl_list_remove(&watch->subsurfaceDestroy.link);
      }
      wl_list_remove(&watch->destroy.link);
    }
    m_viewSurfaceWatches.clear();
  }

  void View::onMap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void View::onUnmap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void View::onRootSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_rootSurfaceDestroy);
    wl_list_remove(&self->m_rootSurfaceDestroy.link);
    self->m_rootSurfaceDestroy.link.next = nullptr;
    self->m_rootSurfaceDestroy.link.prev = nullptr;
  }

  void View::onCommit(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void View::onClientCommit(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_clientCommit);
    self->handleClientCommit();
  }

  void View::onViewSurfaceCommit(wl_listener* listener, void* /*data*/) {
    ViewSurfaceWatch* watch;
    watch = wl_container_of(listener, watch, commit);
    watch->view->syncContentType(watch->surface);
    if (watch->view->effectiveOpacity() < 1.0F) {
      watch->view->m_effectiveOpacityCommitPending = true;
      watch->view->scheduleFrame();
    }
  }

  void View::onViewSurfaceNewSubsurface(wl_listener* listener, void* data) {
    ViewSurfaceWatch* watch;
    watch = wl_container_of(listener, watch, newSubsurface);
    auto* subsurface = static_cast<wlr_subsurface*>(data);
    watch->view->watchViewSurfaceTree(subsurface->surface, subsurface);
    watch->view->syncContentType();
  }

  void View::onViewSubsurfaceDestroy(wl_listener* listener, void* /*data*/) {
    ViewSurfaceWatch* watch;
    watch = wl_container_of(listener, watch, subsurfaceDestroy);
    watch->subsurface = nullptr;
    wl_list_remove(&watch->subsurfaceDestroy.link);
    watch->view->syncContentType();
  }

  void View::onViewSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    ViewSurfaceWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    View* view = watch->view;
    wl_list_remove(&watch->commit.link);
    wl_list_remove(&watch->newSubsurface.link);
    if (watch->subsurface != nullptr) {
      wl_list_remove(&watch->subsurfaceDestroy.link);
    }
    wl_list_remove(&watch->destroy.link);
    std::erase_if(view->m_viewSurfaceWatches, [watch](const auto& candidate) { return candidate.get() == watch; });
  }

  void View::onDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_destroy);
    self->roleDestroyed();
  }

  void View::handleMap() {
    m_hasEverMapped = true;
    // The XDG map signal is emitted before the root surface commit signal.
    // Refresh only the root cache here, then let each descendant's own commit
    // keep its cached double-buffered state authoritative.
    syncContentType(rootSurface());
    if (m_xsurface != nullptr) {
      // X11 has no initial-commit handshake: send the opening configure now. The first buffer is presented through
      // committedContentBox() until the client redraws at the configured size.
      handleCommit(true);
    }
    m_mapped = true;
    m_effects.resetSlots();
    m_tiledOpeningDeferred = false;
    m_presentedTiledBox = {};
    m_acceptClientMaximizeRequests = config().general.honorRestoredMaximize;
    // Firefox often re-assert session maximize after map. With honor off, consume that one request so
    // it cannot override alone/default opening policy; later maximize requests stay valid.
    if (config().general.honorRestoredMaximize) {
      m_consumeRestoredMaximizeRequest = false;
    } else if (requestedMaximized()) {
      m_consumeRestoredMaximizeRequest = true;
    }
    m_acceptClientMaximizeIdle =
        wl_event_loop_add_idle(wl_display_get_event_loop(m_server->display()), onAcceptClientMaximizeRequests, this);
    if (m_acceptClientMaximizeIdle == nullptr) {
      kLog.error("failed to register opening maximize idle source");
      m_acceptClientMaximizeRequests = true;
    }
    m_server->scheduleIpcWindowsEvent();
    m_tiled = looksTiled();
    const wlr_box& mapGeo = geometryBox();
    m_presentation.setSize(mapGeo.width, mapGeo.height);
    resetSurfaceClip();

    // Resolve opening rules before startup focus and placement can change
    // state-based matches. Dynamic rules use the live state later.
    m_initialRuleState = ruleState();
    m_initialRuleState.focused = false;
    m_initialRuleState.alone = false;
    const ResolvedWindowRule rule = resolveWindowRules(
        config(), ruleText(appId()), ruleText(title()), m_xdgTag, m_contentType, m_initialRuleState,
        m_server->uptimeMs()
    );
    m_initialRules = rule;
    m_initialRuleHistory.clear();
    m_initialRuleHistory.push_back(rule);
    m_initialRulesAppId = ruleText(appId());
    m_initialRulesTitle = ruleText(title());
    m_initialRulesXdgTag = m_xdgTag;
    m_initialRulesContentType = m_contentType;
    m_namedScrollingColumnName = rule.defaultScrollingColumn;
    m_namedScrollingColumnOrder = rule.defaultScrollingColumnOrder;
    m_ruleColumnDisplay = rule.defaultColumnDisplay;
    const bool launchRuleOverride = m_launchPlacementPending
        && (rule.defaultOutput.has_value() || rule.defaultWorkspace.has_value() || rule.defaultScratchpad.has_value());
    if (rule.defaultFloating) {
      m_tiled = !*rule.defaultFloating;
    }
    const bool restoreTiled = m_tiled;
    // Unsettled when any rule uses a title pattern: the first handleSetTitle after map re-applies disruptive effects
    // with the real title, even if the client mapped with a placeholder.
    m_initialTitleRulesSettled = !anyWindowRuleHasTitlePattern(config());
    m_initialContentTypeRulesSettled = m_contentType != ContentType::None
        || std::ranges::none_of(config().windowRules,
                                [](const WindowRule& candidate) { return candidate.matchContentType.has_value(); });

    showDecorations(!scheduledFullscreen());

    Workspace* launchWorkspace = m_launchWorkspace != nullptr ? m_launchWorkspace->workspace : nullptr;
    if (!launchRuleOverride && m_workspace == nullptr && launchWorkspace != nullptr) {
      setWorkspace(launchWorkspace, false);
    } else if (launchRuleOverride && m_workspace != nullptr) {
      setWorkspace(nullptr, false);
    }
    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(
          this, rule.defaultScrollingExtent, rule.defaultScrollingExtentPx, LayoutAttachOrigin::OpeningView
      );
    } else if (!attachToAvailableWorkspace(rule, LayoutAttachOrigin::OpeningView)) {
      setOnActiveWorkspace(true);
    }
    bool assignedScratchpad = false;
    if (rule.defaultScratchpad) {
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
          scratchpad != nullptr && scratchpad->hasScratchpad(*rule.defaultScratchpad)) {
        Workspace* restoreWorkspace = m_workspace;
        Output* restoreOutput = restoreWorkspace != nullptr && restoreWorkspace->group() != nullptr
            ? restoreWorkspace->group()->output()
            : currentOutput();
        assignedScratchpad = scratchpad->assignByWindowRule(
            this, *rule.defaultScratchpad, restoreOutput,
            ScratchpadManager::AutomaticAdmission{
                .restoreOutput = restoreOutput,
                .restoreWorkspace = restoreWorkspace,
                .focusOrigin = restoreOutput,
                .restoreTiled = restoreTiled,
                .updateRestoreLocation = true,
            }
        );
        if (assignedScratchpad && rule.defaultFocused.value_or(false)) {
          scratchpad->summon(*rule.defaultScratchpad, restoreOutput, this);
        }
      }
    }
    if (!assignedScratchpad) {
      assignedScratchpad = inheritScratchpadFromParent(restoreTiled);
    }
    if (!assignedScratchpad && rule.defaultPinned && *rule.defaultPinned) {
      setPinned(true, false);
    }
    if (assignedScratchpad && !scratchpadOwnsOpeningGeometry()) {
      // Scratchpad admission has detached the view from its workspace, so its
      // assigned scratchpad output now supplies the correct usable area.
      placeInUsableArea(rule.defaultPosition);
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
        scratchpad->syncViewPresentation(this);
      }
    } else if (!assignedScratchpad && !m_tiled) {
      // The initial commit already applied the floating size rules. Re-requesting them here
      // races the client's first content-driven resize.
      placeInUsableArea(rule.defaultPosition);
      // Enable + clip the float against its home output now that per-output
      // visibility is resolved data-side (no per-render-pass pass to do it).
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
        // layoutAttach only handles tiled arrivals.
        m_workspace->exitFullscreenForIncomingView(this);
      }
    }

    // Apply rule opacity: flush to scene buffers immediately so views on
    // inactive workspaces get the correct opacity when they become visible.
    if (rule.opacity) {
      m_ruleOpacity = static_cast<float>(*rule.opacity);
      setFadeAlpha(m_fadeAlpha);
    }

    const bool launchPlacementUsed = m_launchPlacementPending
        && !launchRuleOverride
        && !assignedScratchpad
        && m_workspace != nullptr
        && m_workspace->id() == m_launchWorkspaceId;
    Output* preferredOutput = m_server->outputFromWlr(m_server->preferredOutput());
    Workspace* preferredWorkspace = preferredOutput != nullptr && preferredOutput->workspaceGroup() != nullptr
        ? preferredOutput->workspaceGroup()->active()
        : nullptr;
    const bool launchPlacementSilent = launchPlacementUsed && m_workspace != preferredWorkspace;
    if (m_launchPlacementPending) {
      clearLaunchPlacement(!launchPlacementUsed, !launchPlacementUsed);
    }

    updateForeignIdentity();
    updateForeignState();
    const std::optional<bool> deferredActivation = std::exchange(m_deferredActivationTrusted, std::nullopt);
    const bool activateOnMap = deferredActivation.has_value()
        && rule.focusOnActivate.value_or(*deferredActivation || config().general.focusOnActivate);
    const bool focusOnMap =
        activateOnMap || (!deferredActivation.value_or(false) && rule.defaultFocused.value_or(true));
    const bool hiddenScratchpad = assignedScratchpad && !m_onActiveWorkspace;
    if (!m_server->sessionLocked() && focusOnMap && !hiddenScratchpad && !launchPlacementSilent) {
      m_server->focusView(this, activateOnMap ? FocusReason::XdgActivation : FocusReason::Startup);
    } else if (deferredActivation.has_value()) {
      setUrgent(true);
    }

    // Opening state is compositor-owned. Clients may restore a saved maximized
    // flag during this transition; only an explicit window rule overrides the
    // layout's initial size.
    const bool ruleMaximized = !openingParented() && rule.defaultMaximize && *rule.defaultMaximize;
    const bool restoredMaximized = config().general.honorRestoredMaximize && requestedMaximized();
    if (!assignedScratchpad && (ruleMaximized || restoredMaximized)) {
      setMaximized(true);
    }

    // After default_maximize so maximize-to-edges wins the column, but before
    // fullscreen: setFullscreen leaves and restores the maximize-to-edges state.
    if (!assignedScratchpad && rule.defaultMaximizeToEdges && *rule.defaultMaximizeToEdges) {
      setMaximizedToEdges(true);
    }

    // Fullscreen after workspace + focus so the view lands in the right place.
    if (!assignedScratchpad && rule.defaultFullscreen && *rule.defaultFullscreen) {
      setFullscreen(true);
    }

    settleOpeningAloneState();

    if (transientParent() != nullptr) {
      raiseToTop();
    }

    if (m_onActiveWorkspace) {
      const auto& animation = config().animation;
      const auto& open = animation.windowsIn;
      const bool customShader = effectRegistry().animationEffect(AnimationEvent::WindowsIn) != nullptr;
      m_customFade = animation.enabled && open.enabled && customShader;
      const bool animates = animation.enabled && open.enabled && (open.style != "none" || customShader);
      const bool tiledMember =
          m_tiled && !layoutFullscreen() && m_workspace != nullptr && m_workspace->layout().columnOf(this) >= 0;
      if (!animates) {
        setFadeAlpha(1.0F);
        m_fade.snap(1.0);
      } else if (tiledMember) {
        // The admitting arrange places a tiled member and starts its windows_in in the final slot, alongside any
        // windows_move reflow that makes room for it. An overview card follows that same reveal.
        deferTiledOpening();
      } else {
        setFadeAlpha(0.0F);
        m_fade.snap(0.0);
        m_fade.retarget(1.0, open.durationMs, open.curve);

        // Floating and fullscreen windows tween themselves.
        if (!m_customFade && (open.style == "popin" || open.style == "zoom")) {
          const double scale = std::clamp(open.style == "zoom" ? 0.5 : open.scale, 0.0, 1.0);
          if (layoutFullscreen()) {
            // The output box this window rests in is assigned by the arrange that follows this map, so the inset is
            // applied against that box on every fade tick instead of tweening a node it has not been placed in yet.
            m_openingScale = scale;
            const wlr_box area = fullscreenLayoutBox();
            setLayoutTarget(area.x, area.y);
            presentBox(area);
          } else if (m_presentation.width() > 0 && m_presentation.height() > 0) {
            const int targetW = m_presentation.width();
            const int targetH = m_presentation.height();
            const int startW = std::max(1, static_cast<int>(targetW * scale));
            const int startH = std::max(1, static_cast<int>(targetH * scale));
            const int targetX = m_sceneTree->node.x;
            const int targetY = m_sceneTree->node.y;
            const int startX = targetX + (targetW - startW) / 2;
            const int startY = targetY + (targetH - startH) / 2;

            m_presentation.setSize(startW, startH);
            m_presentation.animateTo(targetW, targetH, open.durationMs, open.curve);
            setScenePosition(startX, startY);
            m_posX.snap(startX);
            m_posY.snap(startY);
            m_posX.retarget(targetX, open.durationMs, open.curve);
            m_posY.retarget(targetY, open.durationMs, open.curve);
          }
        } else if (!m_customFade && open.style == "slide") {
          if (layoutFullscreen()) {
            m_openingSlide = kOpenSlidePx;
            const wlr_box area = fullscreenLayoutBox();
            setLayoutTarget(area.x, area.y);
            presentBox(area);
          } else {
            const int targetX = m_sceneTree->node.x;
            const int targetY = m_sceneTree->node.y;
            setScenePosition(targetX, targetY + kOpenSlidePx);
            m_posX.snap(targetX);
            m_posY.snap(targetY + kOpenSlidePx);
            m_posY.retarget(targetY, open.durationMs, open.curve);
          }
        }
        scheduleFrame();
      }
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
    // Opening rules resolve before default_floating and default_pinned move the window, so the state selectors may
    // pick a different set of dynamic effects than the ones applied above.
    applyDynamicRules();
  }

  void View::handleUnmap() {
    if (m_effectSelectionIdle != nullptr) {
      wl_event_source_remove(m_effectSelectionIdle);
      m_effectSelectionIdle = nullptr;
    }
    m_resizeCrossfade.discard();
    Workspace* closingWorkspace = m_workspace;
    Cursor* cursor = m_server->cursor();
    wlr_seat* seat = m_server->seat()->wlr();
    const Overview* overview = m_server->overview();
    View* pointerView = nullptr;
    if (cursor != nullptr) {
      wlr_surface* pointerSurface = nullptr;
      double pointerSx = 0.0;
      double pointerSy = 0.0;
      LayerSurface* pointerLayer = nullptr;
      pointerView =
          m_server->viewAt(cursor->wlr()->x, cursor->wlr()->y, &pointerSurface, &pointerSx, &pointerSy, &pointerLayer);
      if (pointerLayer != nullptr) {
        pointerView = nullptr;
      }
    }
    const bool focusRevealedTile = closingWorkspace != nullptr
        && closingWorkspace->focusedView() == this
        && closingWorkspace->active()
        && m_tiled
        && !shellParentRequested()
        && config().input.focus.followsMouse
        && !m_server->sessionLocked()
        && m_server->exclusiveKeyboardLayer() == nullptr
        && (overview == nullptr || !overview->active())
        && cursor != nullptr
        && cursor->isPassthrough()
        && seat->drag == nullptr
        && seat->pointer_state.button_count == 0
        && View::fromSurface(seat->keyboard_state.focused_surface) == this
        && pointerView == this
        && wlr_box_contains_point(&m_presentedBox, cursor->wlr()->x, cursor->wlr()->y);
    const double closePointerX = cursor != nullptr ? cursor->wlr()->x : 0.0;
    const double closePointerY = cursor != nullptr ? cursor->wlr()->y : 0.0;

    setUrgent(false);
    m_floatingMaximized = false;
    m_maximizedToEdges = false;
    m_hasFullscreenRestoreBox = false;
    m_restorePinnedAfterFullscreen = false;
    m_xCompositorFullscreen = false;
    if (m_pinned) {
      m_pinned = false;
      m_restoreTiledAfterUnpin = false;
      if (m_workspace != nullptr) {
        setSceneParent(m_workspace->viewLayer(false));
        setOnActiveWorkspace(m_workspace->active());
      }
    }
    if (m_server->scratchpadManager() != nullptr) {
      m_server->scratchpadManager()->remove(this);
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewUnmapped(this);
    }
    // Replacement focus can hide the closing tab. Capture while its current visibility still decides eligibility.
    const CloseSnapshotId snapshot = beginCloseAnimation();
    // Choose the layout neighbor while this view still belongs to the layout. Waiting for destroy loses that position.
    // It remains the fallback when the pointer did not belong to the closing tile or no survivor takes its place.
    if (m_workspace != nullptr && m_workspace->focusedView() == this) {
      View* replacement = m_workspace->focusReplacementForRemoval(this);
      if (replacement != nullptr) {
        // The layout judges the pair once this column is gone, so the reveal this unblocks may only fit the survivor.
        m_workspace->noteRemovalOfFocusedColumn(m_workspace->layout().columnOf(this));
        if (m_workspace->active() && !m_server->sessionLocked()) {
          m_server->focusView(replacement, FocusReason::Directional);
        } else {
          m_workspace->setFocusedView(replacement);
        }
      } else {
        m_workspace->setFocusedView(nullptr);
      }
    }
    // The closing snapshot must retain any in-flight opening shader first.
    wlr_scene_node_clear_animations(&m_contentTree->node);
    m_dragSlotBound = false;
    // The snapshot holds the frozen deformation; the live sheet ends here.
    m_dragPhysics = DragPhysics{};
    // The window slots leave with the snapshot; a remap binds them again.
    if (m_effects.needsSurface()) {
      wlr_surface* surface = rootSurface();
      clearWindowEffectSlots(toplevelSurfaceTreeNode(m_contentTree, surface));
      if (m_captureScene != nullptr) {
        clearWindowEffectSlots(toplevelSurfaceTreeNode(&m_captureScene->tree, surface));
      }
    }
    cancelFadeAnimation();
    // The workspace owns the snapshot's visibility and its slide translation from here.
    if (snapshot != kInvalidCloseSnapshot && m_workspace != nullptr) {
      m_workspace->trackCloseSnapshot(snapshot, m_presentedBox);
    }
    cancelSizeAnimation();
    m_tiledSizeRequest.reset();
    m_layoutPresentationHeld = false;
    cancelPositionAnimation();
    m_decoration.setBordersEnabled(false);
    m_decoration.hideEffects();
    m_presentation.setBackdropEnabled(false);
    if (currentFullscreen() || scheduledFullscreen()) {
      // Move out of the fullscreen layer back to the normal workspace/xdg tree.
      setSceneParent(m_workspace ? m_workspace->viewLayer(m_tiled) : m_server->xdgTree());
    }
    const bool hadRuntimeEffects = m_effects.slot(EffectKind::Border).runtimeSelector.has_value()
        || m_effects.slot(EffectKind::Window).runtimeSelector.has_value();
    m_mapped = false;
    m_effects.detach();
    m_effects.resetSlots();
    if (hadRuntimeEffects) {
      m_server->effects().prepare(m_server->renderer());
    }
    m_openingParentRequested = false;
    m_launchToken.reset();
    m_acceptClientMaximizeRequests = false;
    m_consumeRestoredMaximizeRequest = false;
    m_acceptClientMaximizeSerial.reset();
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
    m_presentedTiledBox = {};
    if (m_workspace != nullptr) {
      m_workspace->layoutDetach(this, m_tiled);
      if (focusRevealedTile) {
        // The scene may still be animating from its old geometry. Arrange now, then read the authoritative layout
        // targets once so compositor motion cannot produce a chain of hover focus changes.
        m_workspace->flushArrange();
        View* replacement = tiledViewAtLayoutPoint(*m_workspace, closePointerX, closePointerY);
        if (replacement != nullptr && m_workspace->focusedView() != replacement) {
          m_server->focusView(replacement, FocusReason::PointerHover);
        }
      }
    }
    leaveForeignOutput();
    setForeignActivated(false);
    if (!m_server->cursor()->isPassthrough()) {
      m_server->cursor()->resetMode();
    }
    m_initialTitleRulesSettled = false;
    m_initialContentTypeRulesSettled = false;
    m_initialRules = {};
    m_initialRuleHistory.clear();
    m_initialRuleState = {};
    m_initialRulesAppId.reset();
    m_initialRulesTitle.reset();
    m_initialRulesXdgTag.reset();
    m_initialRulesContentType = ContentType::None;
    m_namedScrollingColumnName.reset();
    m_namedScrollingColumnOrder.reset();
    m_ownsNamedScrollingColumnExtent = false;
    m_ruleOpacity = 1.0F;
    m_appliedRuleState = {};
    // An unmapped window keeps no layout state, so the alone effect it owned is gone with it.
    m_aloneEffectsActive = false;
    m_aloneAction = AloneAction::None;
    m_lastAloneDelta = {};
    m_aloneSavedWidthFrac.reset();
    m_aloneOpeningSeed = AloneSeed::None;
    m_hasMaximizeRestoreBox = false;
    m_floating.clearSizeRequest();
    m_pendingFloatingWidthPx.reset();
    m_pendingFloatingHeightPx.reset();
    m_pendingFloatingWidth.reset();
    m_pendingFloatingHeight.reset();
    m_pendingFloatingPosition.reset();
    m_savedScrollingExtentPx.reset();
    m_savedScrollingExtent.reset();
    if (m_displacedHome) {
      m_server->scheduleDisplacedViewRestore();
    }
  }

  void View::syncContentType(wlr_surface* committedSurface) {
    if (committedSurface != nullptr) {
      const auto committed = std::ranges::find_if(m_viewSurfaceWatches, [committedSurface](const auto& watch) {
        return watch->surface == committedSurface;
      });
      if (committed != m_viewSurfaceWatches.end()) {
        (*committed)->contentType = m_server->surfaceContentType(committedSurface);
      }
    }

    struct Context {
      const View* view;
      ContentType effective = ContentType::None;
    } context{this};
    wlr_surface_for_each_surface(
        rootSurface(),
        [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* data) {
          auto& context = *static_cast<Context*>(data);
          const auto watch = std::ranges::find_if(context.view->m_viewSurfaceWatches, [surface](const auto& candidate) {
            return candidate->surface == surface;
          });
          if (watch == context.view->m_viewSurfaceWatches.end()) {
            return;
          }

          if (contentTypePriority((*watch)->contentType) > contentTypePriority(context.effective)) {
            context.effective = (*watch)->contentType;
          }
        },
        &context
    );
    const ContentType next = context.effective;
    if (next == m_contentType) {
      return;
    }
    m_contentType = next;
    m_server->scheduleIpcWindowsEvent();
    if (m_mapped) {
      if (!m_initialContentTypeRulesSettled && next != ContentType::None) {
        m_initialContentTypeRulesSettled = true;
        m_initialRulesContentType = next;
        applyWindowRules();
        return;
      }
      applyDynamicRules();
    }
  }

  void View::handleClientCommit() {
    // The scene still shows the previous state here, so this is the last moment the outgoing frame can be cloned.
    const auto& animation = config().animation;
    wlr_surface* surface = rootSurface();
    if (!m_mapped
        || !m_tiled
        || !m_onActiveWorkspace
        || !m_tiledSizeRequest
        || !layoutPresentationOwned()
        || scheduledFullscreen()
        || currentFullscreen()
        || !animation.enabled
        || !animation.windowsMove.enabled
        || (surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0
        || surface->pending.buffer == nullptr
        || (surface->pending.width == surface->current.width && surface->pending.height == surface->current.height)
        || !pendingConfigureSettled(m_tiledSizeRequest->serial)) {
      return;
    }
    m_resizeCrossfade.capture(
        m_contentTree, toplevelSurfaceTreeNode(m_contentTree, surface), surface, m_presentedBox.width,
        m_presentedBox.height, surfaceRadius()
    );
  }

  void View::handleCommit(bool reconfigureOpeningState) {
    UMBRIEL_ZONE("View::handleCommit");
    if (m_resizeCrossfade.pending()) {
      const auto& move = config().animation.windowsMove;
      m_resizeCrossfade.start(move.durationMs, move.curve);
      m_resizeCrossfade.applyOpacity(effectiveOpacity());
      scheduleFrame();
    }
    // Client transparency can change on any commit. An unmap commit reaches here after handleUnmap hid the backdrop.
    if (m_mapped) {
      m_presentation.setFullscreenOpaque(fullscreenOpaque());
    }
    if (m_captureScene != nullptr) {
      // Restrict the capture to the window geometry. Client subsurfaces
      // remain visible, while buffer content outside the declared window is
      // excluded. Window-owned popups are separate children and remain part
      // of the isolated scene.
      const wlr_box captureClip = geometryBox();
      wlr_scene_subsurface_tree_set_clip(&m_captureScene->tree.node, &captureClip);
    }
    if (openingConfigurePending() || reconfigureOpeningState) {
      // Resolve window rules early to influence initial tiled/float decision and size.
      WindowRuleState openingState = ruleState();
      openingState.focused = false;
      openingState.floating = !looksTiled();
      openingState.alone = false;
      ResolvedWindowRule rule = resolveWindowRules(
          config(), ruleText(appId()), ruleText(title()), m_xdgTag, m_contentType, openingState, m_server->uptimeMs()
      );
      ScratchpadManager* scratchpadManager = m_server->scratchpadManager();
      const bool openingInScratchpad = rule.defaultScratchpad
          && scratchpadManager != nullptr
          && scratchpadManager->hasScratchpad(*rule.defaultScratchpad);
      const auto& scratchpadConfig = config().animation.scratchpad;
      const bool wantTiled = !openingInScratchpad
          && !rule.defaultPinned.value_or(false)
          && (rule.defaultFloating ? !*rule.defaultFloating : looksTiled());

      // Resolve the workspace this view will attach to, so the output and layout that will actually arrange it are the
      // ones that size the first configure.
      const bool launchRuleOverride = m_launchPlacementPending
          && (rule.defaultOutput.has_value()
              || rule.defaultWorkspace.has_value()
              || rule.defaultScratchpad.has_value());
      Workspace* launchWorkspace = m_launchWorkspace != nullptr ? m_launchWorkspace->workspace : nullptr;
      Workspace* target = launchRuleOverride ? nullptr : (m_workspace != nullptr ? m_workspace : launchWorkspace);
      Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
      WorkspaceGroup* targetGroup = target != nullptr
          ? target->group()
          : windowRuleWorkspaceGroup(*m_server, rule, preferred != nullptr ? preferred->workspaceGroup() : nullptr);
      if (target == nullptr) {
        target = windowRuleWorkspace(targetGroup, rule);
      }

      // A window that opens as the only tiled one on its workspace is configured in its `match.is_alone` state right
      // away, instead of showing one frame at the size its not-alone rules give it. Alone-ness depends on the
      // workspace those rules select, so this is a second pass, and it overrides only the four settings the alone
      // effect owns. A state that neither the client nor the not-alone rules asked for is recorded as the alone
      // rules' own, for map to hand to that effect.
      if (wantTiled && (target == nullptr || target->isOnlyTiledView(this))) {
        const ResolvedWindowRule alone = resolveRulesWithAlone(true);
        const bool seedFullscreen = alone.defaultFullscreen.value_or(false) && !rule.defaultFullscreen.value_or(false);
        const bool seedMaximized =
            (alone.defaultMaximize.value_or(false) && !openingParented() && !rule.defaultMaximize.value_or(false))
            || (alone.defaultMaximizeToEdges.value_or(false) && !rule.defaultMaximizeToEdges.value_or(false));
        // A client's restored maximize flag only blocks alone ownership when we honor it. Otherwise alone seeds as
        // usual and the restore is ignored (and consumed after map if the client re-asserts it).
        const bool clientOwnsRestoredMaximize = config().general.honorRestoredMaximize && requestedMaximized();
        if (seedFullscreen && !requestedFullscreen()) {
          m_aloneOpeningSeed = AloneSeed::Fullscreen;
        } else if (seedMaximized && !requestedFullscreen() && !clientOwnsRestoredMaximize) {
          m_aloneOpeningSeed = AloneSeed::Maximize;
        }
        rule.defaultFullscreen = alone.defaultFullscreen;
        rule.defaultMaximizeToEdges = alone.defaultMaximizeToEdges;
        rule.defaultMaximize = alone.defaultMaximize;
        rule.defaultScrollingExtentPx = alone.defaultScrollingExtentPx;
        rule.defaultScrollingExtent = alone.defaultScrollingExtent;
      }

      const bool wantFullscreen = openingInScratchpad
          ? scratchpadConfig.fullscreen
          : requestedFullscreen() || (rule.defaultFullscreen && *rule.defaultFullscreen);
      const bool wantMaximizeToEdges = openingInScratchpad
          ? !wantFullscreen && scratchpadConfig.maximize
          : rule.defaultMaximizeToEdges && *rule.defaultMaximizeToEdges;
      const bool wantMaximized = openingInScratchpad
          ? wantMaximizeToEdges
          : (!openingParented() && rule.defaultMaximize && *rule.defaultMaximize)
              || wantMaximizeToEdges
              || (config().general.honorRestoredMaximize && requestedMaximized());

      Output* targetOutput = targetGroup != nullptr ? targetGroup->output() : preferred;
      if (openingInScratchpad) {
        targetOutput = scratchpadManager->presentationOutput(*rule.defaultScratchpad, targetOutput);
      }

      // With prefer_no_csd, floating windows are told they are tiled too, so client decorations drop the rounded
      // corners and shadows the border cannot follow.
      const bool tiledEdges = wantTiled || config().appearance.preferNoCsd;
      setTiledState(tiledEdges ? WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT : 0);

      if (wantFullscreen) {
        // An X11 client can request fullscreen before its first buffer. The request event is too early to configure,
        // but the role preserves it in requested state for us to honor now.
        setFullscreenState(true);
        wlr_box fullArea{};
        wlr_output* initialOutput = targetOutput != nullptr ? targetOutput->wlr() : m_server->preferredOutput();
        if (initialOutput != nullptr) {
          wlr_output_layout_get_box(m_server->outputLayout(), initialOutput, &fullArea);
        }
        if (fullArea.width > 0 && fullArea.height > 0) {
          configureSize(fullArea.width, fullArea.height);
        }
      } else if (wantTiled) {
        const wlr_box usable = openingUsableArea(targetOutput);

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
        const wlr_box tiledArea =
            target != nullptr ? target->tiledArea() : applyLayoutStruts(usable, globalConfig.struts);

        Layout::InitialSize initial;
        const std::optional<Layout::InitialSize> namedScrollingColumnInitial =
            target != nullptr && rule.defaultScrollingColumn
            ? target->initialNamedScrollingColumnSize(
                  this, tiledArea, *rule.defaultScrollingColumn, rule.defaultScrollingColumnOrder, wantMaximized
              )
            : std::nullopt;
        if (wantMaximizeToEdges) {
          initial = {.width = usable.width, .height = usable.height};
        } else if (namedScrollingColumnInitial) {
          initial = *namedScrollingColumnInitial;
        } else if (wantMaximized && target != nullptr) {
          initial = target->initialMaximizedSize(this, tiledArea);
        } else {
          initial = layout.initialSize(
              tiledArea, wantMaximized, rule.defaultScrollingExtent, rule.defaultScrollingExtentPx,
              target != nullptr ? target->focusedView() : nullptr
          );
        }
        const SizeHints hints = sizeHints();
        const int width =
            (initial.width > 0 && !wantMaximizeToEdges) ? clampWidth(initial.width, hints) : initial.width;
        const int height =
            (initial.height > 0 && !wantMaximizeToEdges) ? clampHeight(initial.height, hints) : initial.height;
        configureSize(width, height);
        if (wantMaximized) {
          setMaximizedState(true);
        }

        // Keep floating defaults symbolic until the first float transition, when the target usable area is known.
        m_pendingFloatingWidthPx = rule.defaultFloatingWidthPx;
        m_pendingFloatingHeightPx = rule.defaultFloatingHeightPx;
        m_pendingFloatingWidth = rule.defaultFloatingWidth;
        m_pendingFloatingHeight = rule.defaultFloatingHeight;
        m_pendingFloatingPosition = rule.defaultPosition;
      } else {
        const SizeHints hints = sizeHints();
        if (wantMaximized) {
          const wlr_box usable = openingUsableArea(targetOutput);
          requestFloatingSize(usable.width, usable.height);
          setMaximizedState(true);
        } else if (openingInScratchpad && scratchpadConfig.scale > 0.0 && scratchpadConfig.scale <= 1.0) {
          const wlr_box usable = openingUsableArea(targetOutput);
          requestFloatingSize(
              std::max(100, static_cast<int>(std::lround(usable.width * scratchpadConfig.scale))),
              std::max(100, static_cast<int>(std::lround(usable.height * scratchpadConfig.scale)))
          );
        } else {
          const wlr_box usable = openingUsableArea(targetOutput);
          int requestedWidth = 0;
          int requestedHeight = 0;
          if (rule.defaultFloatingWidthPx) {
            requestedWidth = clampWidth(*rule.defaultFloatingWidthPx, hints);
          } else if (rule.defaultFloatingWidth && usable.width > 0) {
            requestedWidth = clampWidth(floatingFractionSize(*rule.defaultFloatingWidth, usable.width), hints);
          }
          if (rule.defaultFloatingHeightPx) {
            requestedHeight = clampHeight(*rule.defaultFloatingHeightPx, hints);
          } else if (rule.defaultFloatingHeight && usable.height > 0) {
            requestedHeight = clampHeight(floatingFractionSize(*rule.defaultFloatingHeight, usable.height), hints);
          }
          requestFloatingSize(requestedWidth, requestedHeight);
        }

        // Keep the configured unit until the window first enters the scrolling layout.
        m_savedScrollingExtentPx = rule.defaultScrollingExtentPx;
        m_savedScrollingExtent = rule.defaultScrollingExtent;
      }
    }
    (void)settleTiledSizeRequest();
    if (!sizeAnimating()) {
      const wlr_box& geometry = geometryBox();
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
        && !scheduledFullscreen()
        && !currentFullscreen()
        && sizeGrabTracksPointer()) {
      // During interactive resize, track geometry so no spurious animation
      // replays the drag when the grab ends and mode returns to Passthrough. A
      // move grab is not that: it retargets the size once and animates there,
      // and the client's ack must not cut that animation short.
      const wlr_box& geometry = geometryBox();
      if (geometry.width > 0 && geometry.height > 0) {
        if (sizeAnimating()) {
          cancelSizeAnimation();
        }
        m_presentation.setSize(geometry.width, geometry.height);
      }
    }
    // Re-apply output clip after configure ack so Super+F / resize sizes show
    // without needing a workspace switch (clip boxes are copied, not live).
    if (m_mapped && m_tiled && m_workspace != nullptr && m_workspace->active()) {
      m_workspace->syncViewPresentation(this);
    } else if (m_mapped && !m_tiled) {
      if (scheduledFullscreen() && m_onActiveWorkspace) {
        // Keep fullscreen placement authoritative; the xdg scene helper just
        // reset the surface offset for this commit.
        applyFullscreenLayout();
      } else {
        syncFloatingResizePosition();
        adoptFloatingClientSize();
        if (!sizeAnimating()) {
          syncFloatingSurfaceClip();
        }
        // Enable + clip through the current presentation owner.
        syncOwnedPresentation();
      }
    } else {
      updateBlur();
      updateShadow();
    }
    updateForeignState();
    if (Output* output = currentOutput()) {
      output->updateHdr();
    }
    // The commit that leaves fullscreen uncovers top-layer surfaces, so an exclusive one takes the seat back.
    if (m_committedFullscreen && !currentFullscreen() && m_mapped) {
      if (LayerSurface* layer = m_server->exclusiveKeyboardLayer()) {
        layer->focus();
      }
    }
    m_committedFullscreen = currentFullscreen();
    if (m_mapped && m_acceptClientMaximizeSerial && configureSettled(*m_acceptClientMaximizeSerial)) {
      m_acceptClientMaximizeSerial.reset();
      m_acceptClientMaximizeRequests = true;
    }
    // The first root commit after the opening gate settles the restore sequence.
    // A later maximize request is client intent and must not be consumed.
    if (m_mapped && m_acceptClientMaximizeRequests) {
      m_consumeRestoredMaximizeRequest = false;
    }
    // Parent trees can move the window without a setScenePosition; keep the X server's idea of it current.
    if (m_mapped && m_xsurface != nullptr) {
      syncXwaylandConfigure();
    }
  }

  void View::roleDestroyed() {
    m_resizeCrossfade.discard();
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
      detachCaptureAudio();
      wl_list_remove(&m_captureSourceDestroy.link);
      m_captureSourceDestroy.link.next = nullptr;
      m_captureSource = nullptr;
    }

    clearViewSurfaceWatches();

    for (wl_listener* listener :
         {&m_map, &m_unmap, &m_commit, &m_clientCommit, &m_destroy, &m_requestMove, &m_requestResize,
          &m_requestMaximize, &m_requestFullscreen, &m_setParent, &m_setTitle, &m_setAppId, &m_xRequestConfigure,
          &m_xRequestActivate, &m_xSetHints}) {
      removeListener(*listener);
    }
    m_sceneTree->node.data = nullptr;
    m_contentTree->node.data = nullptr;
    if (m_toplevel != nullptr) {
      m_toplevel->base->data = nullptr;
    } else {
      m_xsurface->data = nullptr;
    }
    m_server->removeView(this);
  }
} // namespace umbriel
