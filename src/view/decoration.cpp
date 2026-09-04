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

    const auto& appearance = config().appearance;
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
    const BorderRing ring = makeBorderRing(
        contentWidth, contentHeight, appearance.cornerRadius, appearance.borderWidth, appearance.outerBorderWidth
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
    out.push_back(
        BorderSnapshot{
            .node = copy,
            .innerColor = focused ? config().colors.border.focused : config().colors.border.unfocused,
            .outerColor = config().colors.border.outer,
        }
    );
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
