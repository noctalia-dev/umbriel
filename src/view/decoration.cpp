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
    }
  }

  int ViewDecoration::borderWidth() const { return m_ruleBorderWidth.value_or(config().appearance.borderWidth); }

  int ViewDecoration::totalBorderWidth() const { return borderWidth() + config().appearance.outerBorderWidth; }

  int ViewDecoration::cornerRadius() const { return m_ruleCornerRadius.value_or(config().appearance.cornerRadius); }

  bool ViewDecoration::shadowEnabled() const { return m_ruleShadow.value_or(config().appearance.shadow.enabled); }

  void ViewDecoration::updateBorderGeometry(int contentWidth, int contentHeight) {
    if (m_border == nullptr) {
      return;
    }

    const int outerWidth = config().appearance.outerBorderWidth;
    const int width = borderWidth();
    applyBorderGeometry(
        m_border, makeBorderRing(contentWidth, contentHeight, cornerRadius(), width, outerWidth), width, outerWidth
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
    const BorderRing ring = makeBorderRing(
        contentWidth, contentHeight, cornerRadius(), borderWidth(), config().appearance.outerBorderWidth
    );
    return m_border->width != ring.box.width || m_border->height != ring.box.height;
  }

  void ViewDecoration::snapshotBorders(wlr_scene_tree* snapshot, bool focused, std::vector<BorderSnapshot>& out) const {
    if (!bordersVisible() || m_border == nullptr) {
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
    out.push_back(
        BorderSnapshot{
            .node = copy,
            .innerColor = focused ? config().colors.border.focused : config().colors.border.unfocused,
            .outerColor = config().colors.border.outer,
        }
    );
  }

  // Blur
  bool ViewDecoration::applyRule(const ResolvedWindowRule& rule) {
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
    const bool chromeChanged =
        m_ruleBorderWidth != rule.borderWidth || m_ruleCornerRadius != rule.cornerRadius || m_ruleShadow != rule.shadow;
    m_ruleBorderWidth = rule.borderWidth;
    m_ruleCornerRadius = rule.cornerRadius;
    m_ruleShadow = rule.shadow;
    return chromeChanged;
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
  void ViewDecoration::reparentShadow(wlr_scene_tree* layer, int x, int y, bool enabled) {
    if (layer == nullptr) {
      m_shadow.reset();
      if (m_shadowContainer != nullptr) {
        wlr_scene_node_destroy(&m_shadowContainer->node);
        m_shadowContainer = nullptr;
      }
      return;
    }
    if (m_shadowContainer == nullptr) {
      m_shadowContainer = wlr_scene_tree_create(layer);
    } else {
      wlr_scene_node_reparent(&m_shadowContainer->node, layer);
    }
    wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
  }

  void ViewDecoration::setShadowPosition(int x, int y) {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    }
  }

  void ViewDecoration::setShadowEnabled(bool enabled) {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
    }
  }

  void ViewDecoration::raiseShadowToTop() {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_raise_to_top(&m_shadowContainer->node);
    }
  }

  void ViewDecoration::updateShadow(int contentWidth, int contentHeight, int borderInset, int outerRadius) {
    if (m_shadowContainer == nullptr) {
      return;
    }
    // std::nullopt leaves the global switch in charge; an explicit value overrides
    // it either way, so a rule can drop a shadow or draw one the global config does not.
    m_shadow.setEnabled(m_ruleShadow);
    m_shadow.update(m_shadowContainer, contentWidth, contentHeight, borderInset, outerRadius);
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
