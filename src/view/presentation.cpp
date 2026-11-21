#include "view/presentation.h"

#include "config/config.h"
#include "view/presented_crop.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include <utility>
#include "wlr.h"
// clang-format on

namespace umbriel {

  void PositionPixelAlignment::update(
      const AnimatedValue& position, double extent, double scale, double origin, bool resizing
  ) {
    if (position.animating() && m_transitionId == position.transitionId() && m_scale == scale && m_origin == origin) {
      return;
    }

    from.reset();
    target.reset();
    roundingMargin = 0.25;
    m_transitionId = position.animating() ? position.transitionId() : 0;
    m_scale = scale;
    m_origin = origin;
    if (!position.animating() || resizing || extent <= 0.0 || !std::isfinite(scale) || scale <= 0.0) {
      return;
    }

    // Center the origin in the near and far edges' resting rounding cells.
    const double pixels = extent * scale;
    const auto cell = [extent, pixels, scale, origin](double value) {
      const double near = std::round((value - origin) * scale);
      const double far = std::round((value + extent - origin) * scale);
      const double lower = std::max(near - 0.5, far - 0.5 - pixels);
      const double upper = std::min(near + 0.5, far + 0.5 - pixels);
      return std::pair{(lower + upper) / 2.0, (upper - lower) / 2.0};
    };

    const auto [targetPixels, margin] = cell(position.target());
    from = cell(position.from()).first;
    target = targetPixels;
    roundingMargin = std::min(0.25, margin);
  }

  // Presented size
  void ViewPresentation::setSize(double width, double height) {
    m_width = width;
    m_height = height;
  }

  void ViewPresentation::track(int width, int height) {
    if (animating() || width <= 0 || height <= 0) {
      return;
    }
    // Reapplying derived integer geometry must not discard an animated fraction.
    if (width != this->width() || height != this->height()) {
      setSize(width, height);
    }
  }

  // Size animation
  bool ViewPresentation::targeting(int width, int height) const {
    return animating() && width == static_cast<int>(m_animW.target()) && height == static_cast<int>(m_animH.target());
  }

  void ViewPresentation::animateTo(int width, int height, int durationMs, const AnimationCurve& curve) {
    // A tween still presenting this size keeps its speed through the retarget; anything else starts from the size
    // shown.
    const auto restart = [](AnimatedValue& anim, double shown) {
      if (!anim.animating() || std::lround(anim.presentedCurrent()) != std::lround(shown)) {
        anim.snap(shown);
      }
    };
    restart(m_animW, m_width);
    m_animW.retarget(width, durationMs, curve);
    restart(m_animH, m_height);
    m_animH.retarget(height, durationMs, curve);
    m_alignmentW.reset();
    m_alignmentH.reset();
    m_alignmentHeld = false;
  }

  void ViewPresentation::setRegion(int width, int height) {
    m_animW.snap(width);
    m_animH.snap(height);
    m_alignmentW.reset();
    m_alignmentH.reset();
    m_alignmentHeld = false;
  }

  bool ViewPresentation::tick(
      double nowMsec, double pixelsPerUnit, double originX, double originY, std::optional<double> targetWidthPixels,
      std::optional<double> targetHeightPixels, std::optional<PositionOrigins> positionX,
      std::optional<PositionOrigins> positionY
  ) {
    const bool wasAlignmentHeld = m_alignmentHeld;
    const PositionOrigins originsX = positionX.value_or(PositionOrigins{originX, originX, originX, originX});
    const PositionOrigins originsY = positionY.value_or(PositionOrigins{originY, originY, originY, originY});
    const auto align = [pixelsPerUnit](
                           const AnimatedValue& animation, std::optional<PixelAlignment>& alignment,
                           const PositionOrigins& position, std::optional<double> targetPixels
                       ) {
      if ((!animation.animating() && (!alignment || !position.animating))
          || !std::isfinite(pixelsPerUnit)
          || pixelsPerUnit <= 0.0) {
        alignment.reset();
        return;
      }

      if (!alignment || alignment->scale != pixelsPerUnit) {
        // Retain the starting grid until retargeting. Following this frame's position
        // would move the far edge backward when the near edge crosses a rounding boundary.
        alignment = PixelAlignment{
            pixelsPerUnit,
            position.presentedFrom,
            std::round((animation.from() - position.from) * pixelsPerUnit),
            0.0,
        };
      }

      // Follow the position endpoint, not its per-frame origin. When a move
      // settles before the resize, its logical origin replaces the aligned one.
      alignment->targetPixels = (position.presentedTo - alignment->origin) * pixelsPerUnit
          + targetPixels.value_or(std::round((animation.target() - position.to) * pixelsPerUnit));
    };

    align(m_animW, m_alignmentW, originsX, targetWidthPixels);
    align(m_animH, m_alignmentH, originsY, targetHeightPixels);
    std::optional<double> fromWidthPixels;
    std::optional<double> fromHeightPixels;
    if (m_alignmentW) {
      originX = m_alignmentW->origin;
      fromWidthPixels = m_alignmentW->fromPixels;
      targetWidthPixels = m_alignmentW->targetPixels;
    }
    if (m_alignmentH) {
      originY = m_alignmentH->origin;
      fromHeightPixels = m_alignmentH->fromPixels;
      targetHeightPixels = m_alignmentH->targetPixels;
    }

    const bool movedW = m_animW.tick(nowMsec);
    const bool movedH = m_animH.tick(nowMsec);
    // Far edges combine position and size motion, so reserve half of their rounding margin for each timeline.
    static_cast<void>(
        m_animW.finishPixelAlignedSpringTail(pixelsPerUnit, originX, targetWidthPixels, fromWidthPixels, 0.25)
    );
    static_cast<void>(
        m_animH.finishPixelAlignedSpringTail(pixelsPerUnit, originY, targetHeightPixels, fromHeightPixels, 0.25)
    );
    m_alignmentHeld = (!m_animW.animating() && m_alignmentW && originsX.animating)
        || (!m_animH.animating() && m_alignmentH && originsY.animating);
    if (!movedW && !movedH && !wasAlignmentHeld && !m_alignmentHeld) {
      return false;
    }
    const auto presented = [pixelsPerUnit](
                               const AnimatedValue& animation, std::optional<PixelAlignment>& alignment,
                               const PositionOrigins& position, double origin, std::optional<double> targetPixels,
                               std::optional<double> fromPixels
                           ) {
      if (animation.animating()) {
        return animation.pixelAlignedCurrent(pixelsPerUnit, origin, targetPixels, fromPixels);
      }
      if (alignment && position.animating) {
        // Releasing the aligned size while the near edge still moves can cross
        // the far edge's rounding boundary on the resize's completion frame.
        return alignment->origin + alignment->targetPixels / alignment->scale;
      }
      alignment.reset();
      return animation.current();
    };
    setSize(
        presented(m_animW, m_alignmentW, originsX, originX, targetWidthPixels, fromWidthPixels),
        presented(m_animH, m_alignmentH, originsY, originY, targetHeightPixels, fromHeightPixels)
    );
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
