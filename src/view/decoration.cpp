#include "view/decoration.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"

// clang-format off
#include "wlr.h"
// clang-format on

namespace umbriel {

  // Borders
  void ViewDecoration::ensureBorders(wlr_scene_tree* parent) {
    if (m_borderTree != nullptr) {
      return;
    }
    m_borderTree = wlr_scene_tree_create(parent);
    float innerColor[4];
    float outerColor[4];
    premultiplied(innerColor, config().colors.border.unfocused, 1.0F);
    premultiplied(outerColor, config().colors.border.outer, 1.0F);
    m_border = wlr_scene_border_create(m_borderTree, innerColor, outerColor);
    // The punched hole protects client content, so keep the border above the
    // toplevel surface that would otherwise cover its outermost pixels.
    wlr_scene_node_raise_to_top(&m_borderTree->node);
  }

  bool ViewDecoration::bordersVisible() const { return m_borderTree != nullptr && m_borderTree->node.enabled; }

  void ViewDecoration::setBordersEnabled(bool enabled) {
    if (m_borderTree != nullptr) {
      wlr_scene_node_set_enabled(&m_borderTree->node, enabled);
    }
  }

  void ViewDecoration::updateBorderGeometry(int contentWidth, int contentHeight) {
    if (m_border == nullptr) {
      return;
    }

    applyBorderGeometry(
        m_border,
        makeBorderRing(contentWidth, contentHeight, m_cornerRadius, m_borderWidth, m_outerBorderWidth, m_borderPadding),
        m_borderWidth, m_outerBorderWidth
    );
  }

  void ViewDecoration::setBorderColor(bool focused, float alpha) {
    if (m_borderTree == nullptr) {
      return;
    }
    setBorderRawColor(focused ? m_borderColors.focused : m_borderColors.unfocused, alpha);
  }

  void ViewDecoration::setBorderRawColor(const std::array<float, 4>& baseColor, float alpha) {
    if (m_border == nullptr) {
      return;
    }
    float innerColor[4];
    float outerColor[4];
    premultiplied(innerColor, baseColor, alpha);
    premultiplied(outerColor, m_borderColors.outer, alpha);
    wlr_scene_border_set_colors(m_border, innerColor, outerColor);
  }

  bool ViewDecoration::borderGeometryStale(int contentWidth, int contentHeight) const {
    if (m_border == nullptr) {
      return false;
    }
    const BorderRing ring =
        makeBorderRing(contentWidth, contentHeight, m_cornerRadius, m_borderWidth, m_outerBorderWidth, m_borderPadding);
    return m_border->width != ring.box.width || m_border->height != ring.box.height;
  }

  void ViewDecoration::snapshotBorders(
      wlr_scene_tree* snapshot, const std::array<float, 4>& innerColor, float opacity, std::vector<BorderSnapshot>& out
  ) const {
    if (!bordersVisible() || m_border == nullptr) {
      return;
    }
    // Straight colours at the opacity the ring is drawn with right now, so the fade starts from what is on screen
    // and stays in step with the content buffers, which keep their current opacity as their base.
    snapshotBorder(
        snapshot, *m_border, m_borderTree->node.x + m_border->node.x, m_borderTree->node.y + m_border->node.y,
        &m_borderTree->node,
        {
            .innerColor = innerColor,
            .outerColor = m_borderColors.outer,
            .innerWidth = m_borderWidth,
            .outerWidth = m_outerBorderWidth,
            .cornerRadius = m_cornerRadius,
            .padding = m_borderPadding,
        },
        opacity, out
    );
  }

  bool ViewDecoration::setBorderPadding(int padding) {
    if (padding == m_borderPadding) {
      return false;
    }
    m_borderPadding = padding;
    return true;
  }

  bool ViewDecoration::applyRule(const ResolvedWindowRule& rule) {
    const auto& border = config().colors.border;
    m_borderColors = {
        .focused = rule.borderColorFocused.value_or(border.focused),
        .unfocused = rule.borderColorUnfocused.value_or(border.unfocused),
        .outer = rule.borderColorOuter.value_or(border.outer),
    };
    m_blurOptions = SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(rule.blurIgnoreAlpha.value_or(0.0)),
        .enabled = rule.blur.value_or(false),
        .optimized = rule.blurOptimized,
    };
    m_popupBlurOptions = SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(rule.blurIgnoreAlpha.value_or(0.0)),
        .enabled = rule.blurPopups.value_or(false),
        .optimized = rule.blurOptimized,
    };
    const auto& appearance = config().appearance;
    const int borderWidth = rule.borderWidth.value_or(appearance.borderWidth);
    const int outerBorderWidth = rule.outerBorderWidth.value_or(appearance.outerBorderWidth);
    const int cornerRadius = rule.cornerRadius.value_or(appearance.cornerRadius);
    const bool changed = borderWidth != m_borderWidth
        || outerBorderWidth != m_outerBorderWidth
        || cornerRadius != m_cornerRadius
        || rule.shadow != m_ruleShadow;
    m_borderWidth = borderWidth;
    m_outerBorderWidth = outerBorderWidth;
    m_cornerRadius = cornerRadius;
    m_ruleShadow = rule.shadow;
    m_shadow.setEnabled(rule.shadow);
    return changed;
  }

  // Blur
  void ViewDecoration::updateBlur(
      wlr_scene_tree* tree, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& geometry, int radius,
      const wlr_box* clip, float surfaceOpacity, float blurAlpha
  ) {
    m_blur.setAlpha(blurAlpha);
    m_blur.update(tree, surface, nodeBox, geometry, radius, clip, m_blurOptions, surfaceOpacity);
  }

  void ViewDecoration::hideBlur() { m_blur.hide(); }

  // Shadow
  void ViewDecoration::createShadow(wlr_scene_tree* frame) {
    m_shadowContainer = wlr_scene_tree_create(frame);
    wlr_scene_node_lower_to_bottom(&m_shadowContainer->node);
  }

  void ViewDecoration::poolShadow(wlr_scene_tree* frame, wlr_scene_tree* pool, int x, int y, bool enabled) {
    if (m_shadowContainer == nullptr) {
      return;
    }
    if (pool == nullptr) {
      if (!m_shadowPooled) {
        return;
      }
      wlr_scene_node_reparent(&m_shadowContainer->node, frame);
      wlr_scene_node_lower_to_bottom(&m_shadowContainer->node);
      wlr_scene_node_set_position(&m_shadowContainer->node, 0, 0);
      wlr_scene_node_set_enabled(&m_shadowContainer->node, true);
      m_shadowPooled = false;
      return;
    }
    wlr_scene_node_reparent(&m_shadowContainer->node, pool);
    wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
    m_shadowPooled = true;
  }

  void ViewDecoration::setShadowPosition(int x, int y) {
    if (m_shadowPooled) {
      wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    }
  }

  void ViewDecoration::setShadowEnabled(bool enabled) {
    if (m_shadowPooled) {
      wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
    }
  }

  void ViewDecoration::updateShadow(int contentWidth, int contentHeight, int borderInset, int cornerRadius) {
    if (m_shadowContainer == nullptr) {
      return;
    }
    m_shadow.update(m_shadowContainer, contentWidth, contentHeight, borderInset, cornerRadius);
  }

  void ViewDecoration::hideShadow() { m_shadow.hide(); }

  void ViewDecoration::setAlpha(float decorationAlpha, float blurAlpha) {
    m_shadow.setAlpha(decorationAlpha);
    m_blur.setAlpha(blurAlpha);
  }

  void ViewDecoration::hideEffects() {
    m_blur.hide();
    m_shadow.hide();
  }

} // namespace umbriel
