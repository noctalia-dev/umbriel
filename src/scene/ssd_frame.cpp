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
#include <chrono>
#include <cmath>
#include <drm_fourcc.h>
#include <utility>
#include <vector>

namespace umbriel {
  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }

    // An unfocused window keeps its own bar color, dimmed over the palette's unfocused tone.
    constexpr float kAdaptiveUnfocusedOpacity = 0.65F;
    // Slack past the measured title, so a character or two more of the edge is read without the whole bar.
    constexpr int kAdaptiveSamplePadding = 8;

    // Wall clock, not the animation clock: freezing animation time must not stall the readback cadence.
    std::uint64_t monotonicMsec() {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count()
      );
    }

    // Any bar can be any color the window paints, so text and symbols keep a dark edge, scaled by their own alpha so an
    // invisible one leaves no ghost.
    constexpr std::array<float, 4> kTitleShadowColor{0.0F, 0.0F, 0.0F, 0.55F};
    std::optional<std::array<float, 4>> glyphOutline(float glyphAlpha) {
      if (glyphAlpha <= 0.0F) {
        return std::nullopt;
      }
      std::array<float, 4> outline = kTitleShadowColor;
      outline[3] *= glyphAlpha;
      return outline;
    }
  } // namespace

  SsdFrame::SsdFrame(
      wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent, ColorManager* colors,
      wl_event_loop* loop
  )
      : m_backgroundTree(wlr_scene_tree_create(parent)), m_tree(wlr_scene_tree_create(parent)),
        m_gripTree(wlr_scene_tree_create(inputParent != nullptr ? inputParent : parent)), m_loop(loop),
        m_colors(colors), m_labels(std::move(labels)) {
    if (inputParent != nullptr && parent->node.parent == inputParent) {
      // Client surfaces and popups keep input priority over invisible resize grips.
      wlr_scene_node_place_below(&m_gripTree->node, &parent->node);
    }
    // The fill overlaps opaque borders at their antialiased inner edge.
    wlr_scene_node_lower_to_bottom(&m_backgroundTree->node);
    const float clear[4]{};
    m_background = wlr_scene_rect_create(m_backgroundTree, 1, 1, clear);
    m_close = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_maximize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_minimize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    for (auto*& grip : m_grips) {
      grip = wlr_scene_rect_create(m_gripTree, 1, 1, clear);
    }
    wlr_scene_node_set_enabled(&m_tree->node, false);
    wlr_scene_node_set_enabled(&m_backgroundTree->node, false);
    wlr_scene_node_set_enabled(&m_gripTree->node, false);
  }

  SsdFrame::~SsdFrame() {
    if (m_stripTimer != nullptr) {
      wl_event_source_remove(m_stripTimer);
    }
    withdrawInPlaceDamage();
    if (m_blur != nullptr) {
      wlr_scene_node_destroy(&m_blur->node);
    }
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
    wlr_scene_node_destroy(&m_backgroundTree->node);
    if (m_averageBuffer != nullptr) {
      wlr_buffer_unlock(m_averageBuffer);
      m_averageBuffer = nullptr;
    }
    if (m_labelBuffer != nullptr) {
      wlr_buffer_unlock(m_labelBuffer);
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
  void SsdFrame::setOpaqueBorderInset(int inset) {
    if (m_opaqueBorderInset == inset) {
      return;
    }
    m_opaqueBorderInset = inset;
    wlr_scene_rect_set_clipped_region(
        m_background, frameClip(m_background->node.x, m_background->node.y, m_opaqueBorderInset)
    );
    updateBlur();
  }
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
  void SsdFrame::setColorSource(std::optional<TitleBarColorSource> source) {
    if (m_colorSourceOverride == source) {
      return;
    }
    m_colorSourceOverride = source;
    // A mode switch is not a commit, so it has to force the work: an idle window would keep the old fill.
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
    // Another surface is another edge color to judge against, and only redraw() re-measures the title.
    refreshFill(false);
  }
  void SsdFrame::syncContent(const wlr_box& geometry) {
    m_contentGeometry = geometry;
    ++m_commitGeneration;
    // A palette bar reads nothing back and judges nothing against the edge, so a client commit cannot move what it
    // shows: the fill is the palette and the contrast rule already knows it. Skip the per-commit bookkeeping entirely.
    if (colorSource() == TitleBarColorSource::Fixed) {
      return;
    }
    refreshFill(true);
  }

  void SsdFrame::allowInPlaceDamage(wlr_buffer* buffer) {
    m_inPlaceDamage = umbrielfx_scene_buffer_ignore_client_lock(buffer) != nullptr;
  }

  void SsdFrame::withdrawInPlaceDamage() {
    if (!m_inPlaceDamage) {
      return;
    }
    m_inPlaceDamage = false;
    // Ask the scene buffer which buffer it holds: a remembered client buffer would dangle once the client frees it.
    if (m_adaptive != nullptr && m_adaptive->buffer != nullptr) {
      umbrielfx_scene_buffer_drop_ignored_client_lock(wlr_client_buffer_get(m_adaptive->buffer));
    }
  }

  bool SsdFrame::stripFenceSignalled() const {
    if (m_content == nullptr) {
      return true;
    }
    wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(m_content);
    if (sync == nullptr || sync->acquire_timeline == nullptr) {
      return true;
    }
    bool signalled = false;
    // A failed check must not withhold the bar: read the edge rather than keep a stale color.
    if (!wlr_drm_syncobj_timeline_check(sync->acquire_timeline, sync->acquire_point, 0, &signalled)) {
      return true;
    }
    return signalled;
  }

  bool SsdFrame::stripTouched(const wlr_fbox& box) const {
    if (m_content == nullptr) {
      return false;
    }
    // Round outward: the strip is fractional at fractional scales.
    const int left = static_cast<int>(std::floor(box.x));
    const int top = static_cast<int>(std::floor(box.y));
    const int right = static_cast<int>(std::ceil(box.x + box.width));
    const int bottom = static_cast<int>(std::ceil(box.y + box.height));
    pixman_region32_t region;
    pixman_region32_init_rect(&region, left, top, std::max(0, right - left), std::max(0, bottom - top));
    // `pixman_region32_intersect` reports success, not emptiness: a disjoint strip and damage still returns true.
    bool touched = false;
    if (pixman_region32_intersect(&region, &region, &m_content->buffer_damage)) {
      touched = !pixman_region32_empty(&region);
    }
    pixman_region32_fini(&region);
    return touched;
  }

  double SsdFrame::sampleLeftFraction(const SampleSpan& span) const {
    if (span.empty() || m_geometry.contentWidth <= 0) {
      return 0.0;
    }
    return std::clamp(static_cast<double>(span.left) / static_cast<double>(m_geometry.contentWidth), 0.0, 1.0);
  }

  double SsdFrame::sampleWidthFraction(const SampleSpan& span) const {
    if (span.empty() || m_geometry.contentWidth <= 0) {
      return 1.0;
    }
    const double width = static_cast<double>(span.width) / static_cast<double>(m_geometry.contentWidth);
    return std::clamp(width, 0.0, 1.0 - sampleLeftFraction(span));
  }

  void SsdFrame::cacheSampleSpans(const TitleBarLayout& layout, int labelX, int labelWidth) {
    std::array<SampleSpan, 1 + kTitleBarButtonCount> spans{};
    if (m_geometry.contentWidth > 0) {
      // Wherever the title sits, its own span plus a little slack: a centered or right-aligned title is not under the
      // leading edge, so a readback from there would judge the bar against the wrong part of the window. The label and
      // the strip are both logical pixels.
      if (labelWidth > 0) {
        spans[0] = titleBarSampleSpan(
            labelX - kAdaptiveSamplePadding, labelWidth + 2 * kAdaptiveSamplePadding, m_geometry.contentWidth
        );
      }
      // A bare symbol is judged against the edge under its own button, which a mirrored bar can colour differently
      // from the title's span. A button that paints its own colour never reads that edge, so it adds no span. The
      // button boxes are in the bar's coordinates, like the label.
      const auto& colors = config().colors.titleBar;
      const bool backgrounds = config().appearance.titleBar.buttonBackground;
      const auto symbolOnBar = [&](TitleBarButton which) {
        if (!backgrounds) {
          return true;
        }
        switch (which) {
        case TitleBarButton::Close:
          return colors.close[3] <= 0.0F;
        case TitleBarButton::Maximize:
          return colors.maximize[3] <= 0.0F;
        case TitleBarButton::Minimize:
          return colors.minimize[3] <= 0.0F;
        }
        return true;
      };
      for (size_t index = 0; index < kTitleBarButtonCount; ++index) {
        const auto which = static_cast<TitleBarButton>(index);
        const std::optional<wlr_box>& button = layout.buttons[index];
        if (!button.has_value() || !symbolOnBar(which)) {
          continue;
        }
        spans[1 + index] = titleBarSampleSpan(
            button->x - kAdaptiveSamplePadding, button->width + 2 * kAdaptiveSamplePadding, m_geometry.contentWidth
        );
      }
    }
    m_sampleSpans = spans;
  }

  void SsdFrame::armStripFallback(std::uint32_t delayMsec) {
    if (m_loop == nullptr || m_preview) {
      return;
    }
    if (m_stripTimer == nullptr) {
      m_stripTimer = wl_event_loop_add_timer(m_loop, onStripFallbackTimer, this);
      if (m_stripTimer == nullptr) {
        return;
      }
    }
    wl_event_source_timer_update(m_stripTimer, static_cast<int>(delayMsec));
  }

  int SsdFrame::onStripFallbackTimer(void* data) {
    auto* self = static_cast<SsdFrame*>(data);
    if (!self->m_stripDeferred) {
      return 0;
    }
    // No further commit is coming, so the wait or the skip ends here and the edge is read.
    self->refreshFill(false);
    return 0;
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

  std::array<StripSample, 1 + kTitleBarButtonCount>
  SsdFrame::readSampleRegions(const std::array<wlr_fbox, 1 + kTitleBarButtonCount>& boxes) {
    // The only readback in this class, and the only cost average mode cannot avoid: wlroots finishes the frame first.
    // One read covers every region, so a symbol judged against the edge under its own button costs no second frame. A
    // preview shows what its window resolved, so reading back per card would only multiply that cost.
    std::array<StripSample, 1 + kTitleBarButtonCount> results{};
    if (m_preview || m_content == nullptr || m_content->buffer == nullptr || m_content->buffer->texture == nullptr) {
      return results;
    }
    double unionLeft = 0.0;
    double unionTop = 0.0;
    double unionRight = 0.0;
    double unionBottom = 0.0;
    bool unionSet = false;
    for (const wlr_fbox& box : boxes) {
      if (box.width <= 0.0 || box.height <= 0.0) {
        continue;
      }
      if (!unionSet) {
        unionLeft = box.x;
        unionTop = box.y;
        unionRight = box.x + box.width;
        unionBottom = box.y + box.height;
        unionSet = true;
        continue;
      }
      unionLeft = std::min(unionLeft, box.x);
      unionTop = std::min(unionTop, box.y);
      unionRight = std::max(unionRight, box.x + box.width);
      unionBottom = std::max(unionBottom, box.y + box.height);
    }
    if (!unionSet) {
      return results;
    }
    // Round outward: the strip is fractional at fractional scales.
    const int left = static_cast<int>(std::floor(unionLeft));
    const int top = static_cast<int>(std::floor(unionTop));
    const int right = static_cast<int>(std::ceil(unionRight));
    const int bottom = static_cast<int>(std::ceil(unionBottom));
    const wlr_box readBox{left, top, std::max(0, right - left), std::max(0, bottom - top)};
    if (readBox.width <= 0 || readBox.height <= 0) {
      return results;
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
      return results;
    }
    m_stripPixels.resize(static_cast<size_t>(readBox.width) * static_cast<size_t>(readBox.height));
    const wlr_texture_read_pixels_options options{
        .data = m_stripPixels.data(),
        .format = format,
        .stride = static_cast<uint32_t>(readBox.width) * 4U,
        .dst_x = 0,
        .dst_y = 0,
        .src_box = readBox,
    };
    // Refused for a protected or external-only buffer; the last color, or the palette, stays.
    if (!wlr_texture_read_pixels(m_content->buffer->texture, &options)) {
      return results;
    }
    for (size_t index = 0; index < boxes.size(); ++index) {
      const wlr_fbox& box = boxes[index];
      if (box.width <= 0.0 || box.height <= 0.0) {
        continue;
      }
      const int regionLeft = std::max(readBox.x, static_cast<int>(std::floor(box.x)));
      const int regionTop = std::max(readBox.y, static_cast<int>(std::floor(box.y)));
      const int regionRight = std::min(readBox.x + readBox.width, static_cast<int>(std::ceil(box.x + box.width)));
      const int regionBottom = std::min(readBox.y + readBox.height, static_cast<int>(std::ceil(box.y + box.height)));
      if (regionRight <= regionLeft || regionBottom <= regionTop) {
        continue;
      }
      const size_t offset = static_cast<size_t>(regionTop - readBox.y) * static_cast<size_t>(readBox.width)
          + static_cast<size_t>(regionLeft - readBox.x);
      const auto mean = averagePixelsRect(
          m_stripPixels.data() + offset, static_cast<size_t>(readBox.width),
          static_cast<size_t>(regionRight - regionLeft), static_cast<size_t>(regionBottom - regionTop), *order
      );
      results[index] = mean.has_value() ? StripSample{StripSample::State::Fresh, mean}
                                        : StripSample{StripSample::State::Empty, std::nullopt};
    }
    logDebug("[title_bar] averaged a {}x{} strip read back from the window", readBox.width, readBox.height);
    return results;
  }

  TitleBarColorSource SsdFrame::colorSource() const {
    return m_colorSourceOverride.value_or(config().appearance.titleBar.colorSource);
  }

  std::optional<std::array<float, 4>> SsdFrame::barFillColor() const {
    const auto& colors = config().colors.titleBar;
    // The palette shows when the adaptive fill has nothing to draw, so it is the fallback: a transparent edge, an
    // unreadable buffer and `"fixed"` all land on it.
    const std::array<float, 4>& palette = m_focused ? colors.background : colors.backgroundUnfocused;
    switch (colorSource()) {
    case TitleBarColorSource::Average:
      return m_averageColor.value_or(palette);
    case TitleBarColorSource::Window:
      // Mirrored pixels, not a color: with no sample there is nothing to judge the title against, and the palette is
      // not what is on screen. An edge read as empty is the exception, since the palette is what shows through it.
      if (m_contrastSample.has_value()) {
        return m_contrastSample;
      }
      return m_contrastSampleEmpty ? std::optional<std::array<float, 4>>(palette) : std::nullopt;
    case TitleBarColorSource::Fixed:
      break;
    }
    return palette;
  }

  std::array<float, 4> SsdFrame::fillAsShown(const std::array<float, 4>& fill) const {
    const auto& colors = config().colors.titleBar;
    const std::array<float, 4>& palette = m_focused ? colors.background : colors.backgroundUnfocused;
    // Only an adaptive fill is drawn dimmed over the palette when the window is inactive; a fixed bar is the palette
    // itself, drawn over itself, and the palette is what shows through a fill with nothing in it.
    const bool dimmed = colorSource() != TitleBarColorSource::Fixed && !m_focused;
    return asShownBarColor(fill, dimmed ? kAdaptiveUnfocusedOpacity : 1.0F, palette);
  }

  std::optional<std::array<float, 4>> SsdFrame::displayedBarColor() const {
    const auto fill = barFillColor();
    return fill.has_value() ? std::optional(fillAsShown(*fill)) : std::nullopt;
  }

  std::optional<std::array<float, 4>> SsdFrame::glyphBackground(TitleBarButton which) const {
    const auto index = static_cast<size_t>(which);
    if (m_glyphSamples[index].has_value()) {
      return fillAsShown(*m_glyphSamples[index]);
    }
    if (m_glyphSampleEmpty[index]) {
      // A transparent edge shows the palette through the mirrored bar, which is what the symbol sits on.
      const auto& colors = config().colors.titleBar;
      return m_focused ? colors.background : colors.backgroundUnfocused;
    }
    return displayedBarColor();
  }

  std::array<float, 4> SsdFrame::resolvedTitleColor() const {
    const auto& style = config().appearance.titleBar;
    const auto& colors = config().colors.titleBar;
    const std::array<float, 4>& configured = m_focused ? colors.text : colors.textUnfocused;
    // Judged against the bar as shown — the fill dimmed over the unfocused palette when the window is inactive — so
    // the title cannot sink into the dimming the verdict never saw.
    const auto background = displayedBarColor();
    return style.titleContrast && background.has_value() ? contrastAdjusted(configured, *background) : configured;
  }

  wlr_buffer* SsdFrame::averageBufferFor(const std::array<float, 4>& color) {
    // Compare against the baked color: `m_averageColor` already holds the new mean, so it would always match.
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

  bool SsdFrame::refreshFill(bool fromCommit) {
    const std::array<float, 4> title = resolvedTitleColor();
    const auto fill = barFillColor();
    const auto glyphs = m_glyphSamples;
    const auto glyphEmpty = m_glyphSampleEmpty;
    updateAdaptive(fromCommit);
    // Only the contrast rule turns a resolved color into a repaint; the mirror follows the commit on its own. A symbol
    // on its own button color never sees a sample, but a bare one does, so a sample under any button can move it.
    const auto& style = config().appearance.titleBar;
    if (!style.titleContrast) {
      return false;
    }
    if (resolvedTitleColor() == title
        && barFillColor() == fill
        && m_glyphSamples == glyphs
        && m_glyphSampleEmpty == glyphEmpty) {
      return false;
    }
    redraw();
    return true;
  }

  void SsdFrame::updateAdaptive(bool fromCommit) {
    // The palette stays painted underneath, so a transparent edge or a missing buffer leaves no hole.
    const auto& style = config().appearance.titleBar;
    const TitleBarColorSource source = colorSource();
    const bool average = source == TitleBarColorSource::Average;
    wlr_buffer* buffer = source != TitleBarColorSource::Fixed
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
      // A preview can never read a color back, so it keeps the one its window resolved until it is laid out.
      if (!m_preview) {
        m_averageColor.reset();
        m_contrastSample.reset();
        m_contrastSampleEmpty = false;
        m_glyphSamples = {};
        m_glyphSampleEmpty = {};
        m_stripDeferred = false;
        m_sampling.reset();
      }
      m_sampleBoxes = {};
      m_stripPixels.clear();
      m_stripPixels.shrink_to_fit();
      return;
    }
    if (m_adaptive == nullptr) {
      m_adaptive = wlr_scene_buffer_create(m_backgroundTree, nullptr);
      if (m_adaptive == nullptr) {
        return;
      }
      m_adaptive->point_accepts_input = rejectInput;
      wlr_scene_node_place_above(&m_adaptive->node, &m_background->node);
    }
    wlr_fbox base{};
    wlr_surface_get_buffer_source_box(m_content, &base);
    // Cannot sit behind the guard below: color management can change on a commit with no buffer damage. The readback
    // returns the window's encoded values, so the flat color is described as the window is.
    if (m_colors != nullptr) {
      m_colors->applySurfaceDescriptionToBuffer(m_content, m_adaptive);
    }
    // The window's geometry, not the buffer's: a client-drawn window's transparent shadow margins stay out of the
    // strip, and the crop happens upright so a quarter turn does not take its offset off the wrong buffer axis.
    const wlr_fbox strip = titleBarStripSourceBox(
        base, buffer->width, buffer->height, m_contentGeometry, m_content->current.width, m_content->current.height,
        static_cast<uint32_t>(m_content->current.transform), style.colorSourceHeight
    );
    // Damage is in buffer coordinates, like the strip, and belongs to the commit that produced it, so a later chrome
    // repaint does not replay it.
    const bool touched = fromCommit && stripTouched(strip);
    const float opacity = m_alpha * (m_focused ? 1.0F : kAdaptiveUnfocusedOpacity);
    const int barX = m_background->node.x;
    const int barY = m_background->node.y;
    // The fill is the bar's top edge, so it takes the same rounded corners as the palette it covers.
    const clipped_region clip = frameClip(barX, barY, m_opaqueBorderInset);
    const wlr_box clipBox = clip.inside ? clip.area : wlr_box{};
    const bool stripMoved = !wlr_fbox_equal(&m_adaptiveStrip, &strip);
    // A mode switch is not a commit, so it must force the work: otherwise an idle window keeps the stale color.
    const bool modeChanged = m_adaptiveMode != source;
    // Only "window" mirrors without reading, so only there does a sample add anything; the key has no effect elsewhere.
    const bool sampling = source == TitleBarColorSource::Window && style.titleContrast && style.titleContrastSample;
    const bool samplingChanged = m_contrastSampling != sampling;
    m_contrastSampling = sampling;
    if (!sampling) {
      // A kept sample would keep reversing the title or a symbol against an edge nothing reads any more.
      m_contrastSample.reset();
      m_contrastSampleEmpty = false;
      m_glyphSamples = {};
      m_glyphSampleEmpty = {};
      if (!average) {
        m_stripDeferred = false;
      }
    }
    if (!average) {
      // The mean belongs to the mode that read it, and a read that is refused on the way back must not reuse it.
      m_averageColor.reset();
    }
    // Both modes pay the same readback: "average" paints it, "window" judges the title against it. Only the mirror is
    // free, so the read covers the title and a little slack instead of the whole edge. A mirror still needs all of it.
    // Bare symbols add one span each, so a symbol is judged against the edge under its own button; all of it comes from
    // a single read of the union.
    std::array<wlr_fbox, 1 + kTitleBarButtonCount> sampleBoxes{};
    if (average || sampling) {
      const auto spanBox = [&](const SampleSpan& span) {
        const double start = sampleLeftFraction(span);
        const double fraction = sampleWidthFraction(span);
        if (start <= 0.0 && fraction >= 1.0) {
          return strip;
        }
        return titleBarStripSourceBox(
            base, buffer->width, buffer->height, m_contentGeometry, m_content->current.width, m_content->current.height,
            static_cast<uint32_t>(m_content->current.transform), style.colorSourceHeight, fraction, start
        );
      };
      sampleBoxes[0] = spanBox(m_sampleSpans[0]);
      if (sampling) {
        for (size_t index = 0; index < kTitleBarButtonCount; ++index) {
          if (!m_sampleSpans[1 + index].empty()) {
            sampleBoxes[1 + index] = spanBox(m_sampleSpans[1 + index]);
          }
        }
      }
    }
    // A title or a button row that moved takes its sample with it: where the bar reads from is part of what it covers.
    bool spanMoved = false;
    if (average || sampling) {
      for (size_t index = 0; index < sampleBoxes.size(); ++index) {
        if (!wlr_fbox_equal(&m_sampleBoxes[index], &sampleBoxes[index])) {
          spanMoved = true;
          break;
        }
      }
    }
    const bool restrip = (average ? (fromCommit && stripTouched(sampleBoxes[0])) : touched)
        || modeChanged
        || stripMoved
        || spanMoved
        || m_stripDeferred
        || samplingChanged;
    if (fromCommit && !restrip) {
      m_sampling.noteQuietCommit();
    }
    if ((average || sampling) && restrip && !m_preview) {
      const AdaptiveSampling::Input input{
          .fromCommit = fromCommit,
          .fenceSignalled = stripFenceSignalled(),
          .generation = m_commitGeneration,
          .nowMsec = monotonicMsec(),
      };
      const AdaptiveSampling::Action action = m_sampling.decide(input);
      m_sampling.record(action, input);
      switch (action) {
      case AdaptiveSampling::Action::Skip:
        // Owed, not dropped: the timer reads the last frame of a burst that ends on a skipped commit.
        m_stripDeferred = true;
        armStripFallback(AdaptiveSampling::kSampleMinIntervalMsec);
        break;
      case AdaptiveSampling::Action::Wait:
        m_stripDeferred = true;
        armStripFallback(AdaptiveSampling::kFenceGiveUpMsec);
        break;
      case AdaptiveSampling::Action::GiveUp:
        // This commit's fence was abandoned; a later commit brings a new one.
        m_stripDeferred = false;
        break;
      case AdaptiveSampling::Action::Read:
      case AdaptiveSampling::Action::ReadAnyway: {
        const auto samples = readSampleRegions(sampleBoxes);
        m_stripDeferred = false;
        m_sampleBoxes = sampleBoxes;
        if (average) {
          const StripSample& sample = samples[0];
          if (sample.color.has_value()) {
            m_averageColor = sample.color;
          } else if (sample.state == StripSample::State::Empty) {
            // A readable edge with no color in it (transparent, or a strip with no area): the palette shows, as it does
            // for the mirrored mode. An unreadable buffer keeps the last color instead, since its content is unknown.
            m_averageColor.reset();
          }
        }
        if (sampling) {
          resolveStripSample(samples[0], m_contrastSample, m_contrastSampleEmpty);
          for (size_t index = 0; index < kTitleBarButtonCount; ++index) {
            resolveStripSample(samples[1 + index], m_glyphSamples[index], m_glyphSampleEmpty[index]);
          }
        }
        break;
      }
      }
    }
    // The smear mirrors the strip, so it re-mirrors on every attach: the bar must not keep drawing a buffer the client
    // may have freed. The average carries no detail, so only a change inside the sampled region makes it read again.
    wlr_buffer* shown = buffer;
    const wlr_fbox* sourceBox = &strip;
    wl_output_transform transform = m_content->current.transform;
    if (average) {
      shown = m_averageColor.has_value() ? averageBufferFor(*m_averageColor) : nullptr;
      if (shown == nullptr) {
        hideAdaptive();
        m_adaptiveMode = source;
        m_adaptiveStrip = strip;
        return;
      }
      sourceBox = nullptr;
      transform = WL_OUTPUT_TRANSFORM_NORMAL;
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
    m_adaptiveMode = source;
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
    // Unfocus dims the window's own color over the palette's unfocused tone rather than dropping back to it.
    wlr_scene_buffer_set_opacity(m_adaptive, opacity);
    wlr_scene_node_set_position(&m_adaptive->node, barX, barY);
    wlr_scene_node_set_enabled(&m_adaptive->node, true);
  }

  int SsdFrame::zoomed(int value) const { return static_cast<int>(std::lround(value * m_geometry.zoom)); }

  clipped_region SsdFrame::frameClip(int x, int y, int outset) const {
    outset = std::clamp(outset, 0, m_geometry.borderInset);
    const int radius = zoomed(nestedRadius(m_geometry.cornerRadius, m_geometry.borderInset - outset));
    if (radius <= 0) {
      return {};
    }
    return {
        .area =
            {zoomed(-outset) - x, zoomed(-m_geometry.titleBarHeight - outset) - y,
             zoomed(m_geometry.contentWidth + 2 * outset),
             zoomed(m_geometry.contentHeight + m_geometry.titleBarHeight + 2 * outset)},
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
  const TabLabel* SsdFrame::measureTitle(
      const std::array<float, 4>& color, float alpha, TitleBarLayout& parts, int radius, bool shadowed
  ) const {
    const auto& style = config().appearance.titleBar;
    TabLabelRequest request{
        .text = m_title,
        .color = color,
        .font = style.font,
        .maxWidth = parts.title.width,
        .scale = std::max(1.0, std::ceil(static_cast<double>(m_geometry.scale))),
    };
    request.color[3] *= alpha;
    if (shadowed) {
      // The edge fades with the title it sits under, so a translucent one leaves no ghost behind it.
      request.shadow = kTitleShadowColor;
      request.shadow[3] *= request.color[3];
    }
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
    const auto& style = config().appearance.titleBar;
    const int size = titleBarButtonDiameter(m_geometry.titleBarHeight, style.buttonSize);
    const TitleBarGlyph glyph = titleBarGlyph(size, style.buttonGlyphSize, style.buttonGlyphStroke);
    const float scale = std::max(1.0F, m_geometry.scale);
    // At integer scales the symbol maps 1:1 onto the button. At fractional scales the button's physical size depends
    // on its position, and the renderer copies a buffer within one pixel of that size texel for texel, leaving the
    // symbol half a pixel off center. Twice the resolution makes it resample symmetrically instead.
    const int pixels = static_cast<int>(std::ceil(size * scale)) * (scale == std::floor(scale) ? 1 : 2);
    if (icon == nullptr
        || m_iconSizes[index] != pixels
        || m_iconColors[index] != color
        || m_iconOutlines[index] != outline
        || m_iconGlyphs[index] != glyph) {
      wlr_buffer* buffer = renderTitleBarIcon(button, pixels, color, glyph, outline);
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
      m_iconGlyphs[index] = glyph;
    }
    wlr_scene_buffer_set_dest_size(icon, background->width, background->height);
    wlr_scene_buffer_set_opacity(icon, alpha);
    wlr_scene_node_set_position(&icon->node, background->node.x, background->node.y);
    clipBuffer(icon);
    wlr_scene_node_set_enabled(&icon->node, true);
  }

  void SsdFrame::updateColors() {
    // A fill that resolves here (a deferred readback, or a strip that moved) changes what the title and the symbols are
    // judged against. The repaint it starts settles, because the fill it held when it began is the one it keeps.
    if (refreshFill(false)) {
      return;
    }
    const auto& colors = config().colors.titleBar;
    const auto& style = config().appearance.titleBar;
    const bool backgrounds = style.buttonBackground;
    float color[4];
    const auto& background = m_focused ? colors.background : colors.backgroundUnfocused;
    premultiplied(color, background, m_alpha);
    wlr_scene_rect_set_color(m_background, color);
    const float contrast = luminance(background) < 0.5F ? 1.0F : 0.0F;
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
      // A symbol sits on its own button color when backgrounds are painted, and on the bar when they are not or when
      // that color is fully transparent. The button is painted at its state opacity, so a disabled or idle one shows
      // the bar through itself and the symbol is judged against that blend; on a mirrored bar that spot is sampled
      // separately, so a symbol is judged against the edge under its own button rather than the one under the title.
      std::optional<std::array<float, 4>> symbolBackground;
      if (backgrounds && value[3] > 0.0F) {
        const auto bar = displayedBarColor();
        symbolBackground = bar.has_value() && (alpha < 1.0F || value[3] < 1.0F)
            ? std::optional(asShownBarColor(value, alpha, *bar))
            : std::optional(value);
      } else {
        symbolBackground = glyphBackground(which);
      }
      const std::array<float, 4> glyphColor = style.titleContrast && symbolBackground.has_value()
          ? contrastAdjusted(configured, *symbolBackground)
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
    updateBlur();
  }

  void SsdFrame::redraw() {
    const auto& g = m_geometry;
    const int h = g.titleBarHeight;
    const bool available = !g.suppressed && g.contentWidth > 0 && g.contentHeight > 0;
    const bool shown = available && h > 0;
    wlr_scene_node_set_enabled(&m_tree->node, shown);
    wlr_scene_node_set_enabled(&m_backgroundTree->node, shown);
    wlr_scene_node_set_enabled(&m_gripTree->node, available && m_resizable && !m_preview);
    if (!available) {
      // No bar to sample for: let a later redraw measure the title again before it caps a readback.
      m_sampleSpans = {};
      updateAdaptive(false);
      updateBlur();
      return;
    }
    wlr_scene_node_set_position(&m_tree->node, zoomed(g.contentX), zoomed(g.contentY));
    wlr_scene_node_set_position(&m_backgroundTree->node, zoomed(g.contentX), zoomed(g.contentY));
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
      m_sampleSpans = {};
      updateAdaptive(false);
      updateBlur();
      return;
    }
    place(m_background, {0, -h, g.contentWidth, h});
    wlr_scene_rect_set_clipped_region(
        m_background, frameClip(m_background->node.x, m_background->node.y, m_opaqueBorderInset)
    );
    // Before the title is measured, not after: the contrast rule reads the color this leaves behind, and a fill that
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
    // The configured color, or black or white when the bar behind it is too close to read.
    const std::array<float, 4> textColor = resolvedTitleColor();
    // Any bar can be any color, so the title carries a dark edge unless it is turned off. It is painted into the same
    // buffer, one pixel down and right, so the bar holds one node and the shared cache one entry per title.
    const bool shadowed = style.titleShadow && textColor[3] > 0.0F;
    const TabLabel* label = measureTitle(textColor, 1.0F, parts, radius, shadowed);
    // Held before anything else can ask the cache for a label and evict this one. The frame keeps the lock for
    // snapshots.
    holdLabelBuffer(m_labelBuffer, label != nullptr ? label->buffer : nullptr);
    const int labelWidth = label != nullptr ? label->width : 0;
    const int labelHeight = label != nullptr ? label->height : 0;
    const int labelX = titleBarLabelX(parts, g.contentWidth, labelWidth, style.titleAlign);
    // Only records the spans: `refreshFill` below reads them, sees what moved, and repaints this redraw when the
    // title's or a symbol's contrast decision changes.
    cacheSampleSpans(parts, labelX, labelWidth);
    if (m_labelBuffer == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, m_labelBuffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_lower_to_bottom(&m_label->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, m_labelBuffer);
      }
      // Sized to the buffer, which the edge extends past the text, so the edge is neither scaled away nor clipped here;
      // the text is still centred on its own box.
      wlr_scene_buffer_set_dest_size(
          m_label, std::max(1, zoomed(label->bufferWidth)), std::max(1, zoomed(label->bufferHeight))
      );
      wlr_scene_node_set_position(&m_label->node, zoomed(labelX), zoomed(-h + (h - labelHeight) / 2));
      clipBuffer(m_label);
    }
    updateColors();
  }

  bool SsdFrame::contains(const wlr_scene_node* node) const {
    for (const wlr_scene_node* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == &m_tree->node || current == &m_backgroundTree->node || current == &m_gripTree->node) {
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
    auto* backgroundTree = wlr_scene_tree_create(parent);
    wlr_scene_node_lower_to_bottom(&backgroundTree->node);
    if (m_blur != nullptr && m_blur->node.enabled) {
      auto* copy = wlr_scene_blur_create(backgroundTree, m_blur->width, m_blur->height);
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
      auto* copy = wlr_scene_rect_create(
          source == m_background ? backgroundTree : tree, source->width, source->height, source->color
      );
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
      auto* copy = wlr_scene_buffer_create(backgroundTree, m_adaptive->buffer);
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
      frame->setOpaqueBorderInset(m_opaqueBorderInset);
      frame->setBlurOptions(m_blurOptions);
      // The card takes the colors its window already resolved rather than reading them again per card, and they are in
      // place before anything that draws: an un-laid-out card would otherwise drop them.
      frame->m_averageColor = m_averageColor;
      frame->m_contrastSample = m_contrastSample;
      frame->m_contrastSampleEmpty = m_contrastSampleEmpty;
      frame->m_glyphSamples = m_glyphSamples;
      frame->m_glyphSampleEmpty = m_glyphSampleEmpty;
      frame->m_stripDeferred = m_stripDeferred;
      frame->setColorSource(m_colorSourceOverride);
      frame->setContentSurface(m_content, m_contentGeometry);
      frame->reloadConfig();
    }
  }
} // namespace umbriel
