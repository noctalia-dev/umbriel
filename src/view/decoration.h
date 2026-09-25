#pragma once

#include "scene/border_rect.h"
#include "scene/surface_blur.h"
#include "scene/surface_shadow.h"

#include <array>
#include <utility>
#include <vector>

struct wlr_scene_border;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  struct ResolvedWindowRule;

  // Everything drawn around a view's surface: the inner border ring, the outer ring, the blur sampled behind the
  // surface, and the drop shadow. The shadow container is a child of the view's frame, below its content, so it follows
  // the frame's parent, stacking order, position, and visibility. The one exception is a tile: its container is lent to
  // the workspace's tile shadow layer, so tiles never shadow each other. This class holds no reference back to its
  // View. Everything that varies per view (content size, corner radius, fade alpha, focus) arrives as an argument,
  // because those are questions only the View can answer (a fullscreen window keeps its border tree but draws square,
  // and a size animation presents a size the committed geometry has not caught up with yet). Appearance settings are
  // read from the config directly, as the other scene classes do.
  class ViewDecoration {
  public:
    // The single node resolves inner and outer colors from one shared curve.
    void ensureBorders(wlr_scene_tree* parent);
    // True while the ring exists and is showing. Fullscreen disables the tree
    // rather than destroying it, so existence alone does not answer this.
    [[nodiscard]] bool bordersVisible() const;
    [[nodiscard]] wlr_scene_tree* borderTree() const { return m_borderTree; }
    void setBordersEnabled(bool enabled);
    void updateBorderGeometry(int contentWidth, int contentHeight);
    // `alpha` premultiplies the border color so a fading view's ring fades with it.
    void setBorderColor(bool focused, bool scratchpad, float alpha);
    void setBorderRawColor(const std::array<float, 4>& baseColor, float alpha);
    // True when the drawn ring no longer matches the given content size, i.e. a
    // client commit changed geometry behind the layout's back.
    [[nodiscard]] bool borderGeometryStale(int contentWidth, int contentHeight) const;
    // Copy the border into a close-animation snapshot tree. `innerColor` is the straight colour the ring currently
    // shows and `opacity` the effective opacity it is drawn at.
    void snapshotBorders(
        wlr_scene_tree* snapshot, const std::array<float, 4>& innerColor, float opacity,
        std::vector<BorderSnapshot>& out
    ) const;

    // Blur
    [[nodiscard]] SurfaceBlurOptions blurOptions() const { return m_blurOptions; }
    [[nodiscard]] SurfaceBlurOptions popupBlurOptions() const { return m_popupBlurOptions; }
    void applyRule(const ResolvedWindowRule& rule);
    void updateBlur(
        wlr_scene_tree* tree, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& geometry, int radius,
        const wlr_box* clip, float surfaceOpacity, float blurAlpha
    );
    void hideBlur();

    // Shadow
    void createShadow(wlr_scene_tree* frame);
    // Lend the container to `pool` at the frame's position and visibility, or return it below the frame's content when
    // `pool` is null.
    void poolShadow(wlr_scene_tree* frame, wlr_scene_tree* pool, int x, int y, bool enabled);
    [[nodiscard]] bool shadowPooled() const { return m_shadowPooled; }
    // Only a pooled container needs these; under the frame it inherits both.
    void setShadowPosition(int x, int y);
    void setShadowEnabled(bool enabled);
    void updateShadow(int contentWidth, int contentHeight, int borderInset, int cornerRadius);
    void hideShadow();
    void setShadowAnimationSource(wlr_scene_node* source) { m_shadow.setAnimationSource(source); }
    // `inPool` places the copy under every window of `parent`, otherwise directly below `source`.
    [[nodiscard]] ShadowSnapshot snapshotShadow(wlr_scene_tree* parent, wlr_scene_node* source, bool inPool) const {
      return m_shadow.snapshot(parent, source, inPool);
    }
    [[nodiscard]] const wlr_scene_shadow* shadowNode() const { return m_shadow.node(); }

    // Shadows follow the full view opacity. Blur follows only transition
    // opacity, otherwise a window rule attenuates the backdrop twice.
    void setAlpha(float decorationAlpha, float blurAlpha);
    // Both effects off, without touching the border ring (unmap path).
    void hideEffects();

  private:
    wlr_scene_tree* m_borderTree = nullptr;
    wlr_scene_border* m_border = nullptr;
    SurfaceBlur m_blur;
    SurfaceBlurOptions m_blurOptions;
    SurfaceBlurOptions m_popupBlurOptions;
    SurfaceShadow m_shadow;
    wlr_scene_tree* m_shadowContainer = nullptr;
    bool m_shadowPooled = false;
  };

} // namespace umbriel
