#include "config/config.h"
#include "core/log.h"
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
#include "view/maximize.h"
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <algorithm>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  namespace {
    constexpr Logger kLog("view");
  } // namespace

  using view_detail::scratchpadOwnsOpeningGeometry;

  void View::onRequestMove(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_requestMove);
    self->handleRequestMove(data);
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
    if (!self->m_mapped || self->m_acceptClientMaximizeRequests) {
      return;
    }
    // Clients such as kitty restore maximize just after their first frame. Keep the gate closed until the client
    // acknowledges the configure that carries the opening layout.
    if (self->m_toplevel != nullptr) {
      const wlr_xdg_surface* surface = self->m_toplevel->base;
      if (surface->configure_idle != nullptr || !wl_list_empty(&surface->configure_list)) {
        self->m_acceptClientMaximizeSerial = surface->scheduled_serial;
        return;
      }
    }
    self->m_acceptClientMaximizeRequests = true;
  }

  void View::onRequestFullscreen(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestFullscreen);
    self->handleRequestFullscreen();
  }

  void View::onSetParent(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setParent);
    self->handleSetParent();
  }

  void View::handleRequestMove(void* data) {
    if (!config().input.clientWindowDrag) {
      return;
    }
    if (m_toplevel == nullptr) {
      m_server->cursor()->beginClientMove(this, nullptr, 0);
      return;
    }
    auto* event = static_cast<wlr_xdg_toplevel_move_event*>(data);
    m_server->cursor()->beginClientMove(this, event->seat, event->serial);
  }

  void View::handleRequestResize(void* data) {
    if (m_toplevel == nullptr) {
      m_server->cursor()->beginClientResize(this, nullptr, 0, static_cast<wlr_xwayland_resize_event*>(data)->edges);
      return;
    }
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server->cursor()->beginClientResize(this, event->seat, event->serial, event->edges);
  }

  void View::setMaximized(bool maximized, bool animate) {
    if (m_tiled && m_workspace != nullptr) {
      m_floatingMaximized = false;
      if (m_maximizedToEdges) {
        setMaximizedToEdges(false);
      }
      const int column = m_workspace->layout().columnOf(this);
      if (column >= 0 && m_workspace->layout().isFullWidth(column) != maximized) {
        m_workspace->layout().toggleFullWidth(column);
      }
      setMaximizedState(maximized);
      m_workspace->reevaluateFocusedColumn();
      m_workspace->markArrange(animate);
      updateForeignState();
      return;
    }

    // Record the target before cancelSizeAnimation synchronizes the current
    // presentation back through ScratchpadManager.
    m_floatingMaximized = maximized;
    const ScratchpadManager* scratchpad = m_server->scratchpadManager();
    const bool visibleScratchpad = m_onActiveWorkspace && scratchpad != nullptr && scratchpad->contains(this);
    // A visible scratchpad has a manager-owned presentation even though it is
    // detached from a workspace, so its position and size can animate together.
    const bool animateFloating = animate && (m_workspace != nullptr || visibleScratchpad);
    if (!animateFloating) {
      cancelSizeAnimation();
    }
    const bool wasMaximized = scheduledMaximized();
    if (maximized && !wasMaximized) {
      // Capture where the float is heading, not where it currently sits.
      // Dropping fullscreen queues a configure and starts a move, so
      // base->geometry still holds the fullscreen size and the scene node is
      // mid-flight from the fullscreen origin. Restoring from either would
      // strand the float at output size with no way back.
      const auto [restoreWidth, restoreHeight] = floatingSize();
      m_floating.clearSizeRequest();
      m_maximizeRestoreBox = {
          .x = layoutTargetX(),
          .y = layoutTargetY(),
          .width = restoreWidth,
          .height = restoreHeight,
      };
      m_hasMaximizeRestoreBox = restoreWidth > 0 && restoreHeight > 0;

      placeFloatingMaximized(floatingMaximizedBox(floatingUsableArea()), animateFloating);
    } else if (!maximized && wasMaximized && m_hasMaximizeRestoreBox) {
      requestFloatingSize(m_maximizeRestoreBox.width, m_maximizeRestoreBox.height);
      if (animateFloating) {
        beginResizeAnimation(m_maximizeRestoreBox.width, m_maximizeRestoreBox.height);
        animateTo(m_maximizeRestoreBox.x, m_maximizeRestoreBox.y);
      } else {
        setPosition(m_maximizeRestoreBox.x, m_maximizeRestoreBox.y);
      }
      m_hasMaximizeRestoreBox = false;
    } else if (maximized) {
      // Maximize and maximize-to-edges fill different boxes, so a float switching between them is refit.
      placeFloatingMaximized(floatingMaximizedBox(floatingUsableArea()), false);
    }
    setMaximizedState(maximized);
    if (!sizeAnimating()) {
      syncFloatingSurfaceClip();
    }
    updateForeignState();
  }

  void View::placeFloatingMaximized(const wlr_box& box, bool animate) {
    if (box.width <= 0 || box.height <= 0) {
      return;
    }
    m_floatingMaximizedBox = box;
    configureSize(box.width, box.height);
    if (animate) {
      beginResizeAnimation(box.width, box.height);
      animateTo(box.x, box.y);
    } else {
      setPosition(box.x, box.y);
    }
  }

  void View::refitFloatingMaximized() {
    // A scratchpad window has no workspace: its manager refits it with the box this view computes.
    if (m_tiled || !m_mapped || !m_floatingMaximized || m_workspace == nullptr || scheduledFullscreen()) {
      return;
    }
    if (m_server->cursor()->isDraggingView(this)) {
      return;
    }
    const wlr_box box = floatingMaximizedBox(floatingUsableArea());
    const bool moved = box.x != m_floatingMaximizedBox.x
        || box.y != m_floatingMaximizedBox.y
        || box.width != m_floatingMaximizedBox.width
        || box.height != m_floatingMaximizedBox.height;
    if (moved) {
      placeFloatingMaximized(box, false);
      syncFloatingSurfaceClip();
    }
  }

  void View::handleRequestMaximize() {
    if (!roleInitialized()) {
      return;
    }
    if (!m_mapped) {
      if (config().general.honorRestoredMaximize && requestedMaximized()) {
        // Some clients restore maximization only after acknowledging the first
        // configure. Reconfigure before they map a buffer so their first visible
        // content already matches the maximized layout target.
        handleCommit(true);
      } else if (requestedMaximized()) {
        m_consumeRestoredMaximizeRequest = true;
      }
      return;
    }
    if (!m_acceptClientMaximizeRequests) {
      // A mapped request before the opening gate is itself the restore re-assert.
      // Consume it here so no later request inherits the suppression.
      if (!config().general.honorRestoredMaximize && requestedMaximized()) {
        m_consumeRestoredMaximizeRequest = false;
      }
      return;
    }
    // Alone-owned maximize is compositor policy. Clients that closed unmaximized (Firefox) re-assert that after map
    // and would otherwise undo the is_alone default_maximize flash.
    if (m_aloneEffectsActive
        && (m_aloneAction == AloneAction::Maximize || m_aloneAction == AloneAction::MaximizeToEdges)) {
      m_consumeRestoredMaximizeRequest = false;
      if (requestedMaximized() != scheduledMaximized()) {
        setMaximizedState(scheduledMaximized());
      }
      return;
    }
    if (m_consumeRestoredMaximizeRequest && requestedMaximized()) {
      // Drop the opening restore re-assert without granting the client maximize ownership. If the compositor (alone
      // rule, etc.) already maximized, stay there; otherwise re-ack the unmaximized configure.
      m_consumeRestoredMaximizeRequest = false;
      if (!scheduledMaximized()) {
        setMaximizedState(false);
      }
      return;
    }
    m_consumeRestoredMaximizeRequest = false;
    if (m_tiled && m_workspace != nullptr) {
      if (maximizeRequestTargetsEdges(requestedMaximized(), m_maximizedToEdges, config().layout.maximizeToEdges)) {
        setMaximizedToEdges(requestedMaximized());
        return;
      }
      setMaximized(requestedMaximized());
      return;
    }
    setMaximized(requestedMaximized());
  }

  void View::setMaximizedToEdges(bool maximized, bool animate) {
    if (!maximized) {
      m_restoreMaximizedToEdges = false;
    }
    if (!roleInitialized() || maximized == m_maximizedToEdges) {
      return;
    }
    const bool leavingFullscreen = maximized && scheduledFullscreen();
    if (leavingFullscreen) {
      setFullscreen(false, FullscreenExitLayout::DeferToCaller);
    }
    if (m_tiled || !animate) {
      cancelSizeAnimation();
    }

    m_maximizedToEdges = maximized;
    bool columnFullWidth = false;
    if (!maximized && m_tiled && m_workspace != nullptr && !scheduledFullscreen()) {
      const int column = m_workspace->layout().columnOf(this);
      columnFullWidth = column >= 0 && m_workspace->layout().isFullWidth(column);
    }
    if (m_tiled) {
      setMaximizedState(maximized || columnFullWidth);
    } else {
      setMaximized(maximized, animate);
    }
    showDecorations(!maximized && !scheduledFullscreen());
    if (m_workspace != nullptr) {
      m_workspace->snapVisible(this);
      if (leavingFullscreen) {
        // setFullscreen deferred its layout so this final maximize state and edge size replace the pending fullscreen
        // configure together.
        m_workspace->arrange(animate);
      } else {
        m_workspace->markArrange(animate);
      }
    }
    updateForeignState();
  }

  void View::toggleMaximizedToEdges() { setMaximizedToEdges(!m_maximizedToEdges); }

  void View::toggleMaximized() { setMaximized(m_tiled ? !scheduledMaximized() : !m_floatingMaximized); }

  void View::restoreMaximizedForMove() {
    // Fullscreen temporarily covers an underlying floating-maximized state.
    // Moving the fullscreen surface must not consume the state that should be
    // revealed when fullscreen ends.
    if (scheduledFullscreen() || currentFullscreen()) {
      return;
    }
    if (m_maximizedToEdges) {
      setMaximizedToEdges(false, false);
    } else if (m_floatingMaximized) {
      setMaximized(false, false);
    }
  }

  void View::dropMaximizedForResize() {
    if (m_tiled || !roleInitialized()) {
      return;
    }
    if (!m_maximizedToEdges && !m_floatingMaximized) {
      return;
    }
    // Not setMaximized(false)/setMaximizedToEdges(false): those replay the restore box and would undo the size the
    // caller is about to request.
    cancelSizeAnimation();
    const bool wasEdges = m_maximizedToEdges;
    m_maximizedToEdges = false;
    m_floatingMaximized = false;
    m_restoreMaximizedToEdges = false;
    m_hasMaximizeRestoreBox = false;
    setMaximizedState(false);
    if (wasEdges) {
      showDecorations(!scheduledFullscreen());
    }
    updateForeignState();
  }

  void View::handleRequestFullscreen() {
    if (!roleInitialized()) {
      return;
    }
    kLog.debug(
        "request_fullscreen '{}' [{}]: {}", appId() != nullptr ? appId() : "?", static_cast<const void*>(this),
        requestedFullscreen()
    );

    const bool requested = requestedFullscreen();
    if (!requested && scheduledFullscreen() && m_xCompositorFullscreen) {
      kLog.debug("request_fullscreen denied for compositor-owned X11 '{}'", appId() != nullptr ? appId() : "?");
      reassertRoleState();
      return;
    }
    const FullscreenRequestDisposition disposition =
        m_deferredUnfullscreen.observeClientRequest(requested, scheduledActivated(), scheduledFullscreen());

    if (disposition == FullscreenRequestDisposition::Acknowledge) {
      // Wine spams set_fullscreen while already fullscreen. Acknowledge without the visible reparent, scroll snap, and
      // arrange churn that a full setFullscreen() would run. Observing this newer request also clears any parked
      // unfullscreen, so activation cannot apply stale client intent.
      reassertRoleState();
      return;
    }

    if (disposition == FullscreenRequestDisposition::Park) {
      // Wine games commonly unfullscreen when they lose focus. Park that request briefly instead of ripping the game
      // out of the fullscreen strip; xdg or foreign activation consumes it, while expiry preserves fullscreen.
      kLog.debug("request_fullscreen parked for deactivated '{}'", appId() != nullptr ? appId() : "?");
      reassertRoleState();
      return;
    }

    // Honor the client's requested state (not a blind toggle).
    setFullscreen(requested);
  }

  void View::handleSetParent() {
    if (m_mapped) {
      if (inheritScratchpadFromParent(m_tiled) && !scratchpadOwnsOpeningGeometry()) {
        placeInUsableArea(m_initialRules.defaultPosition);
        if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
          scratchpad->syncViewPresentation(this);
        }
      }
      raiseToTop();
    }
    // The IPC window listing reports the parent.
    m_server->scheduleIpcWindowsEvent();
  }

  void View::recordOpeningParentRequest(bool parentRequested) {
    // Once mapped, wlroots' normal parent state is authoritative. Retire the
    // opening hint on any later request, especially an explicit null parent.
    m_openingParentRequested = !m_mapped && parentRequested;
  }

  bool View::openingParented() const { return shellParentRequested() || m_openingParentRequested; }

  void View::toggleFullscreen() {
    if (!roleInitialized()) {
      return;
    }
    const bool fullscreen = !scheduledFullscreen();
    m_xCompositorFullscreen = m_xsurface != nullptr && fullscreen;
    setFullscreen(fullscreen);
  }

  void View::applyDeferredUnfullscreen() {
    if (!m_deferredUnfullscreen.takeOnActivation() || !roleInitialized()) {
      return;
    }
    if (scheduledFullscreen() || currentFullscreen()) {
      kLog.debug("deferred unfullscreen applied on activation for '{}'", appId() != nullptr ? appId() : "?");
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
    wlr_scene_node_place_above(&m_server->pinnedTree()->node, &m_server->fullscreenTree()->node);
    setSceneParent(output != nullptr ? output->pinnedRoot() : m_server->pinnedTree());
    setNodeEnabled(true);
    raiseToTop();
  }

  void View::applyPinnedState() {
    m_pinned = true;
    m_server->scheduleIpcWindowsEvent();
    restorePinnedSceneParent();
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
      if (m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
        wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
      }
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewPinnedChanged(this);
    }
  }

  void View::togglePinned() { setPinned(!m_pinned, true); }

  void View::setPinned(bool pinned, bool focus) {
    if (!m_mapped
        || !roleInitialized()
        || (pinned && (scheduledFullscreen() || currentFullscreen()))
        || pinned == m_pinned) {
      return;
    }
    if (pinned) {
      m_restoreTiledAfterUnpin = m_tiled;
      if (m_tiled) {
        setFloating(true, false);
      }
      applyPinnedState();
      if (focus) {
        m_server->focusView(this);
      }
      refreshStateRuleEffects();
      // A float pins without touching the layout, so the occupancy questions need a pass of their own.
      if (m_workspace != nullptr) {
        m_workspace->markArrange();
      }
      return;
    }

    const bool restoreTiled = m_restoreTiledAfterUnpin;
    m_restoreTiledAfterUnpin = false;
    m_pinned = false;
    m_server->scheduleIpcWindowsEvent();
    if (restoreTiled) {
      setFloating(false, focus);
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPinnedChanged(this);
      }
      return;
    }
    if (m_workspace != nullptr) {
      setSceneParent(m_workspace->viewLayer(false));
      setOnActiveWorkspace(m_workspace->active());
      m_workspace->syncFloatingStack(this);
      m_workspace->syncViewPresentation(this);
    }
    setNodeEnabled(m_onActiveWorkspace);
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewPinnedChanged(this);
    }
    refreshStateRuleEffects();
    if (m_workspace != nullptr) {
      m_workspace->markArrange();
    }
  }

  void View::setFloating(bool floating, bool focus, TilePlacement placement) {
    if (!m_mapped || !roleInitialized()) {
      return;
    }
    kLog.debug(
        "set_floating '{}' [{}] -> {} (tiled={}, pinned={}, fs={})", appId() != nullptr ? appId() : "?",
        static_cast<const void*>(this), floating, m_tiled, m_pinned, scheduledFullscreen()
    );
    if (!floating && m_server->scratchpadManager() != nullptr && m_server->scratchpadManager()->contains(this)) {
      return;
    }
    // A no-op request must stay a no-op: a redundant "make tiled" call must not unfullscreen or cancel the size
    // animation.
    const bool wantTiled = !floating;
    if (m_tiled == wantTiled) {
      return;
    }
    if (m_maximizedToEdges) {
      setMaximizedToEdges(false, false);
    }
    m_floatingMaximized = false;
    const bool unpinning = !floating && m_pinned;
    if (unpinning) {
      m_pinned = false;
      m_restoreTiledAfterUnpin = false;
      m_restorePinnedAfterFullscreen = false;
      if (m_workspace != nullptr) {
        setSceneParent(m_workspace->viewLayer(false));
        setOnActiveWorkspace(m_workspace->active());
      }
    }
    cancelSizeAnimation();
    const bool fullscreen = scheduledFullscreen() || currentFullscreen();
    // Only the float direction leaves fullscreen (it owns its own scene tree). Re-tiling a fullscreen view keeps the
    // state and re-inserts it as a fullscreen column, so the client never sees a transient column-sized configure.
    if (floating && fullscreen) {
      setFullscreen(false, FullscreenExitLayout::DeferToCaller);
      // Remember to restore on the next re-tile. Set after setFullscreen,
      // which clears the flag on every leave-fullscreen path.
      m_refullscreenOnTile = true;
    }
    // Consume the memory: a client that itself left fullscreen while floating cleared it (setFullscreen(false) below
    // via its request), so this only fires for a float episode the client still considers fullscreen.
    const bool refullscreen = !floating && !fullscreen && m_refullscreenOnTile;
    m_refullscreenOnTile = floating && m_refullscreenOnTile;

    if (floating) {
      auto [keepWidth, keepHeight] = floatingRestoreSize();
      // Where the layout puts the tile, read before it leaves the layout. The drawn node lags behind a pending arrange
      // (after a move to another output it is still on the old one), so placing from it would depend on frame timing.
      const bool inLayout = m_workspace != nullptr && m_workspace->layout().columnOf(this) >= 0;
      if (inLayout) {
        m_workspace->flushArrange();
      }
      const wlr_box slot = inLayout ? m_workspace->layout().targetBox(this)
                                    : wlr_box{.x = layoutTargetX(), .y = layoutTargetY(), .width = 0, .height = 0};
      if (m_workspace != nullptr) {
        const int column = m_workspace->layout().columnOf(this);
        if (m_workspace->scrollingLayout() != nullptr && column >= 0) {
          ScrollingLayout* scrolling = m_workspace->scrollingLayout();
          m_savedScrollingExtentPx.reset();
          m_savedScrollingExtent = scrolling->widthFraction(column);
        }
        if (column >= 0 && m_workspace->layout().isFullWidth(column)) {
          m_workspace->layout().clearFullWidthState(column);
          setMaximizedState(false);
        }
        m_workspace->layoutDetach(this);
      }
      const int keepX = slot.x;
      const int keepY = slot.y;
      m_tiled = false;
      m_presentedTiledBox = {};
      if (m_workspace != nullptr) {
        setSceneParent(m_workspace->viewLayer(m_tiled));
        m_workspace->syncFloatingStack(this);
      }
      const wlr_box usable = floatingUsableArea();
      const SizeHints hints = sizeHints();
      if (m_pendingFloatingWidthPx) {
        keepWidth = clampWidth(*m_pendingFloatingWidthPx, hints);
        m_pendingFloatingWidthPx.reset();
        m_pendingFloatingWidth.reset();
      } else if (m_pendingFloatingWidth && usable.width > 0) {
        keepWidth = clampWidth(floatingFractionSize(*m_pendingFloatingWidth, usable.width), hints);
        m_pendingFloatingWidth.reset();
      }
      if (m_pendingFloatingHeightPx) {
        keepHeight = clampHeight(*m_pendingFloatingHeightPx, hints);
        m_pendingFloatingHeightPx.reset();
        m_pendingFloatingHeight.reset();
      } else if (m_pendingFloatingHeight && usable.height > 0) {
        keepHeight = clampHeight(floatingFractionSize(*m_pendingFloatingHeight, usable.height), hints);
        m_pendingFloatingHeight.reset();
      }
      // Do not clear xdg tiled edges: GTK/Qt often resize (CSD / preferred size) when
      // tiled state is dropped. Floating is a compositor layout concern.
      if (keepWidth > 0
          && keepHeight > 0
          && (scheduledSize().width != keepWidth || scheduledSize().height != keepHeight)) {
        requestFloatingSize(keepWidth, keepHeight);
      }
      beginResizeAnimation(keepWidth, keepHeight);
      int floatX = keepX + 50;
      int floatY = keepY + 50;
      bool usedPendingPosition = false;
      if (m_pendingFloatingPosition) {
        if (const auto positioned =
                getFloatingPosition(usable, m_pendingFloatingPosition, std::array{keepWidth, keepHeight})) {
          floatX = positioned->x;
          floatY = positioned->y;
          m_pendingFloatingPosition.reset();
          usedPendingPosition = true;
        }
      }
      if (!usedPendingPosition) {
        if (const auto restored = m_floating.restoredOrigin(usable)) {
          floatX = restored->x;
          floatY = restored->y;
        }
      }
      if (usable.width > 0 && usable.height > 0 && keepWidth > 0 && keepHeight > 0) {
        const int decoration = m_decoration.totalBorderWidth();
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
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewFloatingChanged(this);
      }
      refreshStateRuleEffects();
      return;
    }

    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = geometryBox();
    // A fullscreen geometry is not a floating size; remembering it would make
    // the next float episode restore output-sized dimensions.
    if (!fullscreen && geo.width > 0 && geo.height > 0) {
      m_floating.rememberSize(geo.width, geo.height);
    }
    m_floating.rememberPositionFraction({.x = layoutTargetX(), .y = layoutTargetY()}, usable);

    m_floating.clearSizeRequest();
    m_tiled = true;
    m_restorePinnedAfterFullscreen = false;
    // Restore the fullscreen the float toggle dropped BEFORE the layout attach: arrange then sizes the column to the
    // full output instead of a regular column width, and the client sees no transient windowed configure. setFullscreen
    // also reparents and disables borders.
    if (refullscreen) {
      setFullscreen(true);
    }
    const bool wantFullscreen = fullscreen || refullscreen;
    if (m_workspace != nullptr) {
      setSceneParent(homeTree());
      m_workspace->syncFloatingStack(this);
    }
    setTiledState(WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
    setMaximizedState(false);
    m_decoration.ensureBorders(m_contentTree);
    m_decoration.setBordersEnabled(!wantFullscreen);
    updateBorderGeometry();
    if (m_workspace != nullptr && placement == TilePlacement::Layout) {
      m_workspace->layoutAttach(this);
    }
    applyCornerRadius();
    updateShadow();
    if (focus) {
      m_server->focusView(this);
    }
    // setFullscreen(true) is not re-run on this path, so its scroll snap does not happen; without it the strip can rest
    // showing the neighbor column beside a viewport-wide fullscreen column.
    if (wantFullscreen && m_workspace != nullptr && placement == TilePlacement::Layout) {
      m_workspace->snapVisible(this);
      m_workspace->markArrange(false);
    }
    // Restore a saved extent only when this view owns the column it created. Joining a named column preserves that
    // column's established extent.
    if (!wantFullscreen && m_workspace != nullptr && m_workspace->scrollingLayout() != nullptr) {
      ScrollingLayout* scrolling = m_workspace->scrollingLayout();
      const int column = scrolling->columnOf(this);
      if (column >= 0) {
        const bool ownsExtent = !m_namedScrollingColumnName || m_ownsNamedScrollingColumnExtent;
        if (ownsExtent && m_savedScrollingExtentPx) {
          scrolling->setWidthFromPixels(column, m_workspace->scrollViewportExtent(), *m_savedScrollingExtentPx);
        } else if (ownsExtent && m_savedScrollingExtent) {
          scrolling->setWidthFraction(column, *m_savedScrollingExtent);
        }
        m_savedScrollingExtentPx.reset();
        m_savedScrollingExtent.reset();
      }
    }
    updateForeignState();
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      if (unpinning) {
        overview->onViewPinnedChanged(this);
      } else {
        overview->onViewFloatingChanged(this);
      }
    }
    refreshStateRuleEffects();
  }

  void View::setFullscreen(bool fullscreen, FullscreenExitLayout exitLayout) {
    m_deferredUnfullscreen.clear();
    const bool refreshHoverFocus = !fullscreen
        && m_mapped
        && m_onActiveWorkspace
        && (scheduledFullscreen() || currentFullscreen())
        && config().input.focus.followsMouse;
    kLog.debug(
        "set_fullscreen '{}' [{}] -> {} (tiled={}, ws_active={})", appId() != nullptr ? appId() : "?",
        static_cast<const void*>(this), fullscreen, m_tiled, m_workspace != nullptr && m_workspace->active()
    );
    if (fullscreen && m_maximizedToEdges) {
      setMaximizedToEdges(false, false);
      m_restoreMaximizedToEdges = true;
    }
    if (fullscreen && !m_tiled && !currentFullscreen()) {
      const wlr_box& geometry = geometryBox();
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
      m_xCompositorFullscreen = false;
      m_refullscreenOnTile = false;
    }
    const bool unpinning = fullscreen && m_pinned;
    if (fullscreen) {
      if (unpinning) {
        m_pinned = false;
        m_restorePinnedAfterFullscreen = true;
        if (m_workspace != nullptr) {
          setSceneParent(m_workspace->viewLayer(false));
          setOnActiveWorkspace(m_workspace->active());
        }
      }
      m_floating.clearSizeRequest();
    }
    // Entering fullscreen during a size animation grows from the size on screen, not from the committed one.
    const bool continuePresentation = fullscreen && sizeAnimating();
    const int presentedWidth = m_presentation.width();
    const int presentedHeight = m_presentation.height();
    cancelSizeAnimation();
    if (continuePresentation) {
      m_presentation.setSize(presentedWidth, presentedHeight);
    }
    setFullscreenState(fullscreen);
    setFadeAlpha(m_fadeAlpha);
    updateFullscreenPresentation(0, 0);
    if (fullscreen) {
      // scheduled.fullscreen is set; reparent to fullscreen layer.
      setSceneParent(homeTree());
      raiseToTop();
      // Snap scroll to the now viewport-wide column and reflow neighbors.
      if (m_workspace != nullptr) {
        m_workspace->snapVisible(this);
        // Arrange now, not at the next frame: the fullscreen configure must carry the output size, and the resize
        // animation must own the presentation before a fast client commits, or that commit shows its old buffer
        // centered on the backdrop. arrange() sends the full-output size even when this workspace is hidden.
        m_workspace->arrange(true);
      }
      if (!m_tiled || m_workspace == nullptr) {
        // Floating fullscreen is not part of the layout; size it directly.
        applyFullscreenLayout(true);
      }
    } else {
      setSceneParent(homeTree());
      if (!m_tiled && m_workspace != nullptr) {
        m_workspace->restackFloatingViews();
      } else {
        raiseToTop();
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
    updateBlur();
    updateShadow();
    if (!fullscreen && m_restoreMaximizedToEdges) {
      m_restoreMaximizedToEdges = false;
      // A compound transition owns its final state. Maximize-to-edges applies that state in its caller, while floating
      // must not carry a stale maximized state after detaching from the layout.
      if (exitLayout == FullscreenExitLayout::Immediate) {
        setMaximizedToEdges(true);
      }
    }
    if (!fullscreen) {
      // scheduled.fullscreen is already false; arrange into usable area (exclusive zones).
      if (m_tiled && m_workspace != nullptr) {
        if (exitLayout == FullscreenExitLayout::Immediate) {
          // wlroots has already scheduled the fullscreen-state configure. Arrange synchronously so its size is
          // replaced with the restored tile before that configure is sent, keeping state and geometry in one client
          // transition.
          m_workspace->snapVisible(this);
          m_workspace->arrange(true);
        } else {
          // A compound transition, such as floating or maximize-to-edges, sets its final geometry after this returns.
          m_workspace->markArrange(true);
        }
      } else if (!restoreFloating) {
        placeInUsableArea();
      }
    }
    if (!fullscreen && m_restorePinnedAfterFullscreen) {
      m_restorePinnedAfterFullscreen = false;
      applyPinnedState();
    }
    updateForeignState();
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      Output* output = m_workspace->group()->output();
      output->updateVrr();
      output->updateHdr();
    }
    if (unpinning) {
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPinnedChanged(this);
      }
    }
    if (fullscreen && m_mapped && m_onActiveWorkspace) {
      LayerSurface* layer = LayerSurface::fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface);
      if (layer != nullptr && layer->output() == currentOutput() && !layer->acceptsKeyboard()) {
        m_server->focusView(this);
      }
    }
    if (refreshHoverFocus) {
      // A client-side fullscreen exit, such as leaving a browser video, bypasses the compositor action that normally
      // invalidates hover focus. The restored scene may put another tile beneath a stationary pointer, so its next
      // eligible motion must re-run focus selection even without crossing a border.
      m_server->cursor()->invalidateHoverFocus();
    }
    refreshStateRuleEffects();
  }
} // namespace umbriel
