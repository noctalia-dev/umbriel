#include "scene/surface_blur.h"

#include "config/config.h"
// clang-format off
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on
#include <pixman.h>

namespace umbriel {

  namespace {

    wlr_scene_buffer* findSurfaceBuffer(wlr_scene_node& root, wlr_surface* surface) {
      struct Lookup {
        wlr_surface* surface;
        wlr_scene_buffer* buffer = nullptr;
      } lookup{surface};

      wlr_scene_node_for_each_buffer(
          &root,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            auto* lookup = static_cast<Lookup*>(data);
            if (lookup->buffer != nullptr) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == lookup->surface) {
              lookup->buffer = buffer;
            }
          },
          &lookup
      );
      return lookup.buffer;
    }

    bool isTransparent(wlr_surface* surface, const wlr_box& surfaceBox) {
      const pixman_box32_t box = {
          surfaceBox.x, surfaceBox.y, surfaceBox.x + surfaceBox.width, surfaceBox.y + surfaceBox.height
      };
      return pixman_region32_contains_rectangle(&surface->opaque_region, &box) != PIXMAN_REGION_IN;
    }

  } // namespace

  void SurfaceBlur::update(
      wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox, int cornerRadius,
      const wlr_box* clipBox, const SurfaceBlurOptions& options, float surfaceOpacity, wlr_scene_buffer* maskSource
  ) {
    const Config::Appearance::Blur& cfg = config().appearance.blur;
    wlr_box drawBox = nodeBox;
    if (clipBox != nullptr) {
      if (!wlr_box_intersection(&drawBox, &nodeBox, clipBox) || drawBox.width <= 0 || drawBox.height <= 0) {
        if (m_node != nullptr) {
          wlr_scene_node_set_enabled(&m_node->node, false);
        }
        return;
      }
    }
    // A compositor opacity below 1 makes even a client-declared opaque surface
    // reveal the backdrop, so it also requires a blur node. Its opacity does
    // not attenuate the blur itself: client-provided alpha does not either.
    const bool want = cfg.enabled
        && options.enabled.value_or(false)
        && drawBox.width > 0
        && drawBox.height > 0
        && (surfaceOpacity < 1.0F || isTransparent(surface, surfaceBox));
    if (!want) {
      if (m_node != nullptr) {
        wlr_scene_node_set_enabled(&m_node->node, false);
      }
      return;
    }

    if (m_node != nullptr && m_masked != (options.ignoreAlpha > 0.0F)) {
      wlr_scene_node_destroy(&m_node->node);
      m_node = nullptr;
    }

    if (m_node == nullptr) {
      m_node = wlr_scene_blur_create(parent, drawBox.width, drawBox.height);
      if (m_node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_node->node);
      m_masked = options.ignoreAlpha > 0.0F;
    }

    wlr_scene_buffer* currentMask = wlr_scene_blur_get_transparency_mask_source(m_node);
    if (maskSource != nullptr) {
      if (currentMask != maskSource) {
        wlr_scene_blur_set_transparency_mask_source(m_node, maskSource);
      }
    } else if (currentMask == nullptr) {
      if (wlr_scene_buffer* mask = findSurfaceBuffer(parent->node, surface)) {
        wlr_scene_blur_set_transparency_mask_source(m_node, mask);
      }
    }

    if (m_node->ignore_alpha != options.ignoreAlpha) {
      wlr_scene_blur_set_ignore_alpha(m_node, options.ignoreAlpha);
    }

    wlr_scene_blur_set_should_only_blur_bottom_layer(m_node, options.optimized.value_or(cfg.optimized));

    wlr_scene_node_set_enabled(&m_node->node, true);
    wlr_scene_node_set_position(&m_node->node, drawBox.x, drawBox.y);
    if (m_node->width != drawBox.width || m_node->height != drawBox.height) {
      wlr_scene_blur_set_size(m_node, drawBox.width, drawBox.height);
    }
    if (m_node->corners.top_left != cornerRadius) {
      wlr_scene_blur_set_corner_radius(m_node, cornerRadius);
    }
    if (m_node->alpha != m_alpha) {
      wlr_scene_blur_set_alpha(m_node, m_alpha);
    }
  }

  void SurfaceBlur::setAlpha(float alpha) {
    m_alpha = alpha;
    if (m_node != nullptr && m_node->alpha != alpha) {
      wlr_scene_blur_set_alpha(m_node, alpha);
    }
  }

  void SurfaceBlur::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
  }

} // namespace umbriel
