#pragma once

#include <array>

struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_scene_node;
struct wlr_box;

namespace umbriel {

  struct ShadowSnapshot {
    wlr_scene_tree* tree = nullptr;
    wlr_scene_shadow* node = nullptr;
    std::array<float, 4> color{};
  };

  // Effective shadow parameters
  struct SurfaceShadowOptions {
    bool enabled = true;
    int softness = 10;
    int offsetX = 2;
    int offsetY = 2;
    bool operator==(const SurfaceShadowOptions&) const = default;
  };

  // Global [appearance.shadow] for compositor panels with no window rule.
  [[nodiscard]] SurfaceShadowOptions globalShadowOptions();

  // Owns the desired-state logic for one SceneFX drop-shadow node. The node is a
  // child of the owner's scene tree and freed by scene-tree teardown (no destructor).
  class SurfaceShadow {
  public:
    // contentWidth/Height: toplevel geometry size. borderTotal: decoration ring width drawn outside the content (0 when
    // borders are disabled/hidden). cornerRadius: radius of the decoration's outer edge (0 = square). options: the
    // effective shadow parameters for this window (rule overrides).
    void update(
        wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius,
        const SurfaceShadowOptions& options = {}
    );
    // Disable the node (unmap/fullscreen/off-output path); update() re-enables.
    void hide();
    // Forget the node pointer (caller is destroying the parent tree externally).
    void reset();
    // Set an opacity multiplier applied to the shadow color (for fade animations).
    void setAlpha(float alpha);
    void setAnimationSource(wlr_scene_node* source);
    [[nodiscard]] ShadowSnapshot snapshot(wlr_scene_tree* parent, wlr_scene_node* source) const;

  private:
    wlr_scene_shadow* m_node = nullptr;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
