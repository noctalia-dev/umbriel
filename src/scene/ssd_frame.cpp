#include "scene/ssd_frame.h"

#include "config/config.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "scene/title_bar_icon.h"
#include "server/color_manager.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace umbriel {
  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }

    // An unfocused window keeps its own bar colour, dimmed over the palette's unfocused tone.
    constexpr float kAdaptiveUnfocusedOpacity = 0.65F;

    bool fboxEqual(const wlr_fbox& a, const wlr_fbox& b) {
      return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    // An adaptive bar carries any colour the window paints, so text and symbols keep a dark edge under them.
    const std::array<float, 4> kAdaptiveShadowColor{0.0F, 0.0F, 0.0F, 0.55F};
    std::optional<std::array<float, 4>> adaptiveOutline() { return kAdaptiveShadowColor; }
  } // namespace

  SsdFrame::SsdFrame(
      wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent, ColorManager* colors
  )
      : m_tree(wlr_scene_tree_create(parent)),
        m_gripTree(wlr_scene_tree_create(inputParent != nullptr ? inputParent : parent)), m_colors(colors),
        m_labels(std::move(labels)) {
    if (inputParent != nullptr && parent->node.parent == inputParent) {
      // Client surfaces and popups keep input priority over invisible resize grips.
      wlr_scene_node_place_below(&m_gripTree->node, &parent->node);
    }
    const float clear[4]{};
    m_background = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_close = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_maximize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_minimize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    for (auto*& grip : m_grips) {
      grip = wlr_scene_rect_create(m_gripTree, 1, 1, clear);
    }
    wlr_scene_node_set_enabled(&m_tree->node, false);
    wlr_scene_node_set_enabled(&m_gripTree->node, false);
  }

  SsdFrame::~SsdFrame() {
    if (m_blur != nullptr) {
      wlr_scene_node_destroy(&m_blur->node);
    }
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
    for (auto* buffer : m_iconBuffers) {
      if (buffer != nullptr) {
        wlr_buffer_unlock(buffer);
      }
    }
  }

  void SsdFrame::layout(const ViewChromeGeometry& geometry) {
    if (m_geometry == geometry) {
      return;
    }
    m_geometry = geometry;
    redraw();
  }
  void SsdFrame::setFocused(bool focused) {
    if (m_focused == focused) {
      return;
    }
    m_focused = focused;
    redraw();
  }
  void SsdFrame::setAlpha(float alpha) { setAlphas(alpha, alpha); }
  void SsdFrame::setAlphas(float chromeAlpha, float blurAlpha) {
    chromeAlpha = std::clamp(chromeAlpha, 0.0F, 1.0F);
    blurAlpha = std::clamp(blurAlpha, 0.0F, 1.0F);
    if (m_alpha == chromeAlpha && m_blurAlpha == blurAlpha) {
      return;
    }
    m_alpha = chromeAlpha;
    m_blurAlpha = blurAlpha;
    updateColors();
  }
  void SsdFrame::reloadConfig() { redraw(); }
  void SsdFrame::setTitle(std::string_view title) {
    if (m_title == title) {
      return;
    }
    m_title = title;
    redraw();
  }
  void SsdFrame::setResizable(bool resizable) {
    if (m_resizable == resizable) {
      return;
    }
    m_resizable = resizable;
    redraw();
  }
  void SsdFrame::setMaximizeEnabled(bool enabled) {
    if (m_maximizeEnabled == enabled) {
      return;
    }
    m_maximizeEnabled = enabled;
    updateColors();
  }
  void SsdFrame::setButtons(std::span<const TitleBarButton> buttons) {
    if (std::ranges::equal(m_buttons, buttons)) {
      return;
    }
    m_buttons.assign(buttons.begin(), buttons.end());
    redraw();
  }
  void SsdFrame::setMaximized(bool maximized) { m_maximized = maximized; }
  void SsdFrame::setHovered(SsdFramePart part) {
    if (m_hovered == part) {
      return;
    }
    m_hovered = part;
    updateColors();
  }
  void SsdFrame::setPressed(SsdFramePart part) {
    if (m_pressed == part) {
      return;
    }
    m_pressed = part;
    updateColors();
  }

  void SsdFrame::setBlurOptions(const SurfaceBlurOptions& options) {
    m_blurOptions = options;
    updateBlur();
  }
  void SsdFrame::setBlurLive(bool live) {
    m_blurLive = live;
    updateBlur();
  }
  void SsdFrame::setContentSurface(wlr_surface* surface) {
    if (m_content == surface) {
      return;
    }
    m_content = surface;
    updateAdaptive();
  }
  void SsdFrame::syncContent() { updateAdaptive(); }

  void SsdFrame::updateBlur() {
    const auto& cfg = config().appearance.blur;
    const auto& colors = config().colors.titleBar;
    const float backgroundAlpha = (m_focused ? colors.background : colors.backgroundUnfocused)[3];
    // The title bar has its own transparency, independent of the client's opaque region or alpha mask.
    const bool enabled = m_tree->node.enabled
        && cfg.enabled
        && m_blurOptions.enabled.value_or(false)
        && backgroundAlpha * m_alpha < 1.0F
        && backgroundAlpha >= m_blurOptions.ignoreAlpha;
    if (!enabled) {
      if (m_blur != nullptr) {
        wlr_scene_node_set_enabled(&m_blur->node, false);
      }
      return;
    }
    if (m_blur == nullptr) {
      // Blur the backdrop below the window, excluding the window's own client content near the title bar seam.
      m_blur = wlr_scene_blur_create(m_tree->node.parent, m_background->width, m_background->height);
      if (m_blur == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_blur->node);
    }
    wlr_scene_node_set_enabled(&m_blur->node, true);
    wlr_scene_node_set_position(
        &m_blur->node, m_tree->node.x + m_background->node.x, m_tree->node.y + m_background->node.y
    );
    wlr_scene_blur_set_size(m_blur, m_background->width, m_background->height);
    wlr_scene_blur_set_corner_radii(m_blur, m_background->corners);
    wlr_scene_blur_set_clipped_region(m_blur, m_background->clipped_region);
    wlr_scene_blur_set_should_only_blur_bottom_layer(
        m_blur, m_blurOptions.optimized.value_or(cfg.optimized) && !m_blurLive
    );
    wlr_scene_blur_set_alpha(m_blur, m_blurAlpha);
  }

  void SsdFrame::updateAdaptive() {
    // The palette colour stays painted underneath: a window whose top edge is transparent, or one that has not
    // committed a buffer yet, keeps the configured bar instead of leaving a hole.
    const auto& style = config().appearance.titleBar;
    wlr_buffer* buffer = nullptr;
    if (style.colorSource == TitleBarColorSource::Window
        && m_content != nullptr
        && m_content->buffer != nullptr
        && m_tree->node.enabled
        && m_background->width > 0
        && m_background->height > 0) {
      buffer = &m_content->buffer->base;
    }
    if (buffer == nullptr) {
      if (m_adaptive != nullptr) {
        wlr_scene_node_set_enabled(&m_adaptive->node, false);
      }
      m_adaptiveBuffer = nullptr;
      return;
    }
    if (m_adaptive == nullptr) {
      m_adaptive = wlr_scene_buffer_create(m_tree, nullptr);
      if (m_adaptive == nullptr) {
        return;
      }
      m_adaptive->point_accepts_input = rejectInput;
      wlr_scene_node_place_above(&m_adaptive->node, &m_background->node);
    }
    wlr_fbox source{};
    wlr_surface_get_buffer_source_box(m_content, &source);
    const wlr_fbox strip = titleBarStripSourceBox(
        source, buffer->width, buffer->height, m_content->current.width, m_content->current.height,
        static_cast<uint32_t>(m_content->current.transform), kTitleBarStripHeight
    );
    // Only what the bar shows matters, and the damage is already in buffer coordinates, which is where the strip is.
    pixman_region32_t stripRegion;
    pixman_region32_init_rect(&stripRegion, strip.x, strip.y, strip.width, strip.height);
    const bool touched = pixman_region32_intersect(&stripRegion, &stripRegion, &m_content->buffer_damage);
    pixman_region32_fini(&stripRegion);
    const float opacity = m_alpha * (m_focused ? 1.0F : kAdaptiveUnfocusedOpacity);
    const bool reshaped = m_adaptiveBuffer != buffer
        || !fboxEqual(m_adaptiveStrip, strip)
        || m_adaptiveTransform != m_content->current.transform
        || m_adaptiveWidth != m_background->width
        || m_adaptiveHeight != m_background->height
        || m_adaptiveOpacity != opacity;
    // A commit that leaves the top edge alone, and changes nothing about the bar, costs nothing.
    if (!touched && !reshaped) {
      return;
    }
    if (touched || m_adaptiveBuffer != buffer) {
      // Mirror the window the same way the overview mirrors a card: the surface's committed buffer plus its damage, and
      // the explicit-sync acquire point when the client uses it, so the bar never samples a half-presented frame.
      wlr_scene_buffer_set_buffer_options options{
          .damage = &m_content->buffer_damage,
          .wait_timeline = nullptr,
          .wait_point = 0,
      };
      if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(m_content)) {
        options.wait_timeline = sync->acquire_timeline;
        options.wait_point = sync->acquire_point;
      }
      wlr_scene_buffer_set_buffer_with_options(m_adaptive, buffer, &options);
    }
    m_adaptiveBuffer = buffer;
    m_adaptiveStrip = strip;
    m_adaptiveTransform = m_content->current.transform;
    m_adaptiveWidth = m_background->width;
    m_adaptiveHeight = m_background->height;
    m_adaptiveOpacity = opacity;
    wlr_scene_buffer_set_source_box(m_adaptive, &strip);
    wlr_scene_buffer_set_transform(m_adaptive, m_content->current.transform);
    wlr_scene_buffer_set_dest_size(m_adaptive, m_background->width, m_background->height);
    wlr_scene_buffer_set_corner_radii(m_adaptive, m_background->corners);
    // Unfocus dims the window's own colour over the palette's unfocused tone rather than dropping back to it.
    wlr_scene_buffer_set_opacity(m_adaptive, opacity);
    if (m_colors != nullptr) {
      m_colors->applySurfaceDescriptionToBuffer(m_content, m_adaptive);
    }
    wlr_scene_node_set_position(&m_adaptive->node, m_background->node.x, m_background->node.y);
    wlr_scene_node_set_enabled(&m_adaptive->node, true);
  }

  int SsdFrame::zoomed(int value) const { return static_cast<int>(std::lround(value * m_geometry.zoom)); }

  clipped_region SsdFrame::frameClip(int x, int y) const {
    const int radius = zoomed(nestedRadius(m_geometry.cornerRadius, m_geometry.borderInset));
    if (radius <= 0) {
      return {};
    }
    return {
        .area =
            {-x, zoomed(-m_geometry.titleBarHeight) - y, zoomed(m_geometry.contentWidth),
             zoomed(m_geometry.contentHeight + m_geometry.titleBarHeight)},
        .corners = corner_radii_all(radius),
        .inside = true,
    };
  }

  void SsdFrame::clipBuffer(wlr_scene_buffer* buffer) const {
    const auto clip = frameClip(buffer->node.x, buffer->node.y);
    wlr_scene_buffer_set_corner_radii(buffer, clip.corners);
    wlr_scene_buffer_set_corner_box(buffer, clip.inside ? &clip.area : nullptr);
  }

  std::span<const TitleBarButton> SsdFrame::visibleButtons() const {
    const auto& style = config().appearance.titleBar;
    return style.buttonGlyph != TitleBarButtonGlyph::Never || style.buttonBackground
        ? std::span<const TitleBarButton>(m_buttons)
        : std::span<const TitleBarButton>();
  }

  void SsdFrame::updateButtonIcon(
      TitleBarButton button, const std::array<float, 4>& color, float alpha, bool visible,
      const std::optional<std::array<float, 4>>& outline
  ) {
    const auto index = static_cast<size_t>(button);
    auto*& icon = m_icons[index];
    const std::array<wlr_scene_rect*, kTitleBarButtonCount> backgrounds{m_minimize, m_maximize, m_close};
    const auto* background = backgrounds[index];
    if (!visible || !background->node.enabled) {
      if (icon != nullptr) {
        wlr_scene_node_set_enabled(&icon->node, false);
      }
      return;
    }
    const int size = titleBarButtonDiameter(m_geometry.titleBarHeight, config().appearance.titleBar.buttonSize);
    const float scale = std::max(1.0F, m_geometry.scale);
    // At integer scales the symbol maps 1:1 onto the button. At fractional scales the button's physical size depends
    // on its position, and the renderer copies a buffer within one pixel of that size texel for texel, leaving the
    // symbol half a pixel off center. Twice the resolution makes it resample symmetrically instead.
    const int pixels = static_cast<int>(std::ceil(size * scale)) * (scale == std::floor(scale) ? 1 : 2);
    if (icon == nullptr
        || m_iconSizes[index] != pixels
        || m_iconColors[index] != color
        || m_iconOutlines[index] != outline) {
      wlr_buffer* buffer = renderTitleBarIcon(button, pixels, color, outline);
      if (icon == nullptr) {
        icon = wlr_scene_buffer_create(m_tree, buffer);
        if (icon != nullptr) {
          icon->point_accepts_input = rejectInput;
        }
      } else {
        wlr_scene_buffer_set_buffer(icon, buffer);
      }
      if (icon == nullptr) {
        wlr_buffer_drop(buffer);
        return;
      }
      if (auto* previous = std::exchange(m_iconBuffers[index], wlr_buffer_lock(buffer)); previous != nullptr) {
        wlr_buffer_unlock(previous);
      }
      wlr_buffer_drop(buffer);
      m_iconSizes[index] = pixels;
      m_iconColors[index] = color;
      m_iconOutlines[index] = outline;
    }
    wlr_scene_buffer_set_dest_size(icon, background->width, background->height);
    wlr_scene_buffer_set_opacity(icon, alpha);
    wlr_scene_node_set_position(&icon->node, background->node.x, background->node.y);
    clipBuffer(icon);
    wlr_scene_node_set_enabled(&icon->node, true);
  }

  void SsdFrame::updateColors() {
    const auto& colors = config().colors.titleBar;
    const auto& style = config().appearance.titleBar;
    const bool backgrounds = style.buttonBackground;
    float color[4];
    const auto& background = m_focused ? colors.background : colors.backgroundUnfocused;
    premultiplied(color, background, m_alpha);
    wlr_scene_rect_set_color(m_background, color);
    const float contrast =
        background[0] * 0.2126F + background[1] * 0.7152F + background[2] * 0.0722F < 0.5F ? 1.0F : 0.0F;
    const auto button = [&](wlr_scene_rect* rect, const std::array<float, 4>& base,
                            const std::array<float, 4>& idleGlyph, const std::array<float, 4>& hoverGlyph,
                            SsdFramePart part, bool enabled, TitleBarButton which) {
      auto value = base;
      const bool hovered = m_hovered == part;
      const bool pressed = m_pressed == part;
      const float alpha = !enabled ? 0.35F : (hovered ? 1.0F : 0.85F);
      if (enabled && hovered) {
        for (size_t i = 0; i < 3; ++i) {
          value[i] += (contrast - value[i]) * 0.2F;
        }
      }
      if (enabled && pressed) {
        for (size_t i = 0; i < 3; ++i) {
          value[i] *= 0.8F;
        }
      }
      premultiplied(color, value, backgrounds ? m_alpha * alpha : 0.0F);
      wlr_scene_rect_set_color(rect, color);
      const auto& glyphColor = enabled && (hovered || pressed) ? hoverGlyph : idleGlyph;
      const bool glyphVisible = style.buttonGlyph == TitleBarButtonGlyph::Always
          || (style.buttonGlyph == TitleBarButtonGlyph::Hover && (hovered || pressed));
      updateButtonIcon(
          which, glyphColor, m_alpha * (enabled ? 1.0F : 0.35F), glyphVisible,
          style.colorSource == TitleBarColorSource::Window ? adaptiveOutline() : std::nullopt
      );
    };
    button(
        m_close, colors.close, colors.closeGlyph, colors.closeGlyphHover, SsdFramePart::Close, true,
        TitleBarButton::Close
    );
    button(
        m_maximize, colors.maximize, colors.maximizeGlyph, colors.maximizeGlyphHover, SsdFramePart::Maximize,
        m_maximizeEnabled, TitleBarButton::Maximize
    );
    button(
        m_minimize, colors.minimize, colors.minimizeGlyph, colors.minimizeGlyphHover, SsdFramePart::Minimize, true,
        TitleBarButton::Minimize
    );
    if (m_label != nullptr) {
      wlr_scene_buffer_set_opacity(m_label, m_alpha);
    }
    if (m_labelShadow != nullptr) {
      wlr_scene_buffer_set_opacity(m_labelShadow, m_alpha);
    }
    updateAdaptive();
    updateBlur();
  }

  void SsdFrame::redraw() {
    const auto& g = m_geometry;
    const int h = g.titleBarHeight;
    const bool available = !g.suppressed && g.contentWidth > 0 && g.contentHeight > 0;
    const bool shown = available && h > 0;
    wlr_scene_node_set_enabled(&m_tree->node, shown);
    wlr_scene_node_set_enabled(&m_gripTree->node, available && m_resizable && !m_preview);
    if (!available) {
      updateAdaptive();
      updateBlur();
      return;
    }
    wlr_scene_node_set_position(&m_tree->node, zoomed(g.contentX), zoomed(g.contentY));
    wlr_scene_node_set_position(&m_gripTree->node, zoomed(g.contentX), zoomed(g.contentY));
    const auto place = [&](wlr_scene_rect* rect, const wlr_box& box) {
      wlr_scene_node_set_position(&rect->node, zoomed(box.x), zoomed(box.y));
      wlr_scene_rect_set_size(
          rect, std::max(1, zoomed(box.x + box.width) - zoomed(box.x)),
          std::max(1, zoomed(box.y + box.height) - zoomed(box.y))
      );
      rect->accepts_input = !m_preview;
    };
    const int band = g.borderInset + kSsdResizeBand;
    const std::array<wlr_box, 4> grips{{
        {-band, -h - band, g.contentWidth + 2 * band, band},
        {-band, g.contentHeight, g.contentWidth + 2 * band, band},
        {-band, -h, band, g.contentHeight + h},
        {g.contentWidth, -h, band, g.contentHeight + h},
    }};
    for (size_t i = 0; i < m_grips.size(); ++i) {
      place(m_grips[i], grips[i]);
      wlr_scene_node_set_enabled(&m_grips[i]->node, m_resizable && !m_preview);
    }
    if (!shown) {
      updateAdaptive();
      updateBlur();
      return;
    }
    place(m_background, {0, -h, g.contentWidth, h});
    wlr_scene_rect_set_clipped_region(m_background, frameClip(m_background->node.x, m_background->node.y));
    const auto& style = config().appearance.titleBar;
    const int radius = nestedRadius(g.cornerRadius, g.borderInset);
    TitleBarLayout parts =
        titleBarLayout(g.contentWidth, h, style.buttonPosition, visibleButtons(), style.buttonGeometry(), radius);
    const auto drawButton = [&](wlr_scene_rect* button, const std::optional<wlr_box>& slot) {
      if (!slot) {
        wlr_scene_node_set_enabled(&button->node, false);
        return;
      }
      wlr_box box = *slot;
      box.y -= h;
      place(button, box);
      const int buttonRadius = style.buttonCornerRadius >= 0 ? style.buttonCornerRadius : g.cornerRadius;
      wlr_scene_rect_set_corner_radius(button, std::min(zoomed(box.width) / 2, zoomed(buttonRadius)));
      wlr_scene_rect_set_clipped_region(button, frameClip(button->node.x, button->node.y));
      wlr_scene_node_set_enabled(&button->node, true);
    };
    drawButton(m_close, parts.button(TitleBarButton::Close));
    drawButton(m_maximize, parts.button(TitleBarButton::Maximize));
    drawButton(m_minimize, parts.button(TitleBarButton::Minimize));
    const auto& colors = config().colors.titleBar;
    const std::array<float, 4>& textColor = m_focused ? colors.text : colors.textUnfocused;
    TabLabelRequest request{
        .text = m_title,
        .color = textColor,
        .font = style.font,
        .maxWidth = parts.title.width,
        .scale = std::max(1.0, std::ceil(static_cast<double>(g.scale))),
    };
    const TabLabel* label = m_labels->label(request);
    if (label != nullptr) {
      // The line's measured height determines how far the arc reaches into its horizontal space.
      parts = titleBarLayout(
          g.contentWidth, h, style.buttonPosition, visibleButtons(), style.buttonGeometry(), radius, label->height
      );
      if (label->width > parts.title.width) {
        request.maxWidth = parts.title.width;
        label = m_labels->label(request);
      }
    }
    // A window-coloured bar can be lighter or darker than the palette, so the title keeps a dark edge under it.
    const bool shadowed = style.colorSource == TitleBarColorSource::Window;
    const TabLabel* shadow = nullptr;
    if (shadowed && label != nullptr) {
      TabLabelRequest shadowRequest = request;
      shadowRequest.color = kAdaptiveShadowColor;
      shadow = m_labels->label(shadowRequest);
    }
    if (label == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, label->buffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, m_adaptive != nullptr ? &m_adaptive->node : &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, label->buffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(label->width)), std::max(1, zoomed(label->height)));
      wlr_scene_node_set_position(&m_label->node, zoomed(parts.title.x), zoomed(-h + (h - label->height) / 2));
      clipBuffer(m_label);
    }
    if (shadow == nullptr) {
      if (m_labelShadow != nullptr) {
        wlr_scene_node_destroy(&m_labelShadow->node);
        m_labelShadow = nullptr;
      }
    } else {
      const int offset = std::max(1, zoomed(1));
      if (m_labelShadow == nullptr) {
        m_labelShadow = wlr_scene_buffer_create(m_tree, shadow->buffer);
        m_labelShadow->point_accepts_input = rejectInput;
        wlr_scene_node_place_below(&m_labelShadow->node, m_label != nullptr ? &m_label->node : &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_labelShadow, shadow->buffer);
      }
      wlr_scene_buffer_set_dest_size(
          m_labelShadow, std::max(1, zoomed(shadow->width)), std::max(1, zoomed(shadow->height))
      );
      wlr_scene_node_set_position(
          &m_labelShadow->node, zoomed(parts.title.x) + offset, zoomed(-h + (h - shadow->height) / 2) + offset
      );
    }
    updateColors();
  }

  bool SsdFrame::contains(const wlr_scene_node* node) const {
    for (const wlr_scene_node* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == &m_tree->node || current == &m_gripTree->node) {
        return true;
      }
    }
    return false;
  }
  SsdFramePoint SsdFrame::pointAt(double x, double y) const {
    if (m_preview || m_geometry.suppressed || (!m_tree->node.enabled && !m_gripTree->node.enabled)) {
      return {};
    }
    const auto& style = config().appearance.titleBar;
    return ssdFramePointAt(
        m_geometry.contentWidth, m_geometry.contentHeight, m_geometry.titleBarHeight, m_geometry.borderInset,
        m_resizable, style.buttonPosition, visibleButtons(), style.buttonGeometry(), x, y,
        nestedRadius(m_geometry.cornerRadius, m_geometry.borderInset)
    );
  }

  void SsdFrame::snapshot(wlr_scene_tree* parent) const {
    if (!m_tree->node.enabled) {
      return;
    }
    auto* tree = parent;
    if (m_blur != nullptr && m_blur->node.enabled) {
      auto* copy = wlr_scene_blur_create(tree, m_blur->width, m_blur->height);
      if (copy != nullptr) {
        wlr_scene_node_lower_to_bottom(&copy->node);
        wlr_scene_blur_set_corner_radii(copy, m_blur->corners);
        wlr_scene_blur_set_clipped_region(copy, m_blur->clipped_region);
        wlr_scene_blur_set_alpha(copy, m_blur->alpha);
        wlr_scene_blur_set_should_only_blur_bottom_layer(copy, m_blur->should_only_blur_bottom_layer);
        wlr_scene_node_set_position(&copy->node, m_blur->node.x, m_blur->node.y);
      }
    }
    for (const auto* source : {m_background, m_close, m_maximize, m_minimize}) {
      if (!source->node.enabled) {
        continue;
      }
      auto* copy = wlr_scene_rect_create(tree, source->width, source->height, source->color);
      copy->accepts_input = false;
      wlr_scene_rect_set_corner_radii(copy, source->corners);
      wlr_scene_rect_set_clipped_region(copy, source->clipped_region);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    }
    const auto copyBuffer = [&](const wlr_scene_buffer* source) {
      if (source == nullptr || !source->node.enabled || source->buffer == nullptr) {
        return;
      }
      auto* copy = wlr_scene_buffer_create(tree, source->buffer);
      copy->point_accepts_input = rejectInput;
      wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_buffer_set_corner_radii(copy, source->corners);
      wlr_scene_buffer_set_corner_box(copy, &source->corner_box);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    };
    copyBuffer(m_label);
    for (const auto* icon : m_icons) {
      copyBuffer(icon);
    }
    if (m_adaptive != nullptr && m_adaptive->node.enabled && m_adaptive->buffer != nullptr) {
      auto* copy = wlr_scene_buffer_create(tree, m_adaptive->buffer);
      if (copy != nullptr) {
        copy->point_accepts_input = rejectInput;
        wlr_scene_buffer_set_source_box(copy, &m_adaptive->src_box);
        wlr_scene_buffer_set_transform(copy, m_adaptive->transform);
        wlr_scene_buffer_set_dest_size(copy, m_adaptive->dst_width, m_adaptive->dst_height);
        wlr_scene_buffer_set_corner_radii(copy, m_adaptive->corners);
        wlr_scene_buffer_set_opacity(copy, m_adaptive->opacity);
        wlr_scene_buffer_copy_color(copy, m_adaptive);
        wlr_scene_node_set_position(
            &copy->node, m_tree->node.x + m_adaptive->node.x, m_tree->node.y + m_adaptive->node.y
        );
      }
    }
  }
  std::unique_ptr<ViewChromeAttachment> SsdFrame::makePreview(wlr_scene_tree* parent) const {
    auto preview = std::make_unique<SsdFrame>(parent, m_labels, nullptr, m_colors);
    preview->m_preview = true;
    syncPreview(*preview);
    return preview;
  }
  void SsdFrame::syncPreview(ViewChromeAttachment& preview) const {
    if (auto* frame = dynamic_cast<SsdFrame*>(&preview)) {
      frame->setTitle(m_title);
      frame->setResizable(false);
      frame->setMaximizeEnabled(m_maximizeEnabled);
      frame->setButtons(m_buttons);
      frame->setMaximized(m_maximized);
      frame->setFocused(m_focused);
      frame->setBlurOptions(m_blurOptions);
      // A card shows the same bar, so it needs the same window to take its colour from.
      frame->setContentSurface(m_content);
      frame->reloadConfig();
    }
  }
} // namespace umbriel
