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
#include "scene/ssd_frame.h"
#include "scene/tab_label_cache.h"
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  int View::titleBarHeight() const {
    if (!wantsTitleBar(m_titleBarMode.value_or(config().appearance.titleBar.mode))
        || scheduledFullscreen()
        || (m_tiled && m_workspace != nullptr && m_workspace->inTabGroup(this))) {
      return 0;
    }
    return config().appearance.titleBar.height;
  }

  bool View::wantsTitleBar(TitleBarMode mode) const {
    if (mode == TitleBarMode::Off)
      return false;
    if (mode == TitleBarMode::Forced)
      return true;
    if (m_xsurface != nullptr)
      return m_xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    return m_xdgDecorationPreference.value_or(m_legacyDecorationPreference.value_or(mode == TitleBarMode::On));
  }

  void View::refreshTitleBar() {
    if (!m_mapped)
      return;
    updateBorderGeometry();
    applyCornerRadius();
    updateBlur();
    updateShadow();
    refitFloatingMaximized();
  }

  void View::setXdgDecorationPreference(std::optional<bool> serverSide) {
    if (m_xdgDecorationPreference == serverSide)
      return;
    m_xdgDecorationPreference = serverSide;
    refreshTitleBar();
  }

  void View::setLegacyDecorationPreference(std::optional<bool> serverSide) {
    if (m_legacyDecorationPreference == serverSide)
      return;
    m_legacyDecorationPreference = serverSide;
    refreshTitleBar();
  }

  void View::onSetDecorations(wl_listener* listener, void*) {
    View* self = wl_container_of(listener, self, m_setDecorations);
    self->handleSetDecorations();
  }

  void View::handleSetDecorations() { refreshTitleBar(); }

  SsdFramePoint View::ssdFramePointAt(double lx, double ly) const {
    if (m_ssdFrame == nullptr)
      return {};
    int x = 0;
    int y = 0;
    if (!wlr_scene_node_coords(&m_contentTree->node, &x, &y))
      return {};
    return m_ssdFrame->pointAt(lx - x, ly - y);
  }

  void View::syncSsdFrame() {
    if (!m_mapped)
      return;
    const int previousHeight = m_ssdFrame != nullptr ? m_ssdFrame->titleBarHeight() : 0;
    if (previousHeight != titleBarHeight()) {
      if (m_workspace != nullptr)
        m_workspace->markArrange();
      m_server->scheduleIpcWindowsEvent();
    }
    if ((titleBarHeight() > 0 || !m_tiled) && m_ssdFrame == nullptr) {
      m_ssdFrame = std::make_unique<SsdFrame>(chromeParent(), TabLabelCache::shared(), m_sceneTree);
    }
    if (m_ssdFrame == nullptr)
      return;
    const SizeHints hints = sizeHints();
    const bool fixed = hints.maxWidth > 0
        && hints.maxWidth == hints.minWidth
        && hints.maxHeight > 0
        && hints.maxHeight == hints.minHeight;
    m_ssdFrame->setTitle(title() != nullptr ? title() : "");
    m_ssdFrame->setResizable(!m_tiled);
    m_ssdFrame->setMaximizeEnabled(!fixed);
    syncTitleBarButtons();
    m_ssdFrame->setMaximized(scheduledMaximized() || floatingMaximized());
    m_ssdFrame->setFocused(m_borderFocusedState);
    m_ssdFrame->setAlphas(chromeAlpha(), blurAlpha());
    m_ssdFrame->setBlurOptions(blurOptions());
    m_ssdFrame->setBlurLive(m_server->cursor() != nullptr && m_server->cursor()->isDraggingView(this));
    const wlr_box content =
        m_presentedBox.width > 0 && m_presentedBox.height > 0 ? m_presentedBox : committedContentBox();
    layoutChromeAttachment(content.width, content.height);
    wlr_scene_node_raise_to_top(&m_attachmentTree->node);
  }

  void View::syncTitleBarButtons() {
    if (m_ssdFrame == nullptr)
      return;
    const std::vector<TitleBarButton>& configured =
        m_titleBarButtons ? *m_titleBarButtons : config().appearance.titleBar.buttons;
    const bool canMinimize = minimizable();
    std::array<TitleBarButton, kTitleBarButtonCount> shown{};
    size_t count = 0;
    for (const TitleBarButton button : configured) {
      if ((button != TitleBarButton::Minimize || canMinimize) && count < shown.size()) {
        shown[count++] = button;
      }
    }
    m_ssdFrame->setButtons(std::span(shown.data(), count));
  }

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
    if (m_ssdFrame != nullptr)
      m_ssdFrame->setFocused(focused);

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

    if (focusChanged && m_mapped) {
      applyDynamicRules();
    } else {
      updateBorderColor(focusChanged);
    }
  }

  void View::updateBorderColor(bool focusChanged) {
    const auto& animation = config().animation;
    const Config::Colors::Border& colors = m_decoration.borderColors();
    const auto& targetBase = m_borderFocusedState ? colors.focused : colors.unfocused;
    const auto& border = animation.border;
    const bool borderAnimates = m_mapped && animation.enabled && border.enabled && borderInset() > 0;
    const auto update = [&](AnimatedColor& value, const auto& target, bool focusEvent) {
      if (borderAnimates && (focusChanged || value.animating())) {
        if (value.target() != target || (focusChanged && focusEvent)) {
          value.retarget(target, border.durationMs, border.curve);
          scheduleFrame();
        }
      } else {
        value.snap(target);
      }
    };
    update(m_borderColorAnim, targetBase, true);
    update(m_outerBorderColorAnim, colors.outer, false);
    m_decoration.setBorderRawColors(m_borderColorAnim.current(), m_outerBorderColorAnim.current(), effectiveOpacity());
  }

  void View::settleFocusChrome() {
    if (!m_borderColorAnim.animating()
        && !m_outerBorderColorAnim.animating()
        && !m_focusDim.animating()
        && !m_ruleOpacity.animating()
        && !m_ruleBlur.animating()) {
      return;
    }
    m_borderColorAnim.snap(m_borderColorAnim.target());
    m_outerBorderColorAnim.snap(m_outerBorderColorAnim.target());
    m_focusDim.snap(m_focusDim.target());
    m_ruleOpacity.snap(m_ruleOpacity.target());
    m_ruleBlur.snap(m_ruleBlur.target());
    setFadeAlpha(m_fadeAlpha);
    updateBlur();
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
    // Layout targets, resize animations, and pending floating restores can lead committed geometry. Round the
    // presented frame while the client redraws.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating() || sizeGrabActive() || m_floating.pendingSize().has_value())
        && m_presentation.width() > 0
        && m_presentation.height() > 0;
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
          wlr_scene_buffer_set_corner_radii(
              buffer,
              ctx->view->titleBarHeight() > 0 ? corner_radii_new(0, 0, ctx->radius, ctx->radius)
                                              : corner_radii_all(ctx->radius)
          );
          wlr_scene_buffer_set_corner_box(buffer, ctx->radius > 0 ? &cornerBox : nullptr);
        },
        &ctx
    );
  }

  void View::updateBlur() {
    const wlr_box content = committedContentBox();
    updateBlur(content.width, content.height);
  }

  SurfaceBlurOptions View::blurOptions() const {
    auto options = m_decoration.blurOptions();
    // A disabled target must keep drawing until its focus transition reaches zero.
    options.enabled = m_ruleBlur.current() > 0.0 || m_ruleBlur.target() > 0.0;
    return options;
  }

  float View::blurAlpha() const {
    return m_fadeAlpha * m_overviewOpacity * static_cast<float>(std::clamp(m_ruleBlur.current(), 0.0, 1.0));
  }

  void View::updateBlur(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateBlur");
    if (m_ssdFrame != nullptr) {
      m_ssdFrame->setBlurOptions(blurOptions());
      m_ssdFrame->setAlphas(chromeAlpha(), blurAlpha());
    }
    if (currentFullscreen() && fullscreenOpaque()) {
      m_decoration.hideBlur();
      return;
    }
    const wlr_box nodeBox{0, 0, contentWidth, contentHeight};
    const int radius = surfaceRadius();
    // Content and its backdrop share a square seam below the title bar.
    const fx_corner_radii corners =
        titleBarHeight() > 0 ? corner_radii_new(0, 0, radius, radius) : corner_radii_all(radius);
    m_decoration.updateBlur(
        m_contentTree, rootSurface(), nodeBox, geometryBox(), corners, nullptr, effectiveOpacity(), blurAlpha(),
        blurOptions()
    );
  }

  void View::updateShadow() {
    if (scheduledFullscreen() || m_maximizedToEdges) {
      m_decoration.hideShadow();
      return;
    }
    // Shadows follow the presented frame through layout changes, animations, and pending floating restores.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating() || sizeGrabActive() || m_floating.pendingSize().has_value())
        && m_presentation.width() > 0
        && m_presentation.height() > 0;
    updateShadow(
        usePresentedSize ? m_presentation.width() : geometry.width,
        usePresentedSize ? m_presentation.height() : geometry.height
    );
  }

  void View::updateShadow(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateShadow");
    const int borderTotal = borderInset();
    m_decoration.updateShadow(
        contentWidth, contentHeight, borderTotal, decorated() ? m_decoration.cornerRadius() : 0, titleBarHeight()
    );
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
    syncSsdFrame();
    m_decoration.updateBorderGeometry(contentWidth, contentHeight, titleBarHeight());
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
    // Chrome joins the frame after the attachment tree, which must stay on top.
    wlr_scene_node_raise_to_top(&m_attachmentTree->node);
  }

  void View::layoutChromeAttachment(int contentWidth, int contentHeight) {
    m_chromeContentWidth = contentWidth;
    m_chromeContentHeight = contentHeight;
    if (m_contentTree == nullptr) {
      return;
    }
    const Output* output = currentOutput();
    ViewChromeGeometry geometry{
        .contentX = 0,
        .contentY = 0,
        .contentWidth = contentWidth,
        .contentHeight = contentHeight,
        .borderInset = borderInset(),
        .cornerRadius = m_maximizedToEdges ? 0 : decorationCornerRadius(),
        .titleBarHeight = titleBarHeight(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = !m_mapped || scheduledFullscreen() || currentFullscreen(),
    };
    if (m_ssdFrame != nullptr)
      m_ssdFrame->layout(geometry);
    geometry.suppressed = geometry.suppressed || m_maximizedToEdges;
    if (m_chromeAttachment != nullptr)
      m_chromeAttachment->layout(geometry);
  }

  void View::refreshConfigChrome() {
    syncSsdFrame();
    if (m_ssdFrame != nullptr)
      m_ssdFrame->reloadConfig();
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
    syncModalShade();
  }
} // namespace umbriel
