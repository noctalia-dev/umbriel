#pragma once

#include <optional>
#include <pixman.h>

extern "C" {
#include <wlr/util/box.h>
}

struct fx_corner_radii;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  struct SurfaceBlurOptions {
    float ignoreAlpha = 0.0F;
    std::optional<bool> enabled;
    std::optional<bool> optimized;
  };

  // True when the surface's opaque region does not cover surfaceBox, given in surface-local coordinates.
  bool surfaceTransparent(wlr_surface* surface, const wlr_box& surfaceBox);

  // A client's ext_background_effect_v1 request. The region is committed state in surface-local coordinates;
  // `source` is the surface-local box the blur node shows. It is not always the committed geometry: a fullscreen
  // buffer is centred and a cropped tiled window is clipped 1:1, so the owner names the box that maps onto the node.
  struct ClientBlurRegion {
    const pixman_region32_t* region = nullptr;
    wlr_box source{};
  };

  // Maps `region`, in surface-local coordinates, into the blur node's space.
  // The node's transparency mask stretches the whole surface over the node box,
  // so the region follows the same ratio and origin shift: surfaceBox is the
  // surface-local content box (whose origin the scene helper places at the node's
  // origin) and nodeWidth/Height the blur node's size. Edges are clamped to the
  // node and rounded outward, so a fractional animation scale never clips a
  // blurred pixel and an oversized request cannot overflow.
  void mapBlurRegionToNode(
      const pixman_region32_t& region, const wlr_box& surfaceBox, int nodeWidth, int nodeHeight, pixman_region32_t* out
  );

  // Owns the desired-state logic for one SceneFX backdrop-blur node. The node itself is a child of the owner's scene
  // tree and is freed by scene-tree teardown, never by this class (no destructor).
  class SurfaceBlur {
  public:
    // Creates/updates/disables the node to match config and surface state. nodeBox: content box in `parent` coordinates
    // (position + size of the node). surfaceBox: content box in surface-local coordinates (opaque-region test).
    // clipBox: optional visible subset in `parent` coordinates; nullptr = full nodeBox. surfaceOpacity: opacity applied
    // to the surface buffers, used only to decide whether an opaque client reveals the blur. maskSource: optional scene
    // buffer to use instead of finding the surface's regular scene buffer. clientBlur: the region the client asked for
    // through ext_background_effect_v1; a null region means no request. A non-null region enables the blur on its own
    // and clips it; the node box stays the whole surface so the mask keeps its alignment.
    void update(
        wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
        fx_corner_radii corners, const wlr_box* clipBox = nullptr, const SurfaceBlurOptions& options = {},
        float surfaceOpacity = 1.0F, wlr_scene_buffer* maskSource = nullptr, const ClientBlurRegion& clientBlur = {}
    );
    // Opacity of the blur effect itself. Keep this independent from surface
    // opacity so compositor opacity matches equivalent client alpha.
    void setAlpha(float alpha);
    // Forces a live blur of everything below, overriding the optimized option, for a surface drawn over scene
    // content the cached background blur lacks.
    void setLive(bool live);
    // Disable the node (unmap path); update() re-enables.
    void hide();

  private:
    wlr_scene_blur* m_node = nullptr;
    bool m_masked = false;
    bool m_optimized = false;
    bool m_live = false;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
