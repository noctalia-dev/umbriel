#include "config/config.h"
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
#include "view/view_internal.h"
// clang-format off
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  bool View::decorated() const { return m_decoration.bordersVisible(); }

  int View::borderInset() const { return decorated() ? m_decoration.totalBorderWidth() : 0; }

  int View::surfaceRadius() const {
    return decorated() && !scheduledFullscreen() ? nestedRadius(m_decoration.cornerRadius(), borderInset()) : 0;
  }

  void View::setBorderFocused(bool focused) {
    const bool focusChanged = m_borderFocusedState != focused;
    m_borderFocusedState = focused;
    if (m_chromeAttachment != nullptr) {
      m_chromeAttachment->setFocused(focused);
    }

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

    const Config::Colors::Border& colors = m_decoration.borderColors();
    const std::array<float, 4>& targetBase = focused ? colors.focused : colors.unfocused;

    // A window without a drawn border has nothing to fade, and an invisible transition would still keep frames coming.
    const auto& border = animation.border;
    const bool borderAnimates = m_mapped && animation.enabled && border.enabled && borderInset() > 0;
    if (borderAnimates && focusChanged) {
      m_borderColorAnim.retarget(targetBase, border.durationMs, border.curve);
      scheduleFrame();
    } else if (borderAnimates && m_borderColorAnim.animating()) {
      // A repeated request for the same focus state, such as the pointer crossing outputs before an output-focus action
      // refocuses, keeps the running transition; only a new color target redirects it.
      if (m_borderColorAnim.target() != targetBase) {
        m_borderColorAnim.retarget(targetBase, border.durationMs, border.curve);
        scheduleFrame();
      }
    } else {
      m_borderColorAnim.snap(targetBase);
      m_decoration.setBorderColor(focused, effectiveOpacity());
    }

    if (focusChanged && m_mapped) {
      applyDynamicRules();
    }
  }

  void View::settleFocusChrome() {
    if (!m_borderColorAnim.animating() && !m_focusDim.animating()) {
      return;
    }
    m_borderColorAnim.snap(m_borderColorAnim.target());
    m_focusDim.snap(m_focusDim.target());
    setFadeAlpha(m_fadeAlpha);
    syncAnimationEffects();
  }

  void View::setUrgent(bool urgent) {
    if (m_urgent == urgent) {
      return;
    }
    m_urgent = urgent;
    if (m_workspace != nullptr) {
      m_workspace->updateUrgent();
      // A hidden tab can only ask for attention through its slot in the bar.
      m_workspace->tabs().memberChanged(this);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::applyCornerRadius() {
    // Every buffer under the toplevel's surface tree is rounded against one box, the window's content box, so the arc
    // lands on the window's corners whichever surface (main or subsurface) draws them. Popups are excluded: their
    // surface is its own root.
    const int radius = surfaceRadius();
    // A tiled target and an active resize animation can both lead committed geometry, so the box follows the presented
    // size in either case.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating()) && m_presentation.width() > 0 && m_presentation.height() > 0;
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
        usePresentedSize ? m_presentation.width() : geometry.width,
        usePresentedSize ? m_presentation.height() : geometry.height,
        m_contentTree->node.x,
        m_contentTree->node.y,
    };
    wlr_scene_node_for_each_buffer(
        &m_contentTree->node,
        [](wlr_scene_buffer* buffer, int sx, int sy, void* data) {
          auto* ctx = static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr
              || wlr_surface_get_root_surface(sceneSurface->surface) != ctx->view->rootSurface()) {
            return;
          }

          // The iterator accumulates positions from the node it was handed, so subtracting the tree's own position
          // yields tree-local coordinates. The xdg scene helper places the surface tree at (-geometry.x, -geometry.y),
          // which puts the content box at the tree origin, and the corner box is node-relative.
          const wlr_box cornerBox{
              ctx->treeX - sx,
              ctx->treeY - sy,
              ctx->contentWidth,
              ctx->contentHeight,
          };
          // Always set, zeros included: a window that lost its radius must lose the box with it.
          wlr_scene_buffer_set_corner_radii(buffer, corner_radii_all(ctx->radius));
          wlr_scene_buffer_set_corner_box(buffer, ctx->radius > 0 ? &cornerBox : nullptr);
        },
        &ctx
    );
  }

  void View::updateBlur() {
    const wlr_box content = committedContentBox();
    updateBlur(content.width, content.height);
  }

  void View::updateBlur(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateBlur");
    if (currentFullscreen() && fullscreenOpaque()) {
      m_decoration.hideBlur();
      return;
    }
    const wlr_box nodeBox{0, 0, contentWidth, contentHeight};
    m_decoration.updateBlur(
        m_contentTree, rootSurface(), nodeBox, geometryBox(), surfaceRadius(), nullptr, effectiveOpacity(), m_fadeAlpha
    );
  }

  void View::updateShadow() {
    if (scheduledFullscreen() || m_maximizedToEdges) {
      m_decoration.hideShadow();
      return;
    }
    // Tiled targets and active resize animations can lead committed geometry,
    // so their shadows must follow the presented size.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating()) && m_presentation.width() > 0 && m_presentation.height() > 0;
    updateShadow(
        usePresentedSize ? m_presentation.width() : geometry.width,
        usePresentedSize ? m_presentation.height() : geometry.height
    );
  }

  void View::updateShadow(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateShadow");
    const int borderTotal = borderInset();
    m_decoration.updateShadow(contentWidth, contentHeight, borderTotal, decorated() ? m_decoration.cornerRadius() : 0);
    m_decoration.setShadowAnimationSource(&m_contentTree->node);
  }

  void View::showDecorations(bool enabled) {
    m_decoration.ensureBorders(m_contentTree);
    m_decoration.setBordersEnabled(enabled);
    updateBorderGeometry();
    applyCornerRadius();
    updateBlur();
    updateShadow();
  }

  void View::updateBorderGeometry() {
    const wlr_box content = committedContentBox();
    updateBorderGeometry(content.width, content.height);
  }

  void View::updateBorderGeometry(int contentWidth, int contentHeight) {
    m_decoration.updateBorderGeometry(contentWidth, contentHeight);
    layoutChromeAttachment(contentWidth, contentHeight);
  }

  void View::setChromeAttachment(std::unique_ptr<ViewChromeAttachment> attachment) {
    m_chromeAttachment = std::move(attachment);
    if (m_chromeAttachment == nullptr) {
      return;
    }
    m_chromeAttachment->setFocused(m_borderFocusedState);
    m_chromeAttachment->setAlpha(chromeAlpha());
    if (m_chromeContentWidth <= 0 || m_chromeContentHeight <= 0) {
      const wlr_box content = committedContentBox();
      layoutChromeAttachment(content.width, content.height);
    } else {
      layoutChromeAttachment(m_chromeContentWidth, m_chromeContentHeight);
    }
  }

  void View::layoutChromeAttachment(int contentWidth, int contentHeight) {
    m_chromeContentWidth = contentWidth;
    m_chromeContentHeight = contentHeight;
    if (m_chromeAttachment == nullptr || m_contentTree == nullptr) {
      return;
    }
    const Output* output = currentOutput();
    m_chromeAttachment->layout({
        .contentX = m_contentTree->node.x,
        .contentY = m_contentTree->node.y,
        .contentWidth = contentWidth,
        .contentHeight = contentHeight,
        .borderInset = borderInset(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = !m_mapped || m_maximizedToEdges || scheduledFullscreen() || currentFullscreen(),
    });
  }

  void View::refreshConfigChrome() {
    if (m_chromeAttachment != nullptr) {
      m_chromeAttachment->reloadConfig();
    }
    m_focusDimInitialized = false;
    // Temporary unfocus would consume pool selections during an unrelated reload.
    setBorderFocused(m_borderFocusedState);
    updateBorderGeometry();
    applyCornerRadius();
    applyDynamicRules();
    if (notifyAloneStateChanged() && m_workspace != nullptr) {
      m_workspace->markArrange();
    }
    updateShadow();
    reloadBackdropColor();
  }
} // namespace umbriel
