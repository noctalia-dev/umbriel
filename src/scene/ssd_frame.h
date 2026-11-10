#pragma once

#include "config/config.h"
#include "scene/adaptive_sampling.h"
#include "scene/ssd_frame_geometry.h"
#include "scene/surface_blur.h"
#include "view/chrome_attachment.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_loop;
struct wl_event_source;
struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct clipped_region;

namespace umbriel {
  class ColorManager;
  struct TabLabel;
  class TabLabelCache;

  class SsdFrame final : public ViewChromeAttachment {
  public:
    // `loop` only drives the fallback that reads an edge whose acquire fence never signals; a preview has none.
    SsdFrame(
        wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent = nullptr,
        ColorManager* colors = nullptr, wl_event_loop* loop = nullptr
    );
    ~SsdFrame() override;
    void layout(const ViewChromeGeometry& geometry) override;
    void setFocused(bool focused) override;
    void setAlpha(float alpha) override;
    // Like surface blur, backdrop blur follows lifecycle fades independently of the chrome's drag opacity.
    void setAlphas(float chromeAlpha, float blurAlpha);
    void reloadConfig() override;
    void setTitle(std::string_view title);
    void setResizable(bool resizable);
    void setMaximizeEnabled(bool enabled);
    // The buttons to show, from the title outward.
    void setButtons(std::span<const TitleBarButton> buttons);
    void setMaximized(bool maximized);
    void setHovered(SsdFramePart part);
    void setPressed(SsdFramePart part);
    void setBlurOptions(const SurfaceBlurOptions& options);
    void setBlurLive(bool live);
    // The window content an adaptive bar takes its color from, and that window's geometry in surface-local
    // coordinates. Borrowed: the view owns the surface.
    void setContentSurface(wlr_surface* surface, const wlr_box& geometry);
    // Re-reads the strip after the window committed, with the geometry the commit left behind.
    void syncContent(const wlr_box& geometry);
    bool maximizeEnabled() const { return m_maximizeEnabled; }
    int titleBarHeight() const { return m_geometry.titleBarHeight; }
    bool contains(const wlr_scene_node* node) const;
    SsdFramePoint pointAt(double x, double y) const;
    void snapshot(wlr_scene_tree* parent) const;
    std::unique_ptr<ViewChromeAttachment> makePreview(wlr_scene_tree* parent) const override;
    void syncPreview(ViewChromeAttachment& preview) const override;

