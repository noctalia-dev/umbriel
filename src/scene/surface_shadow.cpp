#include "scene/surface_shadow.h"

#include "config/config.h"
// clang-format off
#include <cstring> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace umbriel {

  void SurfaceShadow::update(
      wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius
  ) {
    const auto& cfg = config().appearance.shadow;
    const auto& shadow = config().colors.shadow;
    const int sigma = cfg.softness;

    // Decorated box origin (in parent coords): (-borderTotal, -borderTotal)
    // Decorated box size: (contentWidth + 2*borderTotal, contentHeight + 2*borderTotal)
    const int decWidth = contentWidth + 2 * borderTotal;
    const int decHeight = contentHeight + 2 * borderTotal;

    const bool want = cfg.enabled && shadow[3] > 0.0F && contentWidth > 0 && contentHeight > 0;
    if (!want) {
      if (m_node != nullptr) {
        wlr_scene_node_set_enabled(&m_node->node, false);
      }
      return;
    }

    // Lazy create: first time only.
    if (m_node == nullptr) {
      const float initColor[4] = {shadow[0], shadow[1], shadow[2], shadow[3] * m_alpha};
      m_node = wlr_scene_shadow_create(parent, 0, 0, 0, static_cast<float>(sigma), initColor);
      if (m_node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_node->node);
    }

    // Node geometry: shadow extends sigma px beyond the decorated box on each side.
    int nodeWidth = decWidth + 2 * sigma;
    int nodeHeight = decHeight + 2 * sigma;
    int nodeX = -borderTotal - sigma + cfg.offsetX;
    int nodeY = -borderTotal - sigma + cfg.offsetY;

    // Hole punched over the window+borders so the shadow never shows through
    // transparent surfaces. Node-local coordinates.
    wlr_box hole{
        .x = sigma - cfg.offsetX,
        .y = sigma - cfg.offsetY,
        .width = decWidth,
        .height = decHeight,
    };

    wlr_scene_node_set_position(&m_node->node, nodeX, nodeY);
    if (m_node->width != nodeWidth || m_node->height != nodeHeight) {
      wlr_scene_shadow_set_size(m_node, nodeWidth, nodeHeight);
    }

    // Sync parameters when changed.
    if (m_node->blur_sigma != static_cast<float>(sigma)) {
      wlr_scene_shadow_set_blur_sigma(m_node, static_cast<float>(sigma));
    }
    const float color[4] = {shadow[0], shadow[1], shadow[2], shadow[3] * m_alpha};
    if (std::memcmp(m_node->color, color, sizeof(m_node->color)) != 0) {
      wlr_scene_shadow_set_color(m_node, color);
    }
    if (m_node->corner_radius != cornerRadius) {
      wlr_scene_shadow_set_corner_radius(m_node, cornerRadius);
    }

    const fx_corner_radii corners = corner_radii_new(cornerRadius, cornerRadius, cornerRadius, cornerRadius);
    wlr_scene_shadow_set_clipped_region(m_node, clipped_region{.area = hole, .corners = corners});

    wlr_scene_node_set_enabled(&m_node->node, true);
  }

  void SurfaceShadow::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
  }

  void SurfaceShadow::setAlpha(float alpha) {
    m_alpha = alpha;
    if (m_node != nullptr) {
      const auto& shadow = config().colors.shadow;
      const float color[4] = {shadow[0], shadow[1], shadow[2], shadow[3] * m_alpha};
      wlr_scene_shadow_set_color(m_node, color);
    }
  }

  void SurfaceShadow::reset() { m_node = nullptr; }
} // namespace umbriel
