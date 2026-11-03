#include "view/presentation.h"

#include "config/config.h"
#include "view/presented_crop.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  // Presented size
  void ViewPresentation::setSize(int width, int height) {
    m_width = width;
    m_height = height;
  }

  void ViewPresentation::track(int width, int height) {
    if (animating() || width <= 0 || height <= 0) {
      return;
    }
    m_width = width;
    m_height = height;
  }

  // Size animation
  bool ViewPresentation::targeting(int width, int height) const {
    return animating() && width == static_cast<int>(m_animW.target()) && height == static_cast<int>(m_animH.target());
  }

  void ViewPresentation::animateTo(int width, int height, int durationMs, const AnimationCurve& curve) {
    // A tween still presenting this size keeps its speed through the retarget; anything else starts from the size
    // shown.
    const auto restart = [](AnimatedValue& anim, int shown) {
      if (!anim.animating() || std::lround(anim.current()) != shown) {
        anim.snap(shown);
      }
    };
    restart(m_animW, m_width);
    m_animW.retarget(width, durationMs, curve);
    restart(m_animH, m_height);
    m_animH.retarget(height, durationMs, curve);
  }

  void ViewPresentation::setRegion(int width, int height) {
    m_animW.snap(width);
    m_animH.snap(height);
  }

  bool ViewPresentation::tick(double nowMsec) {
    const bool movedW = m_animW.tick(nowMsec);
    const bool movedH = m_animH.tick(nowMsec);
    // A carried-speed spring settles on its own solver; stop it once no pixel can change.
    static_cast<void>(m_animW.finishSpringTail(1.0));
    static_cast<void>(m_animH.finishSpringTail(1.0));
    if (!movedW && !movedH) {
      return false;
    }
    m_width = static_cast<int>(std::lround(m_animW.current()));
    m_height = static_cast<int>(std::lround(m_animH.current()));
    return true;
  }

  // Fullscreen
  void ViewPresentation::createBackdrop(wlr_scene_tree* parent) {
    m_backdrop = wlr_scene_rect_create(parent, 0, 0, config().colors.backdrop.data());
    wlr_scene_rect_set_corner_radius(m_backdrop, 0);
    wlr_scene_node_lower_to_bottom(&m_backdrop->node);
    wlr_scene_node_set_enabled(&m_backdrop->node, false);
  }

  void ViewPresentation::setBackdropEnabled(bool enabled) {
    if (m_backdrop != nullptr) {
      wlr_scene_node_set_enabled(&m_backdrop->node, enabled);
    }
  }

  void ViewPresentation::setFullscreenOpaque(bool opaque) {
    m_fullscreenOpaque = opaque;
    setBackdropEnabled(m_fullscreenSized && m_fullscreenOpaque);
  }

  void ViewPresentation::setBackdropBox(int x, int y, int width, int height) {
    if (m_backdrop != nullptr) {
      wlr_scene_node_set_position(&m_backdrop->node, x, y);
      wlr_scene_rect_set_size(m_backdrop, width, height);
    }
  }

  void ViewPresentation::reloadBackdropColor() {
    if (m_backdrop != nullptr) {
      wlr_scene_rect_set_color(m_backdrop, config().colors.backdrop.data());
    }
  }

  void ViewPresentation::updateFullscreen(
      bool fullscreen, int tileWidth, int tileHeight, wlr_scene_node* surfaceNode, const wlr_box& geometry
  ) {
    const bool validSize = tileWidth > 0 && tileHeight > 0;
    m_fullscreenSized = fullscreen && validSize;
    setBackdropEnabled(m_fullscreenSized && m_fullscreenOpaque);
    if (m_fullscreenSized) {
      setBackdropBox(0, 0, tileWidth, tileHeight);
      m_offsetX = fullscreenCenterOffset(tileWidth, geometry.width);
      m_offsetY = fullscreenCenterOffset(tileHeight, geometry.height);
      if (surfaceNode != nullptr) {
        // The wlroots xdg scene helper resets this to (-geo.x, -geo.y) on every
        // commit; our commit handler re-applies the centering offset afterwards.
        wlr_scene_node_set_position(surfaceNode, m_offsetX - geometry.x, m_offsetY - geometry.y);
      }
      m_contentCentered = m_offsetX != 0 || m_offsetY != 0;
      return;
    }
    m_offsetX = 0;
    m_offsetY = 0;
    if (!fullscreen && m_contentCentered) {
      if (surfaceNode != nullptr) {
        wlr_scene_node_set_position(surfaceNode, -geometry.x, -geometry.y);
      }
      m_contentCentered = false;
    }
  }

  // Buffer crop
  void ViewPresentation::applyCrop(
      wlr_scene_tree* tree, wlr_surface* surface, const wlr_box& geometry, const wlr_box& content,
      const wlr_box& surfaceClip
  ) const {
    struct Ctx {
      wlr_surface* surface;
      const wlr_box* geometry;
      const wlr_box* content;
      const wlr_box* clip;
    } ctx{surface, &geometry, &content, &surfaceClip};

    wlr_scene_node_for_each_buffer(
        &tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto& ctx = *static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr || sceneSurface->surface != ctx.surface) {
            return;
          }
          wlr_surface* surface = sceneSurface->surface;
          wlr_fbox base{};
          wlr_surface_get_buffer_source_box(surface, &base);
          // Geometry, content, and clip are in scene units, which differ from surface units when it is drawn scaled.
          int width = 0;
          int height = 0;
          wlr_scene_surface_get_size(sceneSurface, &width, &height);
          const wlr_fbox src = croppedSourceBox(base, *ctx.geometry, *ctx.content, *ctx.clip, width, height);
          if (src.width <= 0 || src.height <= 0) {
            return;
          }
          wlr_scene_buffer_set_source_box(buffer, &src);
          wlr_scene_buffer_set_dest_size(buffer, ctx.clip->width, ctx.clip->height);
        },
        &ctx
    );
  }

  void ViewPresentation::resetCrop(wlr_scene_tree* tree, wlr_surface* surface) const {
    wlr_scene_node_for_each_buffer(
        &tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto* target = static_cast<wlr_surface*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr || sceneSurface->surface != target) {
            return;
          }
          wlr_scene_buffer_set_source_box(buffer, nullptr);
          int width = 0;
          int height = 0;
          wlr_scene_surface_get_size(sceneSurface, &width, &height);
          wlr_scene_buffer_set_dest_size(buffer, width, height);
        },
        surface
    );
  }

} // namespace umbriel
