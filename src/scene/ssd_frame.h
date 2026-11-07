#pragma once

#include "scene/ssd_frame_geometry.h"
#include "scene/surface_blur.h"
#include "view/chrome_attachment.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wlr_client_buffer;
struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_buffer;
struct clipped_region;

namespace umbriel {
  class ColorManager;
  struct TabLabel;
  class TabLabelCache;

  class SsdFrame final : public ViewChromeAttachment {
  public:
    SsdFrame(
        wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent = nullptr,
        ColorManager* colors = nullptr
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
    // The window content an adaptive bar takes its colour from, and that window's geometry in surface-local
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
    void hideAdaptive();
    // The mean colour of the strip, read back from the window's committed texture.
    std::optional<std::array<float, 4>> averageStrip(const wlr_fbox& strip) const;
    // A one-pixel buffer of `color`, kept until the colour changes.
    wlr_buffer* averageBufferFor(const std::array<float, 4>& color);
    // The mirrored client buffer is locked by the scene; tell wlroots to ignore that lock so it can still reuse the
    // texture for in-place shm damage, and undo it when the mirror is replaced or cleared.
    void ignoreLock(wlr_buffer* buffer);
    void releaseIgnoredLock();
    void updateButtonIcon(
        TitleBarButton button, const std::array<float, 4>& color, float alpha, bool visible,
        const std::optional<std::array<float, 4>>& outline
    );
    std::span<const TitleBarButton> visibleButtons() const;
    // The title as the cache renders it, with `color`'s alpha scaled by `alpha`, re-measuring `parts` against the
    // line's height. The live draw and the close snapshot share this so both ask for the same label.
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
    // The same title in the shadow colour, one pixel down and right, so it stays readable on any bar colour.
    wlr_scene_buffer* m_labelShadow = nullptr;
    // Paints the bar with the top strip of the window content; kept below the label and above the palette colour.
    wlr_scene_buffer* m_adaptive = nullptr;
    wlr_surface* m_content = nullptr;
    wlr_box m_contentGeometry{};
    wlr_client_buffer* m_ignoredBuffer = nullptr;
    // What the strip node last mirrored, so a commit that leaves the top edge alone costs nothing.
    wlr_buffer* m_adaptiveBuffer = nullptr;
    wlr_fbox m_adaptiveStrip{};
    // The client buffer the node's current contents came from, so a swap re-mirrors or re-reads it.
    wlr_buffer* m_adaptiveContent = nullptr;
    std::optional<std::array<float, 4>> m_averageColor;
    wlr_buffer* m_averageBuffer = nullptr;
    wl_output_transform m_adaptiveTransform = WL_OUTPUT_TRANSFORM_NORMAL;
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
