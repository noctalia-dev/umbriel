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
    return m_server->usableAreaAt(m_sceneTree->node.x, m_sceneTree->node.y);
  }

  wlr_box View::floatingMaximizedBox(const wlr_box& usable) const {
    if (m_maximizedToEdges || usable.width <= 0 || usable.height <= 0) {
      return usable;
    }
    const LayoutStruts& struts = m_workspace != nullptr ? m_workspace->layoutConfig().struts : config().layout.struts;
    const int pad = m_workspace != nullptr ? m_workspace->layoutConfig().edgePad : config().layoutEdgePad();
    const wlr_box inside = applyLayoutStruts(usable, struts);
    const wlr_box box{
        .x = inside.x + pad,
        .y = inside.y + pad,
        .width = inside.width - (2 * pad),
        .height = inside.height - (2 * pad),
    };
    return box.width > 0 && box.height > 0 ? box : usable;
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

  std::optional<FloatingPoint> View::floatingClampTarget(FloatingPoint origin, int width, int height, bool resize) {
    if (m_tiled || !m_mapped || scheduledFullscreen() || scheduledMaximized() || sizeGrabActive()) {
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
    const FloatingPoint clamped =
        resize ? clampFloatingOriginForResize(origin, box, usable) : clampFloatingOrigin(origin, box, usable);
    if (clamped.x == origin.x && clamped.y == origin.y) {
      return std::nullopt;
    }
    return clamped;
  }

  void View::clampFloatingPosition() {
    if (m_posX.animating() || m_posY.animating()) {
      return;
    }
    const wlr_box& geo = geometryBox();
    const FloatingPoint origin{.x = m_sceneTree->node.x, .y = m_sceneTree->node.y};
    if (const auto clamped = floatingClampTarget(origin, geo.width, geo.height, false)) {
      setPosition(clamped->x, clamped->y);
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
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = geometryBox();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    const FloatingPoint origin = centeredOrigin(usable, geo.width, geo.height);
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
    }
    return origin;
  }

  void View::placeInUsableArea(const std::optional<WindowPosition>& position) {
    const wlr_box usable = floatingUsableArea();

    std::optional<FloatingPoint> origin = getFloatingPosition(usable, position);
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
    }
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
    const wlr_box& geo = geometryBox();
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
    dropMaximizedForResize();
    requestFloatingSize(width, height);
    beginResizeAnimation(width, height);
    animateTo(anchoredOrigin.x - geo.x, anchoredOrigin.y - geo.y);
    clampFloatingPositionForSize(width, height);
    finishFloatingResize();
  }

  void View::finishFloatingResize() { m_floating.endResize(); }

  void View::syncFloatingResizePosition() {
    if (!m_floating.anchor()) {
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
    if (m_tiled || !m_mapped || scheduledFullscreen() || scheduledMaximized()) {
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
      clampFloatingPosition();
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
