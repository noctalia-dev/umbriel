#include "view/decoration.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"

extern "C" {
#include <umbrielfx/render/animation.h>
}

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
      m_nineRect.setEnabled(enabled && config().appearance.useNineRect);
    }
  }

  void ViewDecoration::updateBorderGeometry(int contentWidth, int contentHeight) {
    if (m_border == nullptr) {
      return;
    }

    const auto& appearance = config().appearance;
    wlr_scene_node_set_enabled(&m_border->node, !appearance.useNineRect);
    if (appearance.useNineRect) {
      m_nineRect.update(m_borderTree, appearance.nineRect.asset, contentWidth, contentHeight);
      m_nineRect.setEnabled(m_borderTree->node.enabled);
      return;
    }
    m_nineRect.clear();
    applyBorderGeometry(
        m_border,
        makeBorderRing(
            contentWidth, contentHeight, appearance.cornerRadius, appearance.borderWidth, appearance.outerBorderWidth
        ),
        appearance.borderWidth, appearance.outerBorderWidth
    );
  }

  void ViewDecoration::setBorderColor(bool focused, bool scratchpad, float alpha) {
    if (m_borderTree == nullptr) {
      return;
    }
    const auto& baseColor = scratchpad
        ? (focused ? config().colors.border.scratchpadFocused : config().colors.border.scratchpadUnfocused)
        : (focused ? config().colors.border.focused : config().colors.border.unfocused);
    setBorderRawColor(baseColor, alpha);
  }

  void ViewDecoration::setBorderRawColor(const std::array<float, 4>& baseColor, float alpha) {
    if (m_border == nullptr) {
      return;
    }
    m_nineRect.setAlpha(alpha);
    m_nineRect.setTint(baseColor);
    float innerColor[4];
    float outerColor[4];
    premultiplied(innerColor, baseColor, alpha);
    premultiplied(outerColor, config().colors.border.outer, alpha);
    wlr_scene_border_set_colors(m_border, innerColor, outerColor);
  }

  bool ViewDecoration::borderGeometryStale(int contentWidth, int contentHeight) const {
    if (m_border == nullptr) {
      return false;
    }
    const auto& appearance = config().appearance;
    if (appearance.useNineRect)
      return m_nineRect.stale(contentWidth, contentHeight, appearance.nineRect.asset);
    const BorderRing ring = makeBorderRing(
        contentWidth, contentHeight, appearance.cornerRadius, appearance.borderWidth, appearance.outerBorderWidth
    );
    return m_border->width != ring.box.width || m_border->height != ring.box.height;
  }

  void ViewDecoration::snapshotBorders(
      wlr_scene_tree* snapshot, const std::array<float, 4>& innerColor, float opacity, std::vector<BorderSnapshot>& out
  ) const {
    if (!bordersVisible() || m_border == nullptr || config().appearance.useNineRect) {
      return;
    }

    wlr_scene_border* copy = wlr_scene_border_create(snapshot, m_border->inner_color, m_border->outer_color);
    if (copy == nullptr) {
      return;
    }
    wlr_scene_border_set_geometry(
        copy, m_border->width, m_border->height, m_border->inner_width, m_border->outer_width, m_border->clipped_region,
        m_border->seam_corners, m_border->outer_corners
    );
    wlr_scene_node_set_position(
        &copy->node, m_borderTree->node.x + m_border->node.x, m_borderTree->node.y + m_border->node.y
    );
    wlr_scene_node_copy_animations_for_snapshot(&copy->node, &m_borderTree->node);
    // Straight colours at the opacity the ring is drawn with right now, so the fade starts from what is on screen
    // and stays in step with the content buffers, which keep their current opacity as their base.
    BorderSnapshot captured{.node = copy, .innerColor = innerColor, .outerColor = config().colors.border.outer};
    captured.innerColor[3] *= opacity;
    captured.outerColor[3] *= opacity;
    out.push_back(captured);
  }

  // Blur
  void ViewDecoration::applyRule(const ResolvedWindowRule& rule) {
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
  }

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
      const auto insets =
          config().appearance.useNineRect && bordersVisible() ? config().appearance.frameInsets() : FrameInsets{};
      wlr_scene_node_set_position(&m_shadowContainer->node, -insets.left, -insets.top);
      wlr_scene_node_set_enabled(&m_shadowContainer->node, true);
      m_shadowPooled = false;
      return;
    }
    wlr_scene_node_reparent(&m_shadowContainer->node, pool);
    const auto inset =
        config().appearance.useNineRect && bordersVisible() ? config().appearance.frameInsets() : FrameInsets{};
    wlr_scene_node_set_position(&m_shadowContainer->node, x - inset.left, y - inset.top);
    wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
    m_shadowPooled = true;
  }

  void ViewDecoration::setShadowPosition(int x, int y) {
    if (m_shadowPooled) {
      const auto inset =
          config().appearance.useNineRect && bordersVisible() ? config().appearance.frameInsets() : FrameInsets{};
      wlr_scene_node_set_position(&m_shadowContainer->node, x - inset.left, y - inset.top);
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
    if (config().appearance.useNineRect && bordersVisible()) {
      const auto insets = config().appearance.frameInsets();
      m_shadow.update(
          m_shadowContainer, contentWidth + insets.left + insets.right, contentHeight + insets.top + insets.bottom, 0, 0
      );
      if (!m_shadowPooled)
        wlr_scene_node_set_position(&m_shadowContainer->node, -insets.left, -insets.top);
    } else {
      if (!m_shadowPooled)
        wlr_scene_node_set_position(&m_shadowContainer->node, 0, 0);
      m_shadow.update(m_shadowContainer, contentWidth, contentHeight, borderInset, cornerRadius);
    }
  }

  ShadowSnapshot ViewDecoration::snapshotShadow(wlr_scene_tree* parent, wlr_scene_node* source, bool inPool) const {
    auto result = m_shadow.snapshot(parent, source, inPool);
    if (result.node != nullptr && config().appearance.useNineRect && bordersVisible()) {
      const auto insets = config().appearance.frameInsets();
      wlr_scene_node_set_position(
          &result.node->node, result.node->node.x - insets.left, result.node->node.y - insets.top
      );
      wlr_scene_shadow_set_animation_source(result.node, nullptr, config().colors.shadow.data());
    }
    return result;
  }

  void ViewDecoration::setShadowAnimationSource(wlr_scene_node* source) {
    m_shadow.setAnimationSource(config().appearance.useNineRect ? nullptr : source);
  }
  void ViewDecoration::hideShadow() { m_shadow.hide(); }

  void ViewDecoration::setAlpha(float decorationAlpha, float blurAlpha) {
    m_shadow.setAlpha(decorationAlpha);
    m_blur.setAlpha(blurAlpha);
  }

  void ViewDecoration::hideEffects() {
    hideBlur();
    m_shadow.hide();
  }

} // namespace umbriel
