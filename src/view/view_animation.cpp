#include "config/config.h"
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
    void setCompositorOpacity(wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
      float opacity = *static_cast<float*>(data);
      if (wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer)) {
        if (const wlr_alpha_modifier_surface_v1_state* clientAlpha =
                wlr_alpha_modifier_v1_get_surface_state(sceneSurface->surface)) {
          opacity *= static_cast<float>(clientAlpha->multiplier);
        }
      }
      wlr_scene_buffer_set_opacity(buffer, opacity);
    }
  } // namespace

  using view_detail::toplevelSurfaceTreeNode;

  void View::cancelPositionAnimation() {
    // Freeze wherever the node currently sits; callers use this to take over
    // positioning (drags, layout snaps) without a jump.
    m_posX.snap(m_sceneTree->node.x);
    m_posY.snap(m_sceneTree->node.y);
  }

  bool View::fadeComposited() const {
    if (!m_fade.animating()) {
      return false;
    }
    return m_customFade || (!m_inScratchpad && effectRegistry().lifecycleEffect(AnimationEvent::WindowsIn) != nullptr);
  }

  float View::effectiveOpacity() const {
    // Overshooting curves can push this past [0, 1]; wlr_scene_buffer_set_opacity asserts.
    const float ruleOpacity = scheduledFullscreen() && config().appearance.opaqueFullscreen ? 1.0F : m_ruleOpacity;
    const float fade = fadeComposited() ? 1.0F : m_fadeAlpha;
    return std::clamp(
        fade * ruleOpacity * m_dragOpacity * m_overviewOpacity * static_cast<float>(m_focusDim.current()), 0.0F, 1.0F
    );
  }

  bool View::fullscreenOpaque() const {
    if (config().appearance.opaqueFullscreen) {
      return true;
    }
    if (m_ruleOpacity < 1.0F) {
      return false;
    }
    wlr_surface* surface = rootSurface();
    if (const wlr_alpha_modifier_surface_v1_state* clientAlpha = wlr_alpha_modifier_v1_get_surface_state(surface);
        clientAlpha != nullptr && clientAlpha->multiplier < 1.0) {
      return false;
    }
    const wlr_box& geometry = geometryBox();
    return geometry.width <= 0 || geometry.height <= 0 || !surfaceTransparent(surface, geometry);
  }

  void View::setFadeAlpha(float alpha) {
    // Overshooting curves can push this out of range; wlr_scene_buffer_set_opacity asserts opacity is in [0, 1].
    m_fadeAlpha = std::clamp(alpha, 0.0F, 1.0F);
    float effective = effectiveOpacity();
    wlr_scene_node_for_each_buffer(&m_contentTree->node, setCompositorOpacity, &effective);
    m_resizeCrossfade.applyOpacity(effective);
    m_decoration.setBorderRawColor(m_borderColorAnim.current(), effective);
    // The analytic fallback still follows the lifecycle fade. Shader-shaped
    // shadows get their opacity from captured pixels instead of this multiplier.
    const float shadowOpacity = fadeComposited() ? effective * m_fadeAlpha : effective;
    m_decoration.setAlpha(shadowOpacity, m_fadeAlpha * m_overviewOpacity);
    if (m_chromeAttachment != nullptr) {
      m_chromeAttachment->setAlpha(chromeAlpha());
    }
  }

  void View::setOverviewOpacity(float opacity) {
    const float clamped = std::clamp(opacity, 0.0F, 1.0F);
    if (clamped == m_overviewOpacity) {
      return;
    }
    m_overviewOpacity = clamped;
    setFadeAlpha(m_fadeAlpha);
  }

  void View::applyEffectiveOpacity() {
    if (m_sceneTree == nullptr) {
      return;
    }
    float effective = effectiveOpacity();
    if (effective >= 1.0F) {
      return;
    }
    wlr_scene_node_for_each_buffer(&m_contentTree->node, setCompositorOpacity, &effective);
    m_resizeCrossfade.applyOpacity(effective);
  }

  void View::flushPendingEffectiveOpacity() {
    if (!m_effectiveOpacityCommitPending) {
      return;
    }
    applyEffectiveOpacity();
    m_effectiveOpacityCommitPending = false;
  }

  void View::cancelFadeAnimation() {
    m_fade.snap(1.0);
    dropOpeningInset();
    setFadeAlpha(1.0F);
  }

  bool View::tiledOpeningActive() const {
    return m_mapped
        && !m_inScratchpad
        && m_tiled
        && !layoutFullscreen()
        && m_workspace != nullptr
        && m_workspace->layout().columnOf(this) >= 0
        && m_fade.animating()
        && m_fade.target() > m_fade.from();
  }

  bool View::fullscreenOpeningActive() const {
    return m_mapped && !m_inScratchpad && layoutFullscreen() && m_fade.animating() && m_fade.target() > m_fade.from();
  }

  wlr_box View::openingInsetBox(const wlr_box& box) const {
    wlr_box presented = box;
    if (openingInsetActive()) {
      const double progress = std::clamp(m_fade.current(), 0.0, 1.0);
      const double scale = std::lerp(m_openingScale, 1.0, progress);
      presented.width = static_cast<int>(std::lround(box.width * scale));
      presented.height = static_cast<int>(std::lround(box.height * scale));
      presented.x = box.x + (box.width - presented.width) / 2;
      presented.y = box.y
          + (box.height - presented.height) / 2
          + static_cast<int>(std::lround(std::lerp(m_openingSlide, 0.0, progress)));
    }
    presented.width = std::max(1, presented.width);
    presented.height = std::max(1, presented.height);
    return presented;
  }

  wlr_box View::fullscreenLayoutBox() const {
    if (m_workspace != nullptr) {
      const wlr_box box = m_workspace->fullscreenTargetBox(this);
      if (box.width > 0 && box.height > 0) {
        return box;
      }
    }
    return fullscreenArea();
  }

  std::optional<wlr_box> View::openingLayoutBox() const {
    if (!tiledOpeningActive() || m_presentedTiledBox.width <= 0 || m_presentedTiledBox.height <= 0) {
      return std::nullopt;
    }
    return m_presentedTiledBox;
  }

  void View::dropOpeningInset() {
    if (!openingInsetPending()) {
      return;
    }
    m_openingScale = 1.0;
    m_openingSlide = 0;
    if (!m_mapped) {
      return;
    }
    // The inset moved the node off its resting origin; the caller settles the size.
    if (layoutFullscreen()) {
      const wlr_box area = fullscreenLayoutBox();
      if (area.width > 0 && area.height > 0) {
        setScenePosition(area.x, area.y);
      }
      return;
    }
    if (m_tiled && m_workspace != nullptr && m_workspace->layout().columnOf(this) >= 0) {
      const wlr_box slot = m_workspace->presentedTiledBox(this);
      setScenePosition(slot.x, slot.y);
    }
  }

  wlr_box View::committedContentBox() const {
    wlr_box box = geometryBox();
    if (m_xsurface != nullptr || !m_tiled || currentFullscreen()) {
      return box;
    }
    // A client may ack a configure, render the new size into its buffer, and leave set_window_geometry at the old size
    // (Electron does, permanently). Presenting that stale box would crop the content the client just drew, so trust
    // the pixels: grow the box towards the size this view was configured to, never past what the surface holds.
    const wlr_surface_state& surface = rootSurface()->current;
    box.width = std::max(box.width, std::min(scheduledSize().width, surface.width - box.x));
    box.height = std::max(box.height, std::min(scheduledSize().height, surface.height - box.y));
    return box;
  }

  int View::presentedWidth(const wlr_box& target) const {
    if (sizeGrabActive()) {
      return target.width;
    }
    if (layoutPresentationOwned()) {
      return m_presentation.width();
    }
    if (currentFullscreen()) {
      return target.width;
    }
    return std::min(committedContentBox().width, target.width);
  }

  void View::trackPresentedSize(int width, int height) { m_presentation.track(width, height); }

  int View::presentedHeight(const wlr_box& target) const {
    if (sizeGrabActive()) {
      return target.height;
    }
    if (layoutPresentationOwned()) {
      return m_presentation.height();
    }
    if (currentFullscreen()) {
      return target.height;
    }
    return std::min(committedContentBox().height, target.height);
  }

  // Fullscreen chrome follows committed state. Transitions may scale the old buffer, while settled mismatched buffers
  // remain centered and cropped without distortion.

  void View::updateFullscreenPresentation(int width, int height) {
    m_presentation.updateFullscreen(
        currentFullscreen(), width, height, toplevelSurfaceTreeNode(m_contentTree, rootSurface()), geometryBox()
    );
  }

  void View::applyPresentedCrop(const wlr_box& content, const wlr_box& surfaceClip) {
    m_presentation.applyCrop(m_contentTree, rootSurface(), geometryBox(), content, surfaceClip);
  }

  void View::resetPresentedSurface() {
    m_presentation.resetCrop(m_contentTree, rootSurface());
    // Clear the subsurface clip so the next syncViewPresentation re-applies the resting clip through a real
    // reconfigure: an unchanged clip box early-outs and would leave the animated src/dst behind.
    setSurfaceTreeClip(nullptr);
  }

  void View::applyPresentedSize() {
    // A dragged window is out of the layout's hands, so it derives its own
    // presentation instead of going through the workspace below.
    if (const Cursor* cursor = m_server->cursor(); cursor != nullptr && cursor->isDraggingView(this)) {
      applyDragPresentation();
      return;
    }
    // Buffer scale + crop is derived in applyPresentation (applyPresentedCrop) via syncViewPresentation below, so the
    // animated size and the presented crop are always applied together instead of fighting over dest_size.
    const int width = m_presentation.width();
    const int height = m_presentation.height();
    updateBorderGeometry(width, height);
    if (!scheduledFullscreen()) {
      updateShadow(width, height);
    }
    updateBlur(width, height);
    syncOwnedPresentation();
  }

  void View::syncOwnedPresentation() {
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
      return;
    }
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(this)) {
      scratchpad->syncViewPresentation(this);
    }
  }

  void View::applyDragPresentation() {
    applyPresentation({
        .x = m_sceneTree->node.x,
        .y = m_sceneTree->node.y,
        .width = m_presentation.width(),
        .height = m_presentation.height(),
    });
  }

  void View::finishSizeAnimation() {
    const wlr_box content = committedContentBox();
    m_presentation.setSize(content.width, content.height);
    resetPresentedSurface();
    updateBorderGeometry();
    updateBlur();
    updateShadow();
    syncOwnedPresentation();
  }

  void View::cancelSizeAnimation() {
    if (!layoutPresentationOwned()) {
      return;
    }
    m_layoutPresentationHeld = false;
    m_tiledSizeRequest.reset();
    m_resizeCrossfade.discard();
    if (m_layoutMotion) {
      m_workspace->releaseLayoutMotion(this);
      m_layoutMotion = false;
    }
    dropOpeningInset();
    const wlr_box content = committedContentBox();
    m_presentation.snapTo(content.width, content.height);
    finishSizeAnimation();
  }

  bool View::sizeGrabActive() const {
    const Cursor* cursor = m_server->cursor();
    if (cursor == nullptr) {
      return false;
    }
    return cursor->grabbedView() == this || cursor->isResizingWorkspace(m_workspace);
  }

  bool View::sizeGrabTracksPointer() const {
    const Cursor* cursor = m_server->cursor();
    return cursor != nullptr && sizeGrabActive() && !cursor->isDraggingView(this);
  }

  void View::enterDragPresentation() {
    cancelPositionAnimation();
    m_dragOpacity = config().appearance.dragOpacity;
    setFadeAlpha(m_fadeAlpha);
    m_effectiveOpacityCommitPending = false;
    if (m_pinned) {
      wlr_scene_node_place_above(&m_server->dragTree()->node, &m_server->pinnedTree()->node);
    }
    setSceneParent(m_server->dragTree());
    setNodeEnabled(true);
    resetSurfaceClip();
    raiseToTop();
  }

  bool View::beginDragPhysics(double localX, double localY) {
    // Null while animations or physics are off, or when the program failed to compile: the window stays rigid.
    if (effectRegistry().deformationShader() == nullptr) {
      return false;
    }
    m_dragGrabX = localX;
    m_dragGrabY = localY;
    const std::optional<DragFit> fit = dragPhysicsFit();
    if (!fit) {
      return false;
    }
    m_dragBounds = fit->bounds;
    m_dragPhysics.begin(
        static_cast<float>(fit->bounds.width), static_cast<float>(fit->bounds.height), fit->grab[0], fit->grab[1],
        m_dragPhysics.active() ? m_dragPhysics.transitionId() : nextAnimationTransitionId()
    );
    return true;
  }

  void View::setDragPhysicsGrab(double localX, double localY) {
    m_dragGrabX = localX;
    m_dragGrabY = localY;
    fitDragPhysics(true);
  }

  void View::fitDragPhysics(bool grabMoved) {
    if (!m_dragPhysics.grabbed() && !m_dragPhysics.active()) {
      return;
    }
    const std::optional<DragFit> fit = dragPhysicsFit();
    if (!fit || (!grabMoved && wlr_box_equal(&fit->bounds, &m_dragBounds))) {
      return;
    }
    // The sheet spans the box the drag slot draws over, which follows presented resizes and decorations.
    m_dragBounds = fit->bounds;
    m_dragPhysics.resize(
        static_cast<float>(fit->bounds.width), static_cast<float>(fit->bounds.height), fit->grab[0], fit->grab[1]
    );
  }

  std::optional<View::DragFit> View::dragPhysicsFit() const {
    wlr_box bounds{};
    if (!wlr_scene_node_effect_bounds(&m_contentTree->node, &bounds)) {
      return std::nullopt;
    }
    // The bounds are content-tree-local; the grab is frame-local.
    const auto grab = DragPhysics::grabIn(
        static_cast<float>(bounds.x), static_cast<float>(bounds.y), static_cast<float>(bounds.width),
        static_cast<float>(bounds.height), m_dragGrabX - m_contentTree->node.x, m_dragGrabY - m_contentTree->node.y
    );
    return DragFit{.bounds = bounds, .grab = grab};
  }

  bool View::dragPhysicsOn(const Output* output) const {
    int x = 0;
    int y = 0;
    if (output == nullptr || !wlr_scene_node_coords(&m_contentTree->node, &x, &y)) {
      return false;
    }
    const int expand = dragPhysicsExpand();
    const wlr_box drawn{
        x + m_dragBounds.x - expand, y + m_dragBounds.y - expand, m_dragBounds.width + 2 * expand,
        m_dragBounds.height + 2 * expand
    };
    const wlr_box outputBox = output->layoutBox();
    wlr_box overlap{};
    return wlr_box_intersection(&overlap, &drawn, &outputBox);
  }

  int View::dragPhysicsExpand() const { return static_cast<int>(std::ceil(m_dragPhysics.displacementBound())) + 2; }

  void View::moveDragPhysics(double dx, double dy) {
    if (!m_dragPhysics.grabbed()) {
      return;
    }
    const bool wasActive = m_dragPhysics.active();
    m_dragPhysics.move(static_cast<float>(dx), static_cast<float>(dy));
    if (m_dragPhysics.active()) {
      if (!wasActive) {
        // Integration starts at the motion that wakes the sheet.
        m_dragPhysicsMsec = m_server->animationClockMsec();
      }
      scheduleFrame();
    }
  }

  void View::endDragPhysics() { m_dragPhysics.release(); }

  void View::restoreHomePresentation() {
    // The drag derived its own presented size and crop; drop them so the
    // resting presentation below is re-applied through a real reconfigure.
    resetPresentedSurface();
    m_dragOpacity = 1.0F;
    m_effectiveOpacityCommitPending = false;
    setFadeAlpha(m_fadeAlpha);
    wlr_scene_node_place_below(&m_server->dragTree()->node, &m_server->dragIconTree()->node);
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

    setSceneParent(homeTree());
    if (m_workspace == nullptr) {
      setNodeEnabled(m_mapped && m_onActiveWorkspace);
      return;
    }
    setNodeEnabled(m_mapped && m_onActiveWorkspace);
    m_workspace->restackFloatingViews();
    if (m_mapped) {
      m_workspace->syncViewPresentation(this);
    }
  }

  void View::beginResizeAnimation(int width, int height, bool allowFullscreen) {
    const Overview* overview = m_server->overview();
    const bool presentedInOverview = overview != nullptr && overview->active() && m_workspace != nullptr;
    const ScratchpadManager* scratchpad = m_server->scratchpadManager();
    const bool presentedInScratchpad = scratchpad != nullptr && scratchpad->contains(this);
    if (!m_mapped
        || (!m_onActiveWorkspace && !presentedInOverview)
        || (m_workspace == nullptr && !presentedInScratchpad && !allowFullscreen)
        || (!allowFullscreen && (scheduledFullscreen() || currentFullscreen()))
        || width <= 0
        || height <= 0) {
      return;
    }
    // Nothing presented yet (first map): the fade-in covers the appear.
    if (m_presentation.width() <= 0 || m_presentation.height() <= 0) {
      return;
    }
    if (sizeGrabTracksPointer() || (width == m_presentation.width() && height == m_presentation.height())) {
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
    setScenePosition(x, y);
  }

  void View::snapPosition(int x, int y) { setPosition(x, y); }

  void View::setLayoutTarget(int x, int y) {
    m_posX.snap(x);
    m_posY.snap(y);
    m_positioned = true;
  }

  void View::presentTiledBox(const wlr_box& box) {
    m_presentedTiledBox = box;
    presentBox(box);
  }

  void View::placePresentedBox(const wlr_box& box) {
    const wlr_box presented = openingInsetBox(box);
    setScenePosition(presented.x, presented.y);
    m_presentation.setSize(presented.width, presented.height);
  }

  void View::presentBox(const wlr_box& box) {
    if ((tiledOpeningActive() || fullscreenOpeningActive()) && m_presentation.animating()) {
      // windows_in owns lifecycle composition. A resize tween started by the admitting arrange would otherwise win
      // shader selection and apply a second windows_move over the workspace-owned logical presentation.
      m_presentation.snapTo(m_presentation.width(), m_presentation.height());
    }
    placePresentedBox(box);
    applyPresentedSize();
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onViewPresentationChanged(this);
    }
    syncAnimationEffects();
  }

  void View::deferTiledOpening() {
    if (m_tiledOpeningDeferred || !m_mapped) {
      return;
    }
    m_tiledOpeningDeferred = true;
    m_fade.snap(0.0);
    setFadeAlpha(0.0F);
    setNodeEnabled(false);
    syncAnimationEffects();
  }

  void View::resumeTiledOpening() {
    if (!m_tiledOpeningDeferred) {
      return;
    }
    m_tiledOpeningDeferred = false;
    if (!m_mapped) {
      return;
    }

    const auto& animation = config().animation;
    const auto& open = animation.windowsIn;
    m_customFade =
        animation.enabled && open.enabled && effectRegistry().animationEffect(AnimationEvent::WindowsIn) != nullptr;
    m_openingScale = 1.0;
    m_openingSlide = 0;
    if (!animation.enabled
        || !open.enabled
        || (open.style == "none" && effectRegistry().animationEffect(AnimationEvent::WindowsIn) == nullptr)) {
      m_fade.snap(1.0);
      setFadeAlpha(1.0F);
    } else {
      m_fade.snap(0.0);
      m_fade.retarget(1.0, open.durationMs, open.curve);
      setFadeAlpha(0.0F);
      if (!m_customFade && (open.style == "popin" || open.style == "zoom")) {
        m_openingScale = std::clamp(open.style == "zoom" ? 0.5 : open.scale, 0.0, 1.0);
      }
      scheduleFrame();
    }
    setNodeEnabled(m_onActiveWorkspace);
    syncAnimationEffects();
  }

  void View::beginLayoutMotion(float direction) {
    // The motion owns the presented size from here; a size tween still running would overwrite it on its next tick.
    m_presentation.snapTo(m_presentation.width(), m_presentation.height());
    m_layoutPresentationHeld = false;
    m_layoutMotion = true;
    m_layoutMotionDirection = direction;
  }

  void View::requestTiledSize(int width, int height) {
    m_tiledSizeRequest = TiledSizeRequest{
        .serial = configureSize(width, height),
        .width = width,
        .height = height,
        .base = currentSize(),
    };
  }

  bool View::settleTiledSizeRequest() {
    if (!m_tiledSizeRequest) {
      return true;
    }
    const TiledSizeRequest& request = *m_tiledSizeRequest;
    if (!configureSettled(request.serial)) {
      return false;
    }
    const wlr_box content = committedContentBox();
    const bool exact = content.width == request.width && content.height == request.height;
    // An X11 client with resize increments answers with the nearest size it accepts.
    const bool answered =
        m_xsurface != nullptr && (content.width != request.base.width || content.height != request.base.height);
    if (!exact && !answered) {
      return false;
    }
    m_tiledSizeRequest.reset();
    if (m_layoutPresentationHeld) {
      m_layoutPresentationHeld = false;
      m_presentation.setSize(content.width, content.height);
      m_presentation.snapTo(content.width, content.height);
      resetPresentedSurface();
    }
    return true;
  }

  void View::completeLayoutMotion(const wlr_box& target) {
    if (!m_layoutMotion && !m_layoutPresentationHeld) {
      return;
    }
    m_layoutMotion = false;
    m_layoutPresentationHeld = !settleTiledSizeRequest();
    if (m_layoutPresentationHeld) {
      presentTiledBox(target);
    } else {
      finishSizeAnimation();
    }
    syncAnimationEffects();
  }

  void View::endLayoutMotion() {
    if (!m_layoutMotion && !m_layoutPresentationHeld) {
      m_tiledSizeRequest.reset();
      return;
    }
    m_layoutMotion = false;
    m_layoutPresentationHeld = false;
    m_tiledSizeRequest.reset();
    if (tiledOpeningActive() && m_workspace != nullptr) {
      presentTiledBox(m_workspace->presentedTiledBox(this));
    } else {
      dropOpeningInset();
      finishSizeAnimation();
    }
    // Clears the windows_move effect on the frame the motion ends.
    syncAnimationEffects();
  }

  void View::animateFadeTo(float toAlpha, int durationMs, const AnimationCurve& curve) {
    m_customFade = m_inScratchpad && effectRegistry().animationEffect(AnimationEvent::Scratchpad) != nullptr;
    m_fade.snap(m_fadeAlpha);
    m_fade.retarget(toAlpha, durationMs, curve);
    scheduleFrame();
  }

  void View::setDragPosition(int x, int y) { setScenePosition(x, y); }

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

  void View::syncAnimationEffects(
      wlr_scene_tree* target, wlr_scene_node* border, wlr_scene_node* surface, const BorderEffectGate* gate,
      Output* cardOutput, float scale
  ) {
    const bool ownTrees = target == nullptr;
    if (ownTrees) {
      target = m_contentTree;
      if (m_decoration.borderTree() != nullptr) {
        border = &m_decoration.borderTree()->node;
      }
    }
    if (target == nullptr) {
      return;
    }
    if (!m_mapped) {
      wlr_scene_node_clear_animations(&target->node);
      if (ownTrees) {
        m_dragSlotBound = false;
      }
      if (border != nullptr) {
        wlr_scene_node_clear_animations(border);
      }
      m_effects.detach();
      return;
    }
    if (m_posX.animating() || m_posY.animating() || m_presentation.animating()) {
      const auto& movement = m_posX.animating() ? m_posX : (m_posY.animating() ? m_posY : m_presentation.animation());
      bindAnimationEffect(&target->node, AnimationEvent::WindowsMove, movement);
    } else if (
        const AnimatedValue* motion =
            m_layoutMotion && m_workspace != nullptr ? m_workspace->layoutMotionValue() : nullptr;
        motion != nullptr
    ) {
      bindAnimationEffect(&target->node, AnimationEvent::WindowsMove, *motion, m_layoutMotionDirection);
    } else {
      bindAnimationEffect(&target->node, AnimationEvent::WindowsMove, m_presentation.animation());
    }
    // Only the view's own content tree deforms; overview cards stay rigid.
    if (ownTrees && m_dragPhysics.active()) {
      fx_animation_parameters parameters{};
      parameters.progress = 1.0F;
      parameters.linear_progress = 1.0F;
      parameters.direction = 1.0F;
      parameters.transition_id = m_dragPhysics.transitionId();
      parameters.expand = dragPhysicsExpand();
      if (fx_uniform* sheet =
              fx_parameters_add_uniform(&parameters, "umbriel_deformation", FX_UNIFORM_VEC2, DragPhysics::kPoints)) {
        const DragPhysics::Sheet displacement = m_dragPhysics.normalizedDisplacement();
        for (int i = 0; i < DragPhysics::kPoints; ++i) {
          sheet->floats[i * 2] = displacement[i][0];
          sheet->floats[i * 2 + 1] = displacement[i][1];
        }
      }
      wlr_scene_node_set_animation(&target->node, FX_SLOT_DRAG, effectRegistry().deformationShader(), &parameters);
      m_dragSlotBound = true;
    } else if (ownTrees && m_dragSlotBound) {
      wlr_scene_node_set_animation(&target->node, FX_SLOT_DRAG, nullptr, nullptr);
      m_dragSlotBound = false;
    }
    bindAnimationEffect(&target->node, AnimationEvent::DimUnfocused, m_focusDim);
    bindAnimationEffect(&target->node, m_inScratchpad ? AnimationEvent::Scratchpad : AnimationEvent::WindowsIn, m_fade);
    wlr_scene_node_set_animation(
        &target->node, static_cast<unsigned>(m_inScratchpad ? AnimationEvent::WindowsIn : AnimationEvent::Scratchpad),
        nullptr, nullptr
    );
    bindAnimationEffect(border, AnimationEvent::Border, m_borderColorAnim, m_borderFocusedState ? 1.0F : -1.0F);
    // Persistent effects. With none configured this costs one string check per slot and never reads the clock.
    if (m_effects.configured() || effectRegistry().active()) {
      wlr_scene_node* captureSurface = nullptr;
      if (ownTrees && m_effects.needsSurface()) {
        surface = toplevelSurfaceTreeNode(m_contentTree, rootSurface());
        captureSurface =
            m_captureScene != nullptr ? toplevelSurfaceTreeNode(&m_captureScene->tree, rootSurface()) : nullptr;
      }
      const BorderEffectGate ownGate{
          .focused = m_borderFocusedState,
          .decorated = decorated(),
          .urgent = m_urgent,
          .fullscreen = scheduledFullscreen(),
      };
      Output* output = cardOutput != nullptr ? cardOutput : currentOutput();
      m_effects.apply({
          .surface = surface,
          .border = border,
          .captureSurface = captureSurface,
          .gate = gate != nullptr ? *gate : ownGate,
          .scale = scale,
          .seconds = m_effects.configured() && output != nullptr ? output->effectSeconds() : 0.0F,
#ifdef UMBRIEL_TEST_IPC
          .clockAdvancing = !m_server->animationClockFrozen(),
#endif
          .output = output,
          .outputBox = output != nullptr ? output->layoutBox() : wlr_box{},
      });
    }
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
      setScenePosition(cx, cy);
      // Clips are derived from the node's current position; refresh them as the
      // node moves or partial-visibility trims land displaced.
      syncOwnedPresentation();
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
      } else if (cursor == nullptr || !cursor->isDraggingView(this)) {
        // A drag keeps the size it retargeted to: settling on committed
        // geometry here would snap the window back until the client acks.
        finishSizeAnimation();
      }
    }

    if (m_fade.tick(nowMsec)) {
      m_customFade = m_customFade
          && m_fade.animating()
          && effectRegistry().animationEffect(m_inScratchpad ? AnimationEvent::Scratchpad : AnimationEvent::WindowsIn)
              != nullptr;
      const float rawAlpha = std::clamp(static_cast<float>(m_fade.current()), 0.0F, 1.0F);
      const bool builtInSlide = !m_inScratchpad
          && !m_customFade
          && config().animation.windowsIn.style == "slide"
          && effectRegistry().animationEffect(AnimationEvent::WindowsIn) == nullptr;
      // Keep the window visible through more of its travel so slide is clearly distinct from fade.
      setFadeAlpha(builtInSlide ? std::sqrt(rawAlpha) : rawAlpha);
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPresentationChanged(this);
      }
      // A fresh tiled opener owns its final slot while the fade runs. After a later admission, an already positioned
      // opener can instead receive intermediate logical boxes from windows_move. Popin/zoom applies its inset to either
      // box, and a custom shader stays composed over the lifecycle presentation.
      const bool finishingTiledOpen = m_mapped
          && !m_inScratchpad
          && m_tiled
          && !layoutFullscreen()
          && m_workspace != nullptr
          && m_workspace->layout().columnOf(this) >= 0
          && m_fade.target() > m_fade.from();
      // A fullscreen opener is presented inside the output box its layout assigned, which the arrange that follows
      // its map owns. Styles without an inset leave the node where that arrange put it.
      const bool insetFullscreenOpen =
          openingInsetPending() && m_mapped && !m_inScratchpad && layoutFullscreen() && m_fade.target() > m_fade.from();
      if (insetFullscreenOpen) {
        if (fullscreenOpeningActive()) {
          presentBox(fullscreenLayoutBox());
        } else {
          dropOpeningInset();
          finishSizeAnimation();
        }
      } else if (finishingTiledOpen && !m_layoutMotion) {
        if (tiledOpeningActive()) {
          presentTiledBox(m_workspace->presentedTiledBox(this));
        } else {
          dropOpeningInset();
          finishSizeAnimation();
        }
      }
      active = active || m_fade.animating();
    }
    if (m_focusDim.tick(nowMsec)) {
      setFadeAlpha(m_fadeAlpha);
      active = active || m_focusDim.animating();
    }
    if (m_resizeCrossfade.tick(nowMsec)) {
      active = true;
    }

    if (m_borderColorAnim.tick(nowMsec)) {
      m_decoration.setBorderRawColor(m_borderColorAnim.current(), effectiveOpacity());
      active = active || m_borderColorAnim.animating();
    }
    if (m_dragPhysics.active() || m_dragPhysics.grabbed()) {
      if (effectRegistry().deformationShader() == nullptr) {
        // Without the program (physics turned off), the window stays rigid for the rest of the drag.
        m_dragPhysics = DragPhysics{};
      } else if (m_dragPhysics.active()) {
        fitDragPhysics(false);
        const auto elapsed = static_cast<int64_t>(nowMsec - m_dragPhysicsMsec);
        m_dragPhysicsMsec = nowMsec;
        active = m_dragPhysics.tick(static_cast<double>(elapsed) / 1000.0) || active;
      }
    }
    syncAnimationEffects();
    if (!active && m_mapped && m_xsurface != nullptr) {
      syncXwaylandConfigure();
    }
    return active;
  }

  bool View::animatesOn(const Output* output) const {
    // A dragged window's sheet draws on every output its box reaches.
    if (m_dragPhysics.active() && dragPhysicsOn(output)) {
      return true;
    }
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
        || m_resizeCrossfade.active()
        || m_dragPhysics.active();
  }

  wlr_box View::fullscreenArea() const {
    Output* output = nullptr;
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      output = m_workspace->group()->output();
    }
    if (output == nullptr) {
      output = currentOutput();
    }
    wlr_output* wlrOutput = output != nullptr ? output->wlr() : m_server->preferredOutput();
    wlr_box fullArea{};
    wlr_output_layout_get_box(m_server->outputLayout(), wlrOutput, &fullArea);
    return fullArea;
  }

  CloseSnapshotId View::beginCloseAnimation() {
    const auto& animation = config().animation;
    if (!m_mapped
        || presentationSuppressed()
        || !m_onActiveWorkspace
        || !animation.enabled
        || !animation.windowsOut.enabled
        || m_server->sessionLocked()
        || (m_server->overview() != nullptr && m_server->overview()->active())) {
      return kInvalidCloseSnapshot;
    }

    Output* output =
        m_workspace != nullptr && m_workspace->group() != nullptr ? m_workspace->group()->output() : currentOutput();
    if (output == nullptr) {
      return kInvalidCloseSnapshot;
    }

    // Under the output's clipped root, so a snapshot of a view straddling the shared edge stays contained while it
    // fades. Server::removeOutput purges this output's snapshots before the Output is destroyed. The tree remains at
    // the captured presented box in output-root coordinates; the buffers keep their offsets inside the content subtree.
    wlr_scene_tree* snap = wlr_scene_tree_create(output->viewRoot());
    if (snap == nullptr) {
      return kInvalidCloseSnapshot;
    }
    wlr_scene_node_set_position(&snap->node, m_presentedBox.x, m_presentedBox.y);
    wlr_scene_tree* content = wlr_scene_tree_create(snap);
    if (content == nullptr) {
      wlr_scene_node_destroy(&snap->node);
      return kInvalidCloseSnapshot;
    }

    std::vector<BorderSnapshot> snapBorders;
    m_decoration.snapshotBorders(snap, m_borderColorAnim.current(), effectiveOpacity(), snapBorders);

    // Copy surface buffers.
    struct CopyCtx {
      wlr_scene_tree* snap;
      int rootX;
      int rootY;
      int buffersCopied;
    };
    CopyCtx ctx{content, m_contentTree->node.x, m_contentTree->node.y, 0};
    wlr_scene_node_for_each_buffer(
        &m_contentTree->node,
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
          wlr_scene_buffer_set_corner_box(copy, &src->corner_box);
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
      return kInvalidCloseSnapshot;
    }

    wlr_scene_node_copy_animations_for_snapshot(&snap->node, &m_contentTree->node);
    // Window and overlay effects live on the surface tree; the snapshot's content tree takes them over with time
    // frozen.
    if (m_effects.needsSurface()) {
      if (wlr_scene_node* surface = toplevelSurfaceTreeNode(m_contentTree, rootSurface())) {
        wlr_scene_node_copy_animations_for_snapshot(&content->node, surface);
      }
    }
    // A close snapshot owns its windows_out lifecycle. Keep a possible interrupted windows_in effect, but do not
    // freeze windows_move into the snapshot.
    wlr_scene_node_set_animation(&snap->node, static_cast<unsigned>(AnimationEvent::WindowsMove), nullptr, nullptr);
    const auto shadow = m_decoration.snapshotShadow(output->viewRoot(), &snap->node, m_decoration.shadowPooled());
    const CloseSnapshotId id = m_server->animateCloseSnapshot(
        output, snap, content, std::move(snapBorders), m_presentedBox, std::nullopt, shadow
    );
    wlr_output_schedule_frame(output->wlr());
    return id;
  }

  void View::setSurfaceTreeClip(const wlr_box* clip) {
    // Clip only the toplevel subsurface tree. Calling set_clip on the xdg root also stamps that clip onto popup
    // children (wrong coords → cut-off menus), and clearing clip on border/popup trees asserts when they have no
    // subsurface tree.
    if (wlr_scene_node* surfaceNode = toplevelSurfaceTreeNode(m_contentTree, rootSurface())) {
      wlr_scene_subsurface_tree_set_clip(surfaceNode, clip);
    } else if (clip == nullptr) {
      wlr_scene_subsurface_tree_set_clip(&m_contentTree->node, nullptr);
    }
    // A clip change runs wlroots' scene surface reconfigure, which resets the scene-buffer opacity to the client alpha.
    // This runs in the render path after the animation tick, so re-apply our fade/rule opacity.
    applyEffectiveOpacity();
  }

  void View::resetSurfaceClip() {
    // Fullscreen must not keep a copied tile clip (that freezes usable-area size and leaves a bar-sized gap). Use
    // scheduled (not current): on leave, scheduled clears immediately while current lags until the client acks.
    const bool fullscreen = scheduledFullscreen();
    const wlr_box content = committedContentBox();
    trackPresentedSize(content.width, content.height);
    if (!fullscreen && !m_tiled) {
      syncFloatingSurfaceClip();
      applyCornerRadius();
      updateBorderGeometry();
      return;
    }
    const wlr_box* clip = (!fullscreen && m_tiled) ? &content : nullptr;
    setSurfaceTreeClip(clip);
    applyCornerRadius();
    updateBorderGeometry();
    updateBlur();
    updateShadow();
  }

  void View::applyFullscreenLayout(bool animate) {
    const wlr_box fullArea = fullscreenArea();
    if (fullArea.width <= 0 || fullArea.height <= 0) {
      return;
    }
    if (scheduledSize().width != fullArea.width || scheduledSize().height != fullArea.height) {
      configureSize(fullArea.width, fullArea.height);
    }
    if (fullscreenOpeningActive()) {
      // windows_in owns the node position and the presented size until the fade ends; the layout owns only the box
      // they are derived from.
      setLayoutTarget(fullArea.x, fullArea.y);
      placePresentedBox(fullArea);
    } else if (animate) {
      beginResizeAnimation(fullArea.width, fullArea.height, true);
      animateTo(fullArea.x, fullArea.y);
    } else if (!m_posX.animating() && !m_posY.animating()) {
      setPosition(fullArea.x, fullArea.y);
    }

    // Present at the node's absolute position: a workspace mid-slide offsets its
    // whole tree on either axis, and the local origin does not carry that.
    int lx = 0;
    int ly = 0;
    wlr_scene_node_coords(&m_sceneTree->node, &lx, &ly);
    const wlr_box target{
        lx,
        ly,
        fullArea.width,
        fullArea.height,
    };
    applyPresentation(target);
  }

  void View::applyPresentation(const wlr_box& target) {
    updateFullscreenPresentation(target.width, target.height);
    const wlr_box& geometry = geometryBox();
    // Stay inside the tile while geometry lags configure (Electron often stays wide).
    const wlr_box content{
        .x = target.x,
        .y = target.y,
        .width = presentedWidth(target),
        .height = presentedHeight(target),
    };
    if (currentFullscreen()) {
      // The backdrop is the window's own letterbox, so it follows the presented box rather than the tile: a window
      // scaling into or out of fullscreen carries its surround instead of sitting in an output-wide one. The output's
      // clipped root scissors whatever hangs over a shared edge.
      m_presentation.setBackdropBox(0, 0, content.width, content.height);
    }
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
    if (layoutPresentationOwned() || sizeGrabActive()) {
      // The clip crops 1:1 in surface coordinates and caps the destination at the committed surface size, so it cannot
      // express an animated or interactive presented size. Program the buffer directly; the clip above keeps the buffer
      // node positioned at the visible box origin.
      applyPresentedCrop(content, surfaceClip);
    }
    m_resizeCrossfade.present(content.width, content.height);
    updateBorderGeometry(content.width, content.height);
    updateShadow();
    updateBlur(content.width, content.height);
  }
} // namespace umbriel
