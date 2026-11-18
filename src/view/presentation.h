#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
}

#include <cmath>
#include <cstdint>
#include <optional>

struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  // Pixel endpoints belong to a position transition, not to this frame's resize state.
  struct PositionPixelAlignment {
    std::optional<double> from;
    std::optional<double> target;
    double roundingMargin = 0.25;

    void update(const AnimatedValue& position, double extent, double scale, double origin, bool resizing);

  private:
    uint64_t m_transitionId = 0;
    double m_scale = 0.0;
    double m_origin = 0.0;
  };

  // The size a view is currently *drawn* at, which can differ from the committed size: an unacked layout configure, a
  // resize animation, or fullscreen (a mismatched buffer is centered, not scaled). Borders, shadow, blur and output
  // clipping must read this, not the committed geometry. Holds no reference back to its View.
  class ViewPresentation {
  public:
    // Presented size
    int width() const { return static_cast<int>(std::lround(m_width)); }
    int height() const { return static_cast<int>(std::lround(m_height)); }
    double fractionalWidth() const { return m_width; }
    double fractionalHeight() const { return m_height; }
    // Adopt a size outright: map, animation end, or an interactive resize that
    // owns the size itself.
    void setSize(double width, double height);
    // Adopt a size only if it is real and no animation currently owns it.
    void track(int width, int height);

    // Size animation
    // A finished resize still owns its fractional size while the accompanying move runs.
    bool animating() const { return m_animW.animating() || m_animH.animating() || m_alignmentHeld; }
    const AnimatedValue& animation() const { return m_animW.animating() ? m_animW : m_animH; }
    // True when an animation is already heading exactly here, so a repeated
    // layout pass does not restart it from the current interpolated size.
    bool targeting(int width, int height) const;
    // Start from the presented size, not the committed one: mid-animation
    // retargets must continue from what is on screen.
    void animateTo(int width, int height, int durationMs, const AnimationCurve& curve = AnimationCurve{});
    void setRegion(int width, int height);
    // Output-grid origins relative to a position transition's logical and presented endpoints.
    struct PositionOrigins {
      double from;
      double to;
      double presentedFrom;
      double presentedTo;
      bool animating = false;
    };
    // Advance both axes. Returns true when the presented size moved this tick,
    // which is the caller's cue to re-derive everything drawn from it.
    // Origins locate the output grid relative to the near edges. Moving positions supply both
    // endpoints and their running state. Optional physical far edges preserve fullscreen coverage.
    bool tick(
        double nowMsec, double pixelsPerUnit = 1.0, double originX = 0.0, double originY = 0.0,
        std::optional<double> targetWidthPixels = std::nullopt, std::optional<double> targetHeightPixels = std::nullopt,
        std::optional<PositionOrigins> positionX = std::nullopt, std::optional<PositionOrigins> positionY = std::nullopt
    );

    // Fullscreen
    void createBackdrop(wlr_scene_tree* parent);
    void setBackdropEnabled(bool enabled);
    // Whether a fullscreen window hides what is behind it. The backdrop shows
    // only for an opaque fullscreen window with a valid tile.
    void setFullscreenOpaque(bool opaque);
    void setBackdropBox(int x, int y, int width, int height);
    // Re-read the backdrop color after a config reload.
    void reloadBackdropColor();
    // Offsets centering a client buffer that does not match the output. Negative
    // for an oversized buffer, which crops it equally on both sides.
    int offsetX() const { return m_offsetX; }
    int offsetY() const { return m_offsetY; }
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
    double m_width = 0;
    double m_height = 0;
    AnimatedValue m_animW;
    AnimatedValue m_animH;
    struct PixelAlignment {
      double scale;
      double origin;
      double fromPixels;
      double targetPixels;
    };
    std::optional<PixelAlignment> m_alignmentW;
    std::optional<PixelAlignment> m_alignmentH;
    bool m_alignmentHeld = false;
    wlr_scene_rect* m_backdrop = nullptr;
    int m_offsetX = 0;
    int m_offsetY = 0;
    bool m_contentCentered = false;
    bool m_fullscreenSized = false;
    bool m_fullscreenOpaque = true;
  };

} // namespace umbriel
