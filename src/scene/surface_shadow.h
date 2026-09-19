#pragma once

#include <array>
#include <optional>

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

  // Owns the desired-state logic for one SceneFX drop-shadow node. The node is a
  // child of the owner's scene tree and freed by scene-tree teardown (no destructor).
  class SurfaceShadow {
  public:
    // contentWidth/Height: toplevel geometry size. borderTotal: decoration ring width drawn outside the content (0 when
    // borders are disabled/hidden). cornerRadius: radius of the decoration's outer edge (0 = square).
    void update(wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius);
    // Owner override of appearance.shadow.enabled. std::nullopt follows the global
    // switch, so a window rule can both drop a shadow the global config draws and
    // draw one it does not. Owners that never call this follow the global switch.
    void setEnabled(std::optional<bool> enabled);
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
    // Follows appearance.shadow.enabled until an owner overrides it either way.
    std::optional<bool> m_enabled;
  };

} // namespace umbriel
