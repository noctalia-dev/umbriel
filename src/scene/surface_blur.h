#pragma once

#include <optional>

struct wlr_box;
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

  // Owns the desired-state logic for one SceneFX backdrop-blur node. The node itself is a child of the owner's scene
  // tree and is freed by scene-tree teardown, never by this class (no destructor).
  class SurfaceBlur {
  public:
    // Creates/updates/disables the node to match config and surface state. nodeBox: content box in `parent` coordinates
    // (position + size of the node). surfaceBox: content box in surface-local coordinates (opaque-region test).
    // clipBox: optional visible subset in `parent` coordinates; nullptr = full nodeBox. surfaceOpacity: opacity applied
    // to the surface buffers, used only to decide whether an opaque client reveals the blur. maskSource: optional scene
    // buffer to use instead of finding the surface's regular scene buffer.
    void update(
        wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
        int cornerRadius, const wlr_box* clipBox = nullptr, const SurfaceBlurOptions& options = {},
        float surfaceOpacity = 1.0F, wlr_scene_buffer* maskSource = nullptr
    );
    // Opacity of the blur effect itself. Keep this independent from surface
    // opacity so compositor opacity matches equivalent client alpha.
    void setAlpha(float alpha);
    // Disable the node (unmap path); update() re-enables.
    void hide();

  private:
    wlr_scene_blur* m_node = nullptr;
    bool m_masked = false;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
