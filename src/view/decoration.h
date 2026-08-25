#pragma once

#include "scene/surface_blur.h"
#include "scene/surface_shadow.h"
#include "view/border_ring.h"

#include <array>
#include <utility>
#include <vector>

struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  struct ResolvedWindowRule;

  // Everything drawn around a view's surface: the inner border ring, the outer ring, the blur sampled behind the
  // surface, and the drop shadow. The shadow is deliberately not a child of the view's tree. It lives in the
  // workspace's shadow layer so it renders under every window rather than only under its own, which is why it needs its
  // own container node and its own position updates whenever the view moves. This class holds no reference back to its
  // View. Everything that varies per view (content size, corner radius, fade alpha, focus) arrives as an argument,
  // because those are questions only the View can answer (a fullscreen window keeps its border tree but draws square,
  // and a size animation presents a size the committed geometry has not caught up with yet). Appearance settings are
  // read from the config directly, as the other scene classes do.
  class ViewDecoration {
  public:
    // The ring is reused so fullscreen toggles preserve scene-node ordering that
    // keeps the inner ring above the outer one.
    void ensureBorders(wlr_scene_tree* parent);
    // True while the ring exists and is showing. Fullscreen disables the tree
    // rather than destroying it, so existence alone does not answer this.
    [[nodiscard]] bool bordersVisible() const;
    void setBordersEnabled(bool enabled);
    void updateBorderGeometry(int contentWidth, int contentHeight);
    // `alpha` premultiplies the border color so a fading view's ring fades with it.
    void setBorderColor(bool focused, bool scratchpad, float alpha);
    // True when the drawn ring no longer matches the given content size, i.e. a
    // client commit changed geometry behind the layout's back.
    [[nodiscard]] bool borderGeometryStale(int contentWidth, int contentHeight) const;
    // Copy the ring into a close-animation snapshot tree. Each entry pairs the
    // copied rect with the color it should animate toward.
    void snapshotBorders(
        wlr_scene_tree* snapshot, bool focused, std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>>& out
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
    // Pass a null layer to tear the shadow down (the view left every workspace).
    void reparentShadow(wlr_scene_tree* layer, int x, int y, bool enabled);
    void setShadowPosition(int x, int y);
    void setShadowEnabled(bool enabled);
    void raiseShadowToTop();
    void updateShadow(int contentWidth, int contentHeight, int borderInset, int cornerRadius);
    void hideShadow();

    // Shadows follow the full view opacity. Blur follows only transition
    // opacity, otherwise a window rule attenuates the backdrop twice.
    void setAlpha(float decorationAlpha, float blurAlpha);
    // Both effects off, without touching the border ring (unmap path).
    void hideEffects();

  private:
    wlr_scene_tree* m_borderTree = nullptr;
    wlr_scene_rect* m_borderRect = nullptr;
    wlr_scene_rect* m_outerBorderRect = nullptr;
    SurfaceBlur m_blur;
    SurfaceBlurOptions m_blurOptions;
    SurfaceBlurOptions m_popupBlurOptions;
    SurfaceShadow m_shadow;
    wlr_scene_tree* m_shadowContainer = nullptr; // child of workspace shadow layer
  };

} // namespace umbriel
