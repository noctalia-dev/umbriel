#pragma once

#include <array>

struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_scene_node;

namespace umbriel {

  struct ShadowSnapshot {
    wlr_scene_tree* tree = nullptr;
    wlr_scene_shadow* node = nullptr;
    std::array<float, 4> color{};
  };

  // Owns the desired-state logic for one SceneFX drop-shadow node. The node is a
  // child of the owner's scene tree and freed by scene-tree teardown (no destructor).
  class SurfaceShadow {
  public:
    // contentWidth/Height: toplevel geometry size. borderTotal: decoration ring width drawn outside the content (0 when
    // borders are disabled/hidden). cornerRadius: radius of the decoration's outer edge (0 = square).
    void update(wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius);
    // Disable the node (unmap/fullscreen/off-output path); update() re-enables.
    void hide();
    // Forget the node pointer (caller is destroying the parent tree externally).
    void reset();
    // Set an opacity multiplier applied to the shadow color (for fade animations).
    void setAlpha(float alpha);
    void setAnimationSource(wlr_scene_node* source);
    // `inPool` puts the copy under every sibling in `parent`; otherwise it sits directly below `source`, a sibling.
    [[nodiscard]] ShadowSnapshot snapshot(wlr_scene_tree* parent, wlr_scene_node* source, bool inPool) const;
    // Null until the first update() creates it.
    [[nodiscard]] const wlr_scene_shadow* node() const { return m_node; }

  private:
    wlr_scene_shadow* m_node = nullptr;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
