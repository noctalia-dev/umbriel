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
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <algorithm>
#include <ranges>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  wlr_box View::floatingUsableArea() const {
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      return m_workspace->group()->output()->usableArea();
    }
    if (m_server != nullptr && m_server->scratchpadManager() != nullptr) {
      if (Output* output = m_server->scratchpadManager()->outputFor(this)) {
        return output->usableArea();
      }
    }
    return m_server->usableAreaAt(layoutX(), layoutY());
  }

  wlr_box View::floatingMarginArea(const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    if (layout == nullptr && m_workspace != nullptr) {
      layout = &m_workspace->layoutConfig();
    }
    const LayoutStruts& struts = layout != nullptr ? layout->struts : config().layout.struts;
    const int pad = layout != nullptr ? layout->edgePad : config().layoutEdgePad();
    const wlr_box inside = applyLayoutStruts(usable, struts);
    const wlr_box inset{
        .x = inside.x + pad,
        .y = inside.y + pad,
        .width = inside.width - (2 * pad),
        .height = inside.height - (2 * pad),
    };
    return inset.width > 0 && inset.height > 0 ? inset : usable;
  }

  wlr_box View::floatingMaximizedBox(const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    if (usable.width <= 0 || usable.height <= 0) {
      return usable;
    }
    const wlr_box area =
        contentBoxInTile(m_maximizedToEdges ? usable : floatingMarginArea(usable, layout), titleBarHeight());
    // A client whose maximum size is below the area (a fixed-size splash) keeps that size, centered in the area
    // rather than pinned to its corner.
    const SizeHints hints = sizeHints();
    const int width = std::min(area.width, clampWidth(area.width, hints));
    const int height = std::min(area.height, clampHeight(area.height, hints));
    return {
        .x = area.x + ((area.width - width) / 2),
        .y = area.y + ((area.height - height) / 2),
        .width = width,
        .height = height,
    };
  }

  wlr_box View::openingUsableArea(Output* targetOutput) const {
    const wlr_cursor* cursor = m_server->cursor()->wlr();
    if (targetOutput == nullptr) {
      return m_server->usableAreaAt(cursor->x, cursor->y);
    }
    wlr_box usable = targetOutput->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), targetOutput->wlr(), &usable);
    }
    if (usable.width <= 0 || usable.height <= 0) {
      usable = m_server->usableAreaAt(cursor->x, cursor->y);
    }
    return usable;
  }

  FloatingPoint View::grabbableOrigin(FloatingPoint origin, const wlr_box& usable) const {
    return keepTitleBarReachable(clampFloatingOrigin(origin, geometryBox(), usable), titleBarHeight(), usable);
  }

  FloatingPoint View::settleFloatingDrop(int x, int y) {
    const wlr_box usable = floatingUsableArea();
    const FloatingPoint settled = grabbableOrigin({x, y}, usable);
    if (settled.x != x || settled.y != y) {
      animateTo(settled.x, settled.y);
    } else {
      setPosition(x, y);
    }
    rememberFloatingPosition();
    return settled;
  }

  std::optional<FloatingPoint> View::floatingClampTarget(FloatingPoint origin, int width, int height, bool resize) {
    if (m_tiled || !m_mapped || attached() || scheduledFullscreen() || scheduledMaximized() || sizeGrabActive()) {
      return std::nullopt;
    }
    if (Cursor* cursor = m_server->cursor(); cursor != nullptr && cursor->isDraggingView(this)) {
      return std::nullopt;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || width <= 0 || height <= 0) {
      return std::nullopt;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box box{.x = geo.x, .y = geo.y, .width = width, .height = height};
    const FloatingPoint clamped = keepTitleBarReachable(
        resize ? clampFloatingOriginForResize(origin, box, usable) : clampFloatingOrigin(origin, box, usable),
        titleBarHeight(), usable
    );
    if (clamped.x == origin.x && clamped.y == origin.y) {
      return std::nullopt;
    }
    return clamped;
  }

  void View::clampFloatingPosition() {
    // Mid-tween the clamp applies to where the tween is headed and retargets it. Skipping instead would spend the
    // usable-change check on a target that settles out of the margin, and the change is never re-checked.
    const bool animating = m_posX.animating() || m_posY.animating();
    const wlr_box& geo = geometryBox();
    const FloatingPoint origin{
        .x = animating ? layoutTargetX() : layoutX(),
        .y = animating ? layoutTargetY() : layoutY(),
    };
    if (const auto clamped = floatingClampTarget(origin, geo.width, geo.height, false)) {
      if (animating) {
        animateTo(clamped->x, clamped->y);
      } else {
        setPosition(clamped->x, clamped->y);
      }
    }
  }

  void View::clampFloatingPositionForSize(int width, int height) {
    const FloatingPoint origin{.x = layoutTargetX(), .y = layoutTargetY()};
    if (const auto clamped = floatingClampTarget(origin, width, height, true)) {
      animateTo(clamped->x, clamped->y);
    }
  }

  void View::rememberFloatingPosition() {
    if (m_tiled) {
      return;
    }
    m_floating.rememberPositionFraction({layoutTargetX(), layoutTargetY()}, floatingUsableArea());
  }

  void View::restoreFloatingPosition(bool rememberRestored) {
    if (m_tiled) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (const std::optional<FloatingPoint> restored = m_floating.restoredOrigin(usable)) {
      // The remembered fraction is proportional, so on a smaller output it can resolve outside the grabbable margin
      // (a scale jump can leave the window fully off). Placements land inside it, and the clamped origin — where the
      // window actually is — is what gets re-anchored.
      const FloatingPoint origin = grabbableOrigin(*restored, usable);
      animateTo(origin.x, origin.y);
      if (rememberRestored) {
        // Re-anchor on the new usable area so a second deliberate cross-output move lands proportionally again.
        m_floating.rememberPositionFraction(origin, usable);
      }
      return;
    }
    clampFloatingPosition();
  }

  bool View::centerFloating() {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = geometryBox();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    FloatingPoint origin = centeredOrigin(usable, geo.width, geo.height + titleBarHeight());
    origin.y += titleBarHeight();
    clearSnapState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  bool View::moveFloatingTo(const WindowPosition& position) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }

    const wlr_box usable = floatingUsableArea();

    const auto floatingPosition = getFloatingPosition(usable, position, std::nullopt);
    if (!floatingPosition) {
      return false;
    }
    clearSnapState();
    animateTo(floatingPosition->x, floatingPosition->y);
    m_floating.rememberPositionFraction(*floatingPosition, usable);
    return true;
  }

  bool View::moveFloatingBy(int deltaX, int deltaY) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    FloatingPoint origin = {.x = layoutTargetX() + deltaX, .y = layoutTargetY() + deltaY};
    origin = clampFloatingOrigin(origin, geo, usable);
    clearSnapState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  bool View::moveFloatingToEdge(wlr_edges edge) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    const wlr_box area = floatingMarginArea(usable);
    const FloatingPoint current{.x = layoutTargetX(), .y = layoutTargetY()};
    FloatingPoint origin = current;
    switch (edge) {
    case WLR_EDGE_LEFT:
      origin.x = area.x;
      break;
    case WLR_EDGE_RIGHT:
      origin.x = area.x + area.width - geo.width;
      break;
    case WLR_EDGE_TOP:
      origin.y = area.y;
      break;
    case WLR_EDGE_BOTTOM:
      origin.y = area.y + area.height - geo.height;
      break;
    case WLR_EDGE_NONE:
      return false;
    }
    if (origin.x == current.x && origin.y == current.y) {
      return false;
    }
    clearSnapState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  std::optional<FloatingPoint> View::getFloatingPosition(
      const wlr_box usable, const std::optional<WindowPosition>& position, const std::optional<std::array<int, 2>> size
  ) {
    if (usable.width <= 0 || usable.height <= 0) {
      return std::nullopt;
    }

    int w;
    int h;
    // Use size if provided
    if (size) {
      w = (*size)[0];
      h = (*size)[1];
    } else {
      const wlr_box& geo = geometryBox();
      w = geo.width > 0 ? geo.width : usable.width;
      h = geo.height > 0 ? geo.height : usable.height;
    }
    // Floats keep their own size; only center within the usable area.
    const int width = w;
    const int height = h;
    FloatingPoint origin = centeredOrigin(usable, width, height + titleBarHeight());
    origin.y += titleBarHeight();
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
    }
    return keepTitleBarReachable(origin, titleBarHeight(), usable);
  }

  void View::placeInUsableArea(const std::optional<WindowPosition>& position) {
    if (attached()) {
      return;
    }
    const wlr_box usable = floatingUsableArea();

    std::optional<std::array<int, 2>> placementSize;
    const bool automatic = !position
        && transientParent() == nullptr
        && m_workspace != nullptr
        && m_workspace->layoutMode() == LayoutMode::Floating
        && !m_initialRules.defaultFloatingWidth
        && !m_initialRules.defaultFloatingWidthPx
        && !m_initialRules.defaultFloatingHeight
        && !m_initialRules.defaultFloatingHeightPx
        && !scheduledFullscreen()
        && !scheduledMaximized();
    if (automatic) {
      const auto [width, height] = floatingSize();
      const wlr_box area = floatingMarginArea(usable);
      const int availableHeight = std::max(1, area.height - titleBarHeight());
      if (width > 0 && height > 0 && (width > area.width || height > availableHeight)) {
        const double scale =
            std::min(static_cast<double>(area.width) / width, static_cast<double>(availableHeight) / height);
        const SizeHints hints = sizeHints();
        placementSize = std::array{
            clampWidth(static_cast<int>(width * scale), hints), clampHeight(static_cast<int>(height * scale), hints)
        };
        requestFloatingSize((*placementSize)[0], (*placementSize)[1]);
      }
    }
    std::optional<FloatingPoint> origin = getFloatingPosition(usable, position, placementSize);
    if (!origin) {
      return;
    }
    if (position) {
      m_floating.rememberPositionFraction(*origin, usable);
    } else if (const View* parent = transientParent()) {
      // Where the parent is headed, not where its node is mid-animation right after it mapped. A fullscreen parent
      // shows over any panel, so the whole output counts as visible for it.
      const wlr_box shownIn = parent->scheduledFullscreen() ? parent->fullscreenArea() : usable;
      const wlr_box& geo = geometryBox();
      const int width = geo.width > 0 ? geo.width : usable.width;
      const int height = geo.height > 0 ? geo.height : usable.height;
      origin = centeredOverShown(parent->targetBox(), shownIn, width, height);
    } else if (m_workspace != nullptr && m_workspace->layoutMode() == LayoutMode::Floating) {
      const int border = borderInset();
      const int bar = titleBarHeight();
      int step = floatingCascadeStep(bar, border);
      std::vector<wlr_box> taken;
      const auto& stack = m_workspace->floatingStack();
      for (const View* other : std::views::reverse(stack)) {
        if (other != this
            && other->mapped()
            && other->floating()
            && other->positioned()
            && !other->pinned()
            && !other->scheduledFullscreen()) {
          const int otherBorder = other->borderInset();
          const int otherBar = other->titleBarHeight();
          const auto [otherWidth, otherHeight] = other->floatingSize();
          taken.push_back({
              .x = other->layoutTargetX() - otherBorder,
              .y = other->layoutTargetY() - otherBar - otherBorder,
              .width = otherWidth + 2 * otherBorder,
              .height = otherHeight + otherBar + 2 * otherBorder,
          });
          step = std::max(step, floatingCascadeStep(otherBar, otherBorder));
        }
      }
      const auto [width, height] = placementSize.value_or(floatingSize());
      const FloatingPoint frame = cascadeOrigin(
          {.x = origin->x - border, .y = origin->y - bar - border}, taken, step, usable, width + 2 * border,
          height + bar + 2 * border
      );
      origin = {.x = frame.x + border, .y = frame.y + bar + border};
    }
    *origin = keepTitleBarReachable(*origin, titleBarHeight(), usable);
    setPosition(origin->x, origin->y);
  }

  void View::requestFloatingSize(int width, int height) {
    if (m_xsurface != nullptr) {
      m_xFloatingRequestBase = currentSize();
    }
    m_floating.recordSizeRequest(width, height, configureSize(width, height));
  }

  std::array<int, 2> View::floatingSize() const {
    if (const auto& pending = m_floating.pendingSize()) {
      return *pending;
    }
    const wlr_box& geo = geometryBox();
    return {geo.width, geo.height};
  }

  std::optional<std::array<int, 2>> View::floatingAxisBasis(bool width) const {
    if (!m_mapped || m_tiled) {
      return std::nullopt;
    }
    const wlr_box usable = floatingUsableArea();
    const auto [basisWidth, basisHeight] = floatingSize();
    const int basis = width ? basisWidth : basisHeight;
    const int extent = width ? usable.width : usable.height;
    if (extent <= 0 || basis <= 0) {
      return std::nullopt;
    }
    return std::array{basis, extent};
  }

  std::optional<double> View::floatingFraction(bool width) const {
    const auto axis = floatingAxisBasis(width);
    if (!axis) {
      return std::nullopt;
    }
    return floatingSizeFraction((*axis)[0], (*axis)[1]);
  }

  bool View::resizeFloatingFractions(
      const std::optional<double>& widthFraction, const std::optional<double>& heightFraction
  ) {
    if (!m_mapped || m_tiled || currentFullscreen() || scheduledFullscreen()) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return false;
    }
    const SizeHints hints = sizeHints();
    const auto [basisWidth, basisHeight] = floatingSize();
    const int width =
        widthFraction ? clampWidth(floatingFractionSize(*widthFraction, usable.width), hints) : basisWidth;
    const int height =
        heightFraction ? clampHeight(floatingFractionSize(*heightFraction, usable.height), hints) : basisHeight;
    if (width <= 0 || height <= 0) {
      return false;
    }
    clearSnapState();
    dropMaximizedForResize();
    requestFloatingSize(width, height);
    beginResizeAnimation(width, height);
    clampFloatingPositionForSize(width, height);
    return true;
  }

  std::array<int, 2> View::floatingRestoreSize() const {
    if (m_floating.size()) {
      return *m_floating.size();
    }
    // First-time floats prefer the last acked or scheduled configure size,
    // then fall back to the layout target and committed geometry.
    int width = currentSize().width;
    int height = currentSize().height;
    if (width <= 0 || height <= 0) {
      width = scheduledSize().width;
      height = scheduledSize().height;
    }
    if ((width <= 0 || height <= 0) && m_workspace != nullptr) {
      const wlr_box target = m_workspace->layout().targetBox(this);
      if (target.width > 0 && target.height > 0) {
        const SizeHints hints = sizeHints();
        width = clampWidth(target.width, hints);
        height = clampHeight(target.height, hints);
      }
    }
    if (width <= 0 || height <= 0) {
      const wlr_box& geo = geometryBox();
      width = geo.width;
      height = geo.height;
    }
    return {width, height};
  }

  void View::beginFloatingResize(uint32_t edges) {
    clearSnapState();
    const wlr_box& geo = geometryBox();
    m_floating.beginResize(
        {.x = layoutX() + geo.x, .y = layoutY() + geo.y, .width = geo.width, .height = geo.height}, edges
    );
    syncFloatingResizePosition();
  }

  void View::resizeFloating(int width, int height) {
    syncFloatingResizePosition();
    requestFloatingSize(width, height);
  }

  void View::resizeFloatingEdge(uint32_t edges, double delta) {
    // Mirror the guard set of resizeFloatingFractions: a fullscreen view owns its
    // size, a tiled one has no floating box, and a view with no usable area has
    // nothing to size against.
    if (!m_mapped || m_tiled || currentFullscreen() || scheduledFullscreen()) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }
    const bool widthAxis = (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0;
    const auto current = floatingFraction(widthAxis);
    if (!current) {
      return;
    }
    // The same 0.1 fraction floor the fraction path enforces, so a hint-less
    // client cannot collapse to a single pixel.
    const double target = std::clamp(*current + delta, 0.1, 1.0);
    const SizeHints hints = sizeHints();
    const auto [basisWidth, basisHeight] = floatingSize();
    // A pending size and the position animation target form one logical box.
    // Anchor that box rather than the in-flight scene node, so repeated actions
    // keep the same far edge while the previous resize is still animating.
    const wlr_box& geo = geometryBox();
    const wlr_box anchor{
        .x = layoutTargetX() + geo.x,
        .y = layoutTargetY() + geo.y,
        .width = basisWidth,
        .height = basisHeight,
    };
    const int width = widthAxis ? clampWidth(floatingFractionSize(target, usable.width), hints) : basisWidth;
    const int height = widthAxis ? basisHeight : clampHeight(floatingFractionSize(target, usable.height), hints);
    if (width <= 0 || height <= 0) {
      return;
    }
    // Pin the edge opposite the named one, then take the same steps as the fraction path: drop a maximize, animate the
    // change, keep the window on screen. The origin animates with the size (same duration and curve), so the opposite
    // edge stays put while the client catches up.
    //
    // The anchor session stays open until the client answers the configure, so a commit at a different size still
    // recomputes the origin from the actual geometry. finishFloatingResize ends the session but keeps the anchor until
    // the request settles; that commit re-pins the edge and retires the anchor.
    const FloatingPoint anchoredOrigin =
        anchoredContentOrigin(anchor, edges, {.x = 0, .y = 0, .width = width, .height = height});
    m_floating.beginResize(anchor, edges);
    clearSnapState();
    dropMaximizedForResize();
    requestFloatingSize(width, height);
    beginResizeAnimation(width, height);
    animateTo(anchoredOrigin.x - geo.x, anchoredOrigin.y - geo.y);
    clampFloatingPositionForSize(width, height);
    finishFloatingResize();
  }

  void View::finishFloatingResize() { m_floating.endResize(); }

  void View::syncFloatingResizePosition() {
    // An attached dialog resizes around its centre, which its parent's layoutAttachments() keeps.
    if (!m_floating.anchor() || attached()) {
      return;
    }
    const wlr_box& geo = geometryBox();
    const FloatingPoint content = anchoredContentOrigin(*m_floating.anchor(), m_floating.edges(), geo);
    const int x = content.x - geo.x;
    const int y = content.y - geo.y;
    // A keybind resize animates the origin with the size, so a commit implying a different origin retargets that
    // animation (a matching target is left alone). A pointer drag places the origin directly to follow the cursor.
    if ((m_posX.animating() || m_posY.animating()) && !sizeGrabActive()) {
      if (layoutTargetX() != x || layoutTargetY() != y) {
        animateTo(x, y);
      }
      return;
    }
    setPosition(x, y);
  }

  void View::adoptFloatingClientSize() {
    if (m_tiled || !m_mapped || scheduledFullscreen() || scheduledMaximized() || m_snapRegion) {
      return;
    }
    if (!retireFloatingSizeRequest()) {
      return;
    }
    const wlr_box geo = geometryBox();
    if (geo.width <= 0 || geo.height <= 0) {
      return;
    }
    if (scheduledSize().width != geo.width || scheduledSize().height != geo.height) {
      // Once the latest compositor size request is committed, a floating
      // client owns its size. Direct assignment avoids an echo configure.
      adoptScheduledSize(geo.width, geo.height);
      const wlr_box usable = floatingUsableArea();
      if (m_workspace != nullptr
          && m_workspace->layoutMode() == LayoutMode::Floating
          && !m_floating.restoredOrigin(usable)
          && transientParent() == nullptr) {
        placeInUsableArea();
      } else {
        clampFloatingPosition();
      }
    }
  }

  void View::syncFloatingSurfaceClip() {
    if (m_tiled || scheduledFullscreen()) {
      return;
    }
    const wlr_box& geo = geometryBox();
    int width = scheduledSize().width;
    int height = scheduledSize().height;
    if (width <= 0) {
      width = currentSize().width;
    }
    if (height <= 0) {
      height = currentSize().height;
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
} // namespace umbriel
