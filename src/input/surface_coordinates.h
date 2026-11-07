#pragma once

#include "wlr_color.h"

#include <cmath>
#include <optional>
extern "C" {
#include <umbrielfx/types/wlr_scene.h>
}

namespace umbriel {
  struct SurfaceDeltaTransform {
    double scaleX = 1;
    double scaleY = 1;
    void map(double* dx, double* dy) const {
      *dx *= scaleX;
      *dy *= scaleY;
    }
    void unmap(double* dx, double* dy) const {
      *dx /= scaleX;
      *dy /= scaleY;
    }
  };

  struct SurfaceCoordinates {
    wlr_scene_surface* surface;
    int x, y;
  };

  inline std::optional<SurfaceCoordinates> surfaceCoordinates(wlr_scene* scene, wlr_surface* target) {
    if (target == nullptr) {
      return std::nullopt;
    }
    struct Search {
      wlr_surface* target;
      std::optional<SurfaceCoordinates> result;
    } search{target, std::nullopt};
    wlr_scene_node_for_each_buffer(
        &scene->tree.node,
        [](wlr_scene_buffer* buffer, int x, int y, void* data) {
          auto& search = *static_cast<Search*>(data);
          wlr_scene_surface* surface = wlr_scene_surface_try_from_buffer(buffer);
          if (!search.result && surface != nullptr && surface->surface == search.target) {
            search.result = SurfaceCoordinates{surface, x, y};
          }
        },
        &search
    );
    return search.result;
  }

  inline std::optional<SurfaceDeltaTransform>
  surfaceDeltaTransform(wlr_scene* scene, wlr_surface* target, double fallbackScale) {
    if (const auto coordinates = surfaceCoordinates(scene, target)) {
      wlr_scene_surface_mapping mapping;
      if (!wlr_scene_surface_get_mapping(coordinates->surface, &mapping)) {
        return std::nullopt;
      }
      return SurfaceDeltaTransform{mapping.scale_x, mapping.scale_y};
    }
    // Hidden surfaces can still own a grab or a lock being released.
    if (!std::isfinite(fallbackScale) || fallbackScale <= 0) {
      return std::nullopt;
    }
    return SurfaceDeltaTransform{fallbackScale, fallbackScale};
  }

  inline bool
  surfaceLocalCoordinates(wlr_scene* scene, wlr_surface* target, double lx, double ly, double* sx, double* sy) {
    const auto coordinates = surfaceCoordinates(scene, target);
    if (!coordinates) {
      return false;
    }
    *sx = lx - coordinates->x;
    *sy = ly - coordinates->y;
    return wlr_scene_surface_coords_to_surface(coordinates->surface, sx, sy);
  }
} // namespace umbriel
