#include "scene/ssd_frame.h"

#include "config/config.h"
#include "core/log.h"
#include "scene/cairo_buffer.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "scene/title_bar_contrast.h"
#include "scene/title_bar_icon.h"
#include "server/color_manager.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <drm_fourcc.h>
#include <utility>
#include <vector>

namespace umbriel {
  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }

    // An unfocused window keeps its own bar colour, dimmed over the palette's unfocused tone.
    constexpr float kAdaptiveUnfocusedOpacity = 0.65F;

    bool fboxEqual(const wlr_fbox& a, const wlr_fbox& b) {
      return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    // The bar can be any colour the window paints, so text and symbols keep a dark edge: scaled by their own alpha, so
    // an invisible symbol leaves no ghost.
    const std::array<float, 4> kAdaptiveShadowColor{0.0F, 0.0F, 0.0F, 0.55F};
    std::optional<std::array<float, 4>> glyphOutline(float glyphAlpha) {
      if (glyphAlpha <= 0.0F) {
        return std::nullopt;
      }
      std::array<float, 4> outline = kAdaptiveShadowColor;
      outline[3] *= glyphAlpha;
      return outline;
    }
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
    withdrawInPlaceDamage();
    if (m_blur != nullptr) {
      wlr_scene_node_destroy(&m_blur->node);
    }
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
    if (m_averageBuffer != nullptr) {
      wlr_buffer_unlock(m_averageBuffer);
      m_averageBuffer = nullptr;
    }
    for (wlr_buffer* buffer : {m_labelBuffer, m_shadowBuffer}) {
      if (buffer != nullptr) {
        wlr_buffer_unlock(buffer);
      }
    }
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
  void SsdFrame::setContentSurface(wlr_surface* surface, const wlr_box& geometry) {
    if (m_content == surface && wlr_box_equal(&m_contentGeometry, &geometry)) {
      return;
    }
    m_content = surface;
    m_contentGeometry = geometry;
    updateAdaptive(false);
  }
  void SsdFrame::syncContent(const wlr_box& geometry) {
    m_contentGeometry = geometry;
    // A new edge colour can flip the title, which is a label re-measure. It has to be redrawn from here: redraw() calls
    // updateAdaptive(), so the flip cannot request its own repaint from inside it.
    const std::array<float, 4> before = resolvedTitleColor();
    updateAdaptive(true);
    if (!(resolvedTitleColor() == before)) {
      redraw();
    }
  }

  void SsdFrame::allowInPlaceDamage(wlr_buffer* buffer) {
    m_ignoredLock = umbrielfx_scene_buffer_ignore_client_lock(buffer) != nullptr;
  }

  void SsdFrame::withdrawInPlaceDamage() {
    if (!m_ignoredLock) {
      return;
    }
    m_ignoredLock = false;
    // Ask the scene buffer which buffer it holds: a remembered client buffer would dangle once the client frees it.
    if (m_adaptive != nullptr && m_adaptive->buffer != nullptr) {
      umbrielfx_scene_buffer_drop_ignored_client_lock(wlr_client_buffer_get(m_adaptive->buffer));
    }
  }

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

  void SsdFrame::holdLabelBuffer(wlr_buffer*& slot, wlr_buffer* buffer) {
    if (slot == buffer) {
      return;
    }
    wlr_buffer* previous = std::exchange(slot, buffer != nullptr ? wlr_buffer_lock(buffer) : nullptr);
    if (previous != nullptr) {
      wlr_buffer_unlock(previous);
    }
  }

  void SsdFrame::hideAdaptive() {
    if (m_adaptive == nullptr) {
      return;
    }
    wlr_scene_node_set_enabled(&m_adaptive->node, false);
    // Drop the mirror too: its lock would withhold the client's wl_buffer.release and stall a client that waits on it.
    withdrawInPlaceDamage();
    wlr_scene_buffer_set_buffer(m_adaptive, nullptr);
    m_adaptiveBuffer = nullptr;
  }

  SsdFrame::StripSample SsdFrame::averageStrip(const wlr_fbox& strip) {
    // The only readback in this class, and the only cost average mode cannot avoid: wlroots finishes the frame first.
    // A preview shows what its window resolved, so reading back per card would only multiply that cost.
    if (m_preview || m_content == nullptr || m_content->buffer == nullptr || m_content->buffer->texture == nullptr) {
      return {StripSample::State::Refused, std::nullopt};
    }
    // An explicit-sync client's frame is not finished at commit time; reading now would sample a half-rendered frame.
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(m_content);
        sync != nullptr && sync->acquire_timeline != nullptr) {
      bool signalled = false;
      if (!wlr_drm_syncobj_timeline_check(sync->acquire_timeline, sync->acquire_point, 0, &signalled) || !signalled) {
        return {StripSample::State::Deferred, std::nullopt};
      }
    }
    const int left = static_cast<int>(std::floor(strip.x));
    const int top = static_cast<int>(std::floor(strip.y));
    const int right = static_cast<int>(std::ceil(strip.x + strip.width));
    const int bottom = static_cast<int>(std::ceil(strip.y + strip.height));
    const wlr_box box{left, top, std::max(0, right - left), std::max(0, bottom - top)};
    if (box.width <= 0 || box.height <= 0) {
      return {StripSample::State::Empty, std::nullopt};
    }
    // Ask which format the renderer can read: assuming BGRA silently disables the mode without EXT_read_format_bgra.
    const uint32_t format = wlr_texture_preferred_read_format(m_content->buffer->texture);
    std::optional<PixelOrder> order;
    switch (format) {
    case DRM_FORMAT_ARGB8888:
      order = PixelOrder::Argb;
      break;
    case DRM_FORMAT_XRGB8888:
      order = PixelOrder::Xrgb;
      break;
    case DRM_FORMAT_ABGR8888:
      order = PixelOrder::Abgr;
      break;
    case DRM_FORMAT_XBGR8888:
      order = PixelOrder::Xbgr;
      break;
    default:
      break;
    }
    if (!order.has_value()) {
      return {StripSample::State::Refused, std::nullopt};
    }
    m_strip.resize(static_cast<size_t>(box.width) * static_cast<size_t>(box.height));
    const wlr_texture_read_pixels_options options{
        .data = m_strip.data(),
        .format = format,
        .stride = static_cast<uint32_t>(box.width) * 4U,
        .dst_x = 0,
        .dst_y = 0,
        .src_box = box,
    };
    // Refused for a protected or external-only buffer; the last colour, or the palette, stays.
    if (!wlr_texture_read_pixels(m_content->buffer->texture, &options)) {
      return {StripSample::State::Refused, std::nullopt};
    }
    auto mean = averagePixels(m_strip.data(), m_strip.size(), *order);
    if (!mean.has_value()) {
      return {StripSample::State::Empty, std::nullopt};
    }
    logDebug("[title_bar] averaged a {}x{} strip read back from the window", box.width, box.height);
    return {StripSample::State::Fresh, mean};
  }

  std::optional<std::array<float, 4>> SsdFrame::barFillColor() const {
    const auto& style = config().appearance.titleBar;
    const auto& colors = config().colors.titleBar;
    // The palette shows when the adaptive fill has nothing to draw, so it is the fallback: a transparent edge, an
    // unreadable buffer and `"fixed"` all land on it.
    const std::array<float, 4>& palette = m_focused ? colors.background : colors.backgroundUnfocused;
    switch (style.colorSource) {
    case TitleBarColorSource::Average:
      return m_averageColor.value_or(palette);
    case TitleBarColorSource::Window:
      // Mirrored pixels, not a colour: without the sample there is nothing to judge the title against, and the palette
      // is not what is on screen.
      return m_contrastSample;
    case TitleBarColorSource::Fixed:
      break;
    }
    return palette;
  }

  std::array<float, 4> SsdFrame::resolvedTitleColor() const {
    const auto& style = config().appearance.titleBar;
    const auto& colors = config().colors.titleBar;
    const std::array<float, 4>& configured = m_focused ? colors.text : colors.textUnfocused;
    const auto background = barFillColor();
    return style.titleContrast && background.has_value() ? contrastAdjusted(configured, *background) : configured;
  }

  wlr_buffer* SsdFrame::averageBufferFor(const std::array<float, 4>& color) {
    // Compare against the baked colour: `m_averageColor` already holds the new mean, so it would always match.
    if (m_averageBuffer != nullptr && m_averageBufferColor == color) {
      return m_averageBuffer;
    }
    // One pixel stretched over the bar, so no per-frame readback follows.
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_paint(cr);
    cairo_destroy(cr);
    wlr_buffer* created = createCairoBuffer(surface);
    if (created == nullptr) {
      return nullptr;
    }
    // Hold the mean ourselves: once the scene has uploaded it, the scene drops its own lock, which would clear its
    // buffer and leave `snapshot()` with nothing to copy and a rebuilt renderer with no buffer to upload again.
    wlr_buffer* held = wlr_buffer_lock(created);
    wlr_buffer_drop(created);
    if (m_averageBuffer != nullptr) {
      wlr_buffer_unlock(m_averageBuffer);
    }
    m_averageBuffer = held;
    m_averageBufferColor = color;
    return m_averageBuffer;
  }

  void SsdFrame::updateAdaptive(bool fromCommit) {
    // The palette stays painted underneath, so a transparent edge or a missing buffer leaves no hole.
    const auto& style = config().appearance.titleBar;
    const bool average = style.colorSource == TitleBarColorSource::Average;
    wlr_buffer* buffer = style.colorSource != TitleBarColorSource::Fixed
            && m_content != nullptr
            && m_content->buffer != nullptr
            && m_tree->node.enabled
            && m_background->width > 0
            && m_background->height > 0
        ? &m_content->buffer->base
        : nullptr;
    if (buffer == nullptr) {
      hideAdaptive();
      m_adaptiveMode = TitleBarColorSource::Fixed;
      m_averageColor.reset();
      m_contrastSample.reset();
      m_stripDeferred = false;
      m_strip.clear();
      m_strip.shrink_to_fit();
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
    wlr_fbox base{};
    wlr_surface_get_buffer_source_box(m_content, &base);
    // Cannot sit behind the guard below: colour management can change on a commit with no buffer damage. The readback
    // returns the window's encoded values, so the flat colour is described as the window is.
    if (m_colors != nullptr) {
      m_colors->applySurfaceDescriptionToBuffer(m_content, m_adaptive);
    }
    // The window's geometry, not the buffer's: a client-drawn window's transparent shadow margins stay out of the
    // strip, and the crop happens upright so a quarter turn does not take its offset off the wrong buffer axis.
    const wlr_fbox strip = titleBarStripSourceBox(
        base, buffer->width, buffer->height, m_contentGeometry, m_content->current.width, m_content->current.height,
        static_cast<uint32_t>(m_content->current.transform), style.colorSourceHeight
    );
    // Damage is in buffer coordinates, like the strip. Round outward: the strip is fractional at fractional scales. It
    // belongs to the commit that produced it, so a later chrome repaint does not replay it.
    const int left = static_cast<int>(std::floor(strip.x));
    const int top = static_cast<int>(std::floor(strip.y));
    const int right = static_cast<int>(std::ceil(strip.x + strip.width));
    const int bottom = static_cast<int>(std::ceil(strip.y + strip.height));
    pixman_region32_t stripRegion;
    pixman_region32_init_rect(&stripRegion, left, top, std::max(0, right - left), std::max(0, bottom - top));
    // `pixman_region32_intersect` reports success, not emptiness: a disjoint strip and damage still returns true.
    bool touched = false;
    if (fromCommit && pixman_region32_intersect(&stripRegion, &stripRegion, &m_content->buffer_damage)) {
      touched = !pixman_region32_empty(&stripRegion);
    }
    pixman_region32_fini(&stripRegion);
    const float opacity = m_alpha * (m_focused ? 1.0F : kAdaptiveUnfocusedOpacity);
    const int barX = m_background->node.x;
    const int barY = m_background->node.y;
    // The fill is the bar's top edge, so it takes the same rounded corners as the palette it covers.
    const clipped_region clip = frameClip(barX, barY);
    const wlr_box clipBox = clip.inside ? clip.area : wlr_box{};
    const bool stripMoved = !fboxEqual(m_adaptiveStrip, strip);
    // A mode switch is not a commit, so it must force the work: otherwise an idle window keeps the stale colour.
    const bool modeChanged = m_adaptiveMode != style.colorSource;
    // The contrast sample has the same problem: a reload that turns it on has nothing to wait for.
    const bool sampling = style.titleContrast && style.titleContrastSample;
    const bool samplingChanged = m_contrastSampling != sampling;
    m_contrastSampling = sampling;
    // The smear mirrors the strip, so it re-mirrors on every attach: the bar must not keep drawing a buffer the client
    // may have freed. The average carries no detail, so only a real change to the edge makes it read again.
    wlr_buffer* shown = buffer;
    const wlr_fbox* sourceBox = &strip;
    wl_output_transform transform = m_content->current.transform;
    bool restrip = touched || modeChanged || stripMoved || m_stripDeferred || samplingChanged;
    if (average) {
      if (restrip) {
        const StripSample sample = averageStrip(strip);
        m_stripDeferred = sample.state == StripSample::State::Deferred;
        if (sample.color.has_value()) {
          m_averageColor = sample.color;
        } else if (sample.state == StripSample::State::Empty) {
          // A readable edge with no colour in it (transparent, or a strip with no area): the palette shows, as it does
          // for the mirrored mode. An unreadable buffer keeps the last colour instead, since its content is unknown.
          m_averageColor.reset();
        }
      }
      shown = m_averageColor.has_value() ? averageBufferFor(*m_averageColor) : nullptr;
      if (shown == nullptr) {
        hideAdaptive();
        m_adaptiveMode = style.colorSource;
        m_adaptiveStrip = strip;
        return;
      }
      sourceBox = nullptr;
      transform = WL_OUTPUT_TRANSFORM_NORMAL;
    } else if (sampling && restrip) {
      // "window" mirrors the edge instead of reading it, so it has no colour for the title to be judged against. The
      // sample is the same readback "average" pays, taken only when the edge changed, and only when asked for.
      const StripSample sample = averageStrip(strip);
      m_stripDeferred = sample.state == StripSample::State::Deferred;
      if (sample.color.has_value()) {
        m_contrastSample = sample.color;
      } else if (sample.state == StripSample::State::Empty) {
        // A readable edge with nothing in it leaves the palette showing, which is then what the title sits on.
        m_contrastSample =
            m_focused ? config().colors.titleBar.background : config().colors.titleBar.backgroundUnfocused;
      }
    }
    const bool reshaped = m_adaptiveBuffer != shown
        || stripMoved
        || m_adaptiveTransform != transform
        || m_adaptiveWidth != m_background->width
        || m_adaptiveHeight != m_background->height
        || m_adaptiveOpacity != opacity
        || m_adaptive->node.x != barX
        || m_adaptive->node.y != barY
        || !fx_corner_radii_eq(m_adaptive->corners, clip.corners)
        || !wlr_box_equal(&m_adaptive->corner_box, &clipBox);
    // A commit that leaves the top edge alone, and changes nothing about the bar, costs nothing.
    if (!restrip && !reshaped) {
      return;
    }
    if (m_adaptiveBuffer != shown || (touched && !average)) {
      // Mirrored like an overview card: committed buffer, its damage, and the explicit-sync point when there is one.
      wlr_scene_buffer_set_buffer_options options{
          .damage = fromCommit && !average ? &m_content->buffer_damage : nullptr,
          .wait_timeline = nullptr,
          .wait_point = 0,
      };
      if (!average) {
        if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(m_content)) {
          options.wait_timeline = sync->acquire_timeline;
          options.wait_point = sync->acquire_point;
        }
      }
      withdrawInPlaceDamage();
      wlr_scene_buffer_set_buffer_with_options(m_adaptive, shown, &options);
      // Without this wlroots rebuilds the whole texture per commit instead of reusing it for in-place shm damage.
      if (!average) {
        allowInPlaceDamage(shown);
      }
    }
    m_adaptiveBuffer = shown;
    m_adaptiveMode = style.colorSource;
    m_adaptiveStrip = strip;
    m_adaptiveTransform = transform;
    m_adaptiveWidth = m_background->width;
    m_adaptiveHeight = m_background->height;
    m_adaptiveOpacity = opacity;
    wlr_scene_buffer_set_source_box(m_adaptive, sourceBox);
    wlr_scene_buffer_set_transform(m_adaptive, transform);
    wlr_scene_buffer_set_dest_size(m_adaptive, m_background->width, m_background->height);
    wlr_scene_buffer_set_corner_radii(m_adaptive, clip.corners);
    wlr_scene_buffer_set_corner_box(m_adaptive, clip.inside ? &clip.area : nullptr);
    // Unfocus dims the window's own colour over the palette's unfocused tone rather than dropping back to it.
    wlr_scene_buffer_set_opacity(m_adaptive, opacity);
    wlr_scene_node_set_position(&m_adaptive->node, barX, barY);
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

  // The title for this bar, with the layout re-measured against the line's height (the text reaches into the corners).
  const TabLabel*
  SsdFrame::measureTitle(const std::array<float, 4>& color, float alpha, TitleBarLayout& parts, int radius) const {
    const auto& style = config().appearance.titleBar;
    TabLabelRequest request{
        .text = m_title,
        .color = color,
        .font = style.font,
        .maxWidth = parts.title.width,
        .scale = std::max(1.0, std::ceil(static_cast<double>(m_geometry.scale))),
    };
    request.color[3] *= alpha;
    const TabLabel* label = m_labels->label(request);
    if (label != nullptr) {
      // The line's measured height determines how far the arc reaches into its horizontal space.
      parts = titleBarLayout(
          m_geometry.contentWidth, m_geometry.titleBarHeight, style.buttonPosition, visibleButtons(),
          style.buttonGeometry(), radius, label->height
      );
      if (label->width > parts.title.width) {
        request.maxWidth = parts.title.width;
        label = m_labels->label(request);
      }
    }
    return label;
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
      const std::array<float, 4>& configured = enabled && (hovered || pressed) ? hoverGlyph : idleGlyph;
      // A symbol sits on its own button colour when backgrounds are painted, and on the bar when they are not, so those
      // are the two things it has to stay readable against.
      const auto glyphBackground = backgrounds ? std::optional(value) : barFillColor();
      const std::array<float, 4> glyphColor = style.titleContrast && glyphBackground.has_value()
          ? contrastAdjusted(configured, *glyphBackground)
          : configured;
      const bool glyphVisible = style.buttonGlyph == TitleBarButtonGlyph::Always
          || (style.buttonGlyph == TitleBarButtonGlyph::Hover && (hovered || pressed));
      updateButtonIcon(
          which, glyphColor, m_alpha * (enabled ? 1.0F : 0.35F), glyphVisible,
          style.titleShadow ? glyphOutline(glyphColor[3]) : std::nullopt
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
    updateAdaptive(false);
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
      updateAdaptive(false);
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
      updateAdaptive(false);
      updateBlur();
      return;
    }
    place(m_background, {0, -h, g.contentWidth, h});
    wlr_scene_rect_set_clipped_region(m_background, frameClip(m_background->node.x, m_background->node.y));
    // Before the title is measured, not after: the contrast rule reads the colour this leaves behind, and a fill that
    // arrives later would make the title one frame stale.
    updateAdaptive(false);
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
    // The configured colour, or black or white when the bar behind it is too close to it to read.
    const std::array<float, 4> textColor = resolvedTitleColor();
    const TabLabel* label = measureTitle(textColor, 1.0F, parts, radius);
    // Hold the title before the shadow lookup, which can evict it. The frame keeps this lock for its snapshots.
    holdLabelBuffer(m_labelBuffer, label != nullptr ? label->buffer : nullptr);
    const int labelWidth = label != nullptr ? label->width : 0;
    const int labelHeight = label != nullptr ? label->height : 0;
    // The bar can be any colour the application paints, so the title keeps a dark edge unless it is turned off.
    const bool shadowed = style.titleShadow;
    const TabLabel* shadow = nullptr;
    if (shadowed && m_labelBuffer != nullptr && textColor[3] > 0.0F) {
      // Re-measures the same layout without disturbing the title's copy.
      TitleBarLayout shadowParts = parts;
      shadow = measureTitle(kAdaptiveShadowColor, textColor[3], shadowParts, radius);
    }
    holdLabelBuffer(m_shadowBuffer, shadow != nullptr ? shadow->buffer : nullptr);
    if (m_labelBuffer == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, m_labelBuffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, m_adaptive != nullptr ? &m_adaptive->node : &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, m_labelBuffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(labelWidth)), std::max(1, zoomed(labelHeight)));
      wlr_scene_node_set_position(&m_label->node, zoomed(parts.title.x), zoomed(-h + (h - labelHeight) / 2));
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
      clipBuffer(m_labelShadow);
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
    const auto copyRect = [&](const wlr_scene_rect* source) {
      if (!source->node.enabled) {
        return;
      }
      auto* copy = wlr_scene_rect_create(tree, source->width, source->height, source->color);
      copy->accepts_input = false;
      wlr_scene_rect_set_corner_radii(copy, source->corners);
      wlr_scene_rect_set_clipped_region(copy, source->clipped_region);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    };
    // Palette, fill, buttons, text: the live order, or the fill covers the buttons for the whole animation.
    copyRect(m_background);
    // The scene drops its buffer field once the texture is uploaded, so fall back to a buffer this frame still locks.
    const auto copyBuffer = [&](const wlr_scene_buffer* source, wlr_buffer* fallback) {
      if (source == nullptr || !source->node.enabled) {
        return;
      }
      wlr_buffer* buffer = source->buffer != nullptr ? source->buffer : fallback;
      if (buffer == nullptr) {
        return;
      }
      auto* copy = wlr_scene_buffer_create(tree, buffer);
      if (copy == nullptr) {
        return;
      }
      copy->point_accepts_input = rejectInput;
      wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_buffer_set_corner_radii(copy, source->corners);
      wlr_scene_buffer_set_corner_box(copy, &source->corner_box);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    };
    // Above the palette rects and below the text, as in the live tree.
    if (m_adaptive != nullptr && m_adaptive->node.enabled && m_adaptive->buffer != nullptr) {
      auto* copy = wlr_scene_buffer_create(tree, m_adaptive->buffer);
      if (copy != nullptr) {
        copy->point_accepts_input = rejectInput;
        wlr_scene_buffer_set_source_box(copy, &m_adaptive->src_box);
        wlr_scene_buffer_set_transform(copy, m_adaptive->transform);
        wlr_scene_buffer_set_dest_size(copy, m_adaptive->dst_width, m_adaptive->dst_height);
        wlr_scene_buffer_set_corner_radii(copy, m_adaptive->corners);
        wlr_scene_buffer_set_corner_box(copy, &m_adaptive->corner_box);
        wlr_scene_buffer_set_opacity(copy, m_adaptive->opacity);
        wlr_scene_buffer_copy_color(copy, m_adaptive);
        wlr_scene_node_set_position(
            &copy->node, m_tree->node.x + m_adaptive->node.x, m_tree->node.y + m_adaptive->node.y
        );
      }
    }
    copyRect(m_close);
    copyRect(m_maximize);
    copyRect(m_minimize);
    // The live bar already holds both, so a snapshot needs no lookup: `fixed` copies exactly what it always did.
    copyBuffer(m_labelShadow, m_shadowBuffer);
    copyBuffer(m_label, m_labelBuffer);
    for (size_t index = 0; index < m_icons.size(); ++index) {
      // `m_iconBuffers` holds a lock for exactly this copy.
      copyBuffer(m_icons[index], m_iconBuffers[index]);
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
      // The card shows the same bar and takes the colour its window already resolved, not a per-card readback.
      frame->setContentSurface(m_content, m_contentGeometry);
      frame->m_averageColor = m_averageColor;
      // The sample the window paid for, reused rather than taken again per card.
      frame->m_contrastSample = m_contrastSample;
      frame->m_stripDeferred = m_stripDeferred;
      frame->reloadConfig();
    }
  }
} // namespace umbriel
