#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
}

#include <cstdint>

struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  // The size a view is currently *drawn* at, which is not the size its client committed. Three things pull them apart:
  // - a layout configure the client has not acked yet (Electron in particular stays at its old width for several
  // frames), - a resize animation interpolating between two layout sizes, - fullscreen, where an oversized or
  // undersized client buffer is centered in the output rather than scaled (scaling would distort the aspect).
  // Everything that depends on how large the view looks right now (borders, shadow, blur, output clipping) must ask
  // this rather than reading the committed geometry, or it will lag a frame behind or fight the animation. Holds no
  // reference back to its View: the scene tree and surface it operates on arrive as arguments.
  class ViewPresentation {
  public:
    // Presented size
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }
    // Adopt a size outright: map, animation end, or an interactive resize that
    // owns the size itself.
    void setSize(int width, int height);
    // Adopt a size only if it is real and no animation currently owns it.
    void track(int width, int height);

    // Size animation
    [[nodiscard]] bool animating() const { return m_animW.animating() || m_animH.animating(); }
    // True when an animation is already heading exactly here, so a repeated
    // layout pass does not restart it from the current interpolated size.
    [[nodiscard]] bool targeting(int width, int height) const;
    // Start from the presented size, not the committed one: mid-animation
    // retargets must continue from what is on screen.
    void animateTo(int width, int height, int durationMs, const AnimationCurve& curve = AnimationCurve{});
    void snapTo(int width, int height);
    // Advance both axes. Returns true when the presented size moved this tick,
    // which is the caller's cue to re-derive everything drawn from it.
    bool tick(uint64_t nowMsec);

    // Fullscreen
    void createBackdrop(wlr_scene_tree* parent);
    void setBackdropEnabled(bool enabled);
    void setBackdropBox(int x, int y, int width, int height);
    // Re-read the backdrop color after a config reload.
    void reloadBackdropColor();
    // Offsets centering a client buffer that does not match the output. Negative
    // for an oversized buffer, which crops it equally on both sides.
    [[nodiscard]] int offsetX() const { return m_offsetX; }
    [[nodiscard]] int offsetY() const { return m_offsetY; }
    // Follows the client's *committed* fullscreen state, never the scheduled intent: a client mid mode-change (wine)
    // would otherwise render as a stale buffer wearing fullscreen chrome.
    void updateFullscreen(
        bool fullscreen, int tileWidth, int tileHeight, wlr_scene_node* surfaceNode, const wlr_box& geometry
    );

    // Present the buffer at the animated size. `content` and `surfaceClip` use surface coordinates. A scene clip
    // alone cannot express this, because it crops 1:1 and caps the destination at the committed surface size.
    void applyCrop(
        wlr_scene_tree* tree, wlr_surface* surface, const wlr_box& geometry, const wlr_box& content,
        const wlr_box& surfaceClip
    ) const;
    // Undo applyCrop: real buffer size, no source box.
    void resetCrop(wlr_scene_tree* tree, wlr_surface* surface) const;

  private:
    int m_width = 0;
    int m_height = 0;
    AnimatedValue m_animW;
    AnimatedValue m_animH;
    wlr_scene_rect* m_backdrop = nullptr;
    int m_offsetX = 0;
    int m_offsetY = 0;
    bool m_contentCentered = false;
  };

} // namespace umbriel