  private:
    void redraw();
    void updateColors();
    void updateAdaptive(bool fromCommit);
    // Resolves the fill, then repaints when it moved what the title or the bare symbols are judged against. Returns
    // whether it repainted, which stops a caller that would otherwise draw with what the repaint replaces.
    bool refreshFill(bool fromCommit);
    void hideAdaptive();
    // What one readback of the edge produced: a value, a refusal, or a readable edge with nothing to average.
    struct StripSample {
      // `Empty` means the strip was read and holds no color (a transparent edge), so the palette is the honest
      // answer; `Refused` means it could not be read (protected or external-only), so the last color still stands.
      enum class State : uint8_t { Fresh, Refused, Empty };
      State state = State::Refused;
      std::optional<std::array<float, 4>> color;
    };
    StripSample averageStrip(const wlr_fbox& strip);
    // Whether `box` of the client buffer changed in this commit's damage. The caller gates it on a commit.
    bool stripTouched(const wlr_fbox& box) const;
    // Whether this commit's explicit-sync acquire point has signalled, or has no fence at all.
    bool stripFenceSignalled() const;
    // The part of the edge worth sampling: the title plus a little slack, as a fraction of the visible width.
    double sampleWidthFraction() const;
    void cacheSampleWidth(const TabLabel* label, const TitleBarLayout& parts);
    // Reads the edge once `delayMsec` has passed, for a fence that outlived its wait or a read the cadence skipped.
    void armStripFallback(std::uint32_t delayMsec);
    static int onStripFallbackTimer(void* data);
    // A one-pixel buffer of `color`, kept until the color changes.
    wlr_buffer* averageBufferFor(const std::array<float, 4>& color);
    // The color the bar shows, which is what the title is drawn on; empty when "window" was not allowed to sample it.
    std::optional<std::array<float, 4>> barFillColor() const;
    // The title's color after the contrast rule.
    std::array<float, 4> resolvedTitleColor() const;
    // Let wlroots keep reusing the mirrored client buffer's texture for in-place shm damage, and withdraw that when the
    // mirror is replaced or cleared.
    void allowInPlaceDamage(wlr_buffer* buffer);
    void withdrawInPlaceDamage();
    void holdLabelBuffer(wlr_buffer*& slot, wlr_buffer* buffer);
    void updateButtonIcon(
        TitleBarButton button, const std::array<float, 4>& color, float alpha, bool visible,
        const std::optional<std::array<float, 4>>& outline
    );
    std::span<const TitleBarButton> visibleButtons() const;
    // The title with `color`'s alpha scaled by `alpha`, re-measuring `parts` against the line's height.
    const TabLabel*
    measureTitle(const std::array<float, 4>& color, float alpha, TitleBarLayout& parts, int radius) const;
    void updateBlur();
    clipped_region frameClip(int x, int y) const;
    void clipBuffer(wlr_scene_buffer* buffer) const;
    int zoomed(int value) const;
    wlr_scene_tree* m_tree = nullptr;
    // Invisible resize bands stay outside the window's animation capture bounds.
    wlr_scene_tree* m_gripTree = nullptr;
    wlr_scene_rect* m_background = nullptr;
    wlr_scene_blur* m_blur = nullptr;
    SurfaceBlurOptions m_blurOptions;
    bool m_blurLive = false;
    wlr_scene_buffer* m_label = nullptr;
    // The same title in the shadow color, one pixel down and right, so it stays readable on any bar color.
    wlr_scene_buffer* m_labelShadow = nullptr;
    // Consumer locks on the title and shadow: the scene drops its own after uploading, and snapshots copy these.
    wlr_buffer* m_labelBuffer = nullptr;
    wlr_buffer* m_shadowBuffer = nullptr;
    // Paints the bar with the top strip of the window content; kept below the label and above the palette color.
    wlr_scene_buffer* m_adaptive = nullptr;
    wlr_surface* m_content = nullptr;
    wlr_box m_contentGeometry{};
    // Whether the mirrored buffer carries the mark that lets wlroots reuse its texture for in-place shm damage.
    bool m_inPlaceDamage = false;
    // A read the cadence or an unsignalled fence held back: the fallback timer or the next update still owes it.
    bool m_stripDeferred = false;
    AdaptiveSampling m_sampling;
    // Bumped on every commit, so a fence that was read anyway is not read again until the client commits anew.
    std::uint64_t m_commitGeneration = 0;
    // Runs the fallback for a read the loop would otherwise owe until the next commit; a preview has no loop.
    wl_event_loop* m_loop = nullptr;
    wl_event_source* m_stripTimer = nullptr;
    // Width of the title plus a little slack, in logical pixels; 0 samples the whole edge.
    int m_sampleWidth = 0;
    // What the strip node last mirrored, so an unrelated commit costs nothing.
    wlr_buffer* m_adaptiveBuffer = nullptr;
    wlr_fbox m_adaptiveStrip{};
    std::optional<std::array<float, 4>> m_averageColor;
    // What "window" paid a strip readback for, taken once per edge change.
    std::optional<std::array<float, 4>> m_contrastSample;
    // The last read found an edge with no color in it, so the palette is what shows and `barFillColor()` returns it
    // live.
    bool m_contrastSampleEmpty = false;
    // The color baked into `m_averageBuffer`, which is not the same thing as the current mean.
    std::array<float, 4> m_averageBufferColor{};
    wlr_buffer* m_averageBuffer = nullptr;
    // Scratch for one strip readback, reused while consecutive reads cover the same area.
    std::vector<uint32_t> m_stripPixels;
    // The mode the contents were built for: a switch is not a commit, so it must force the work.
    TitleBarColorSource m_adaptiveMode = TitleBarColorSource::Fixed;
    wl_output_transform m_adaptiveTransform = WL_OUTPUT_TRANSFORM_NORMAL;
    // Whether the last update sampled for contrast, so turning the setting on reads the edge without a repaint.
    bool m_contrastSampling = false;
    int m_adaptiveWidth = 0;
    int m_adaptiveHeight = 0;
    float m_adaptiveOpacity = 0.0F;
    ColorManager* m_colors = nullptr;
    wlr_scene_rect* m_close = nullptr;
    wlr_scene_rect* m_maximize = nullptr;
    wlr_scene_rect* m_minimize = nullptr;
    std::array<wlr_scene_buffer*, kTitleBarButtonCount> m_icons{};
    // Keep a consumer lock for snapshots and renderer recovery after the scene uploads an icon's texture.
    std::array<wlr_buffer*, kTitleBarButtonCount> m_iconBuffers{};
    std::array<std::array<float, 4>, kTitleBarButtonCount> m_iconColors{};
    std::array<std::optional<std::array<float, 4>>, kTitleBarButtonCount> m_iconOutlines{};
    std::array<int, kTitleBarButtonCount> m_iconSizes{};
    std::vector<TitleBarButton> m_buttons;
    std::array<wlr_scene_rect*, 4> m_grips{};
    std::shared_ptr<TabLabelCache> m_labels;
    ViewChromeGeometry m_geometry;
    std::string m_title;
    bool m_focused = false;
    bool m_resizable = false;
    bool m_maximizeEnabled = true;
    bool m_maximized = false;
    bool m_preview = false;
    float m_alpha = 1.0F;
    float m_blurAlpha = 1.0F;
    SsdFramePart m_hovered = SsdFramePart::None;
    SsdFramePart m_pressed = SsdFramePart::None;
  };
} // namespace umbriel
