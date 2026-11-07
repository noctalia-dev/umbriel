#include "scene/ssd_frame.h"

#include "config/config.h"
#include "scene/cairo_buffer.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
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

    // An adaptive bar carries any colour the window paints, so text and symbols keep a dark edge under them. The edge
    // takes its alpha from the glyph it backs, so a transparent or faded symbol does not leave a ghost of itself.
    const std::array<float, 4> kAdaptiveShadowColor{0.0F, 0.0F, 0.0F, 0.55F};
    std::optional<std::array<float, 4>> adaptiveOutline(float glyphAlpha) {
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
    releaseIgnoredLock();
    if (m_blur != nullptr) {
      wlr_scene_node_destroy(&m_blur->node);
    }
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
    if (m_averageBuffer != nullptr) {
      wlr_buffer_drop(m_averageBuffer);
      m_averageBuffer = nullptr;
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
    updateAdaptive(true);
  }

  void SsdFrame::ignoreLock(wlr_buffer* buffer) {
    m_ignoredLock = umbrielfx_scene_buffer_ignore_client_lock(buffer) != nullptr;
  }

  void SsdFrame::releaseIgnoredLock() {
    if (!m_ignoredLock) {
      return;
    }
    m_ignoredLock = false;
    // Ask the scene buffer which buffer it holds instead of remembering the client buffer: once the client releases
    // it, the scene drops its lock and the client buffer can be freed, which would leave a remembered pointer
    // dangling. Nothing to undo when the scene has already let go.
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

  void SsdFrame::hideAdaptive() {
    if (m_adaptive == nullptr) {
      return;
    }
    wlr_scene_node_set_enabled(&m_adaptive->node, false);
    // Drop the mirror as well: a node that keeps the buffer holds its lock, and the client's wl_buffer.release never
    // arrives, which stalls a client that waits for it before drawing again.
    releaseIgnoredLock();
    wlr_scene_buffer_set_buffer(m_adaptive, nullptr);
    m_adaptiveBuffer = nullptr;
  }

  std::optional<std::array<float, 4>> SsdFrame::averageStrip(const wlr_fbox& strip) {
    // A readback is the only way to collapse a strip into one colour, and it is the one cost this mode cannot avoid:
    // wlroots finishes the frame before it reads. Everything else in this class mirrors buffers instead.
    if (m_content == nullptr || m_content->buffer == nullptr || m_content->buffer->texture == nullptr) {
      return std::nullopt;
    }
    const int left = static_cast<int>(std::floor(strip.x));
    const int top = static_cast<int>(std::floor(strip.y));
    const int right = static_cast<int>(std::ceil(strip.x + strip.width));
    const int bottom = static_cast<int>(std::ceil(strip.y + strip.height));
    const wlr_box box{left, top, std::max(0, right - left), std::max(0, bottom - top)};
    if (box.width <= 0 || box.height <= 0) {
      return std::nullopt;
    }
    // Ask for the format this renderer can read rather than assuming BGRA: an implementation without
    // EXT_read_format_bgra refuses it, and the mode would silently never work there.
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
      return std::nullopt;
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
    // Refused for a protected or external-only buffer; the palette stays painted underneath either way.
    if (!wlr_texture_read_pixels(m_content->buffer->texture, &options)) {
      return std::nullopt;
    }
    return averagePixels(m_strip.data(), m_strip.size(), *order);
  }

  wlr_buffer* SsdFrame::averageBufferFor(const std::array<float, 4>& color) {
    // Compare against the colour baked into the buffer, never against `m_averageColor`: that already holds the new
    // mean by the time this is called, so comparing with it would always match and freeze the first colour.
    if (m_averageBuffer != nullptr && m_averageBufferColor == color) {
      return m_averageBuffer;
    }
    // One pixel, stretched over the bar: a flat colour costs no per-frame readback once it is known.
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_paint(cr);
    cairo_destroy(cr);
    wlr_buffer* buffer = createCairoBuffer(surface);
    if (buffer == nullptr) {
      return nullptr;
    }
    if (m_averageBuffer != nullptr) {
      wlr_buffer_drop(m_averageBuffer);
    }
    m_averageBuffer = buffer;
    m_averageBufferColor = color;
    return m_averageBuffer;
  }

  void SsdFrame::updateAdaptive(bool fromCommit) {
    // The palette colour stays painted underneath: a window whose top edge is transparent, or one that has not
    // committed a buffer yet, keeps the configured bar instead of leaving a hole.
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
      m_adaptiveContent = nullptr;
      m_adaptiveMode = TitleBarColorSource::Fixed;
      m_averageColor.reset();
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
    const int surfaceWidth = m_content->current.width;
    const int surfaceHeight = m_content->current.height;
    wlr_fbox base{};
    wlr_surface_get_buffer_source_box(m_content, &base);
    // The window's own geometry, not the buffer's: a client-drawn window can carry transparent shadow margins that
    // `xdg_surface.set_window_geometry` excludes, and those margins must not become the bar's colour.
    wlr_box visible{0, 0, surfaceWidth, surfaceHeight};
    if (m_contentGeometry.width > 0 && m_contentGeometry.height > 0) {
      wlr_box clipped{};
      wlr_box_intersection(&clipped, &m_contentGeometry, &visible);
      if (clipped.width > 0 && clipped.height > 0) {
        visible = clipped;
      }
    }
    const double scaleX = surfaceWidth > 0 ? base.width / static_cast<double>(surfaceWidth) : 0.0;
    const double scaleY = surfaceHeight > 0 ? base.height / static_cast<double>(surfaceHeight) : 0.0;
    wlr_fbox source = base;
    int sourceSurfaceWidth = surfaceWidth;
    int sourceSurfaceHeight = surfaceHeight;
    if (scaleX > 0.0 && scaleY > 0.0 && base.width > 0.0 && base.height > 0.0) {
      const double left = std::max(base.x, base.x + visible.x * scaleX);
      const double top = std::max(base.y, base.y + visible.y * scaleY);
      const double right = std::min(base.x + base.width, base.x + (visible.x + visible.width) * scaleX);
      const double bottom = std::min(base.y + base.height, base.y + (visible.y + visible.height) * scaleY);
      if (right > left && bottom > top) {
        source = {left, top, right - left, bottom - top};
        sourceSurfaceWidth = std::max(1, static_cast<int>(std::lround(source.width / scaleX)));
        sourceSurfaceHeight = std::max(1, static_cast<int>(std::lround(source.height / scaleY)));
      }
    }
    // Colour management can change on a commit that carries no buffer damage, so this cannot sit behind the guard
    // below; the setters it reaches are no-ops when nothing changed. The averaged colour is a plain sRGB value of its
    // own, so a null surface resets the node to sRGB rather than inheriting the window's transfer function.
    if (m_colors != nullptr) {
      m_colors->applySurfaceDescriptionToBuffer(average ? nullptr : m_content, m_adaptive);
    }
    const wlr_fbox strip = titleBarStripSourceBox(
        source, buffer->width, buffer->height, sourceSurfaceWidth, sourceSurfaceHeight,
        static_cast<uint32_t>(m_content->current.transform), style.colorSourceHeight
    );
    // Only what the bar shows matters, and the damage is already in buffer coordinates, which is where the strip is.
    // The strip is fractional at fractional scales, so round it outward: a damaged edge row still has to count. The
    // damage belongs to the commit that produced it, so a repaint of the chrome in between does not replay it.
    const int left = static_cast<int>(std::floor(strip.x));
    const int top = static_cast<int>(std::floor(strip.y));
    const int right = static_cast<int>(std::ceil(strip.x + strip.width));
    const int bottom = static_cast<int>(std::ceil(strip.y + strip.height));
    pixman_region32_t stripRegion;
    pixman_region32_init_rect(&stripRegion, left, top, std::max(0, right - left), std::max(0, bottom - top));
    const bool touched = fromCommit && pixman_region32_intersect(&stripRegion, &stripRegion, &m_content->buffer_damage);
    pixman_region32_fini(&stripRegion);
    const float opacity = m_alpha * (m_focused ? 1.0F : kAdaptiveUnfocusedOpacity);
    const int barX = m_background->node.x;
    const int barY = m_background->node.y;
    const bool stripMoved = !fboxEqual(m_adaptiveStrip, strip);
    // A mode switch is not a commit, so it has to force the work itself: otherwise an idle window keeps the palette
    // (or the mean of an edge that changed while the bar was smearing) until its next top-edge damage.
    const bool modeChanged = m_adaptiveMode != style.colorSource;
    // The edge changed: the smear re-mirrors it, and the average reads it back and replaces its flat colour.
    const bool restrip = touched || modeChanged || m_adaptiveContent != buffer || stripMoved;
    // What the bar shows. The smear mirrors the strip itself; the average stretches one pixel, so it carries none of
    // the window's own detail and is one uniform colour by construction.
    wlr_buffer* shown = buffer;
    const wlr_fbox* sourceBox = &strip;
    wl_output_transform transform = m_content->current.transform;
    if (average) {
      if (restrip) {
        m_averageColor = averageStrip(strip);
      }
      shown = m_averageColor.has_value() ? averageBufferFor(*m_averageColor) : nullptr;
      if (shown == nullptr) {
        hideAdaptive();
        m_adaptiveContent = buffer;
        m_adaptiveMode = style.colorSource;
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
        || !fx_corner_radii_eq(m_adaptive->corners, m_background->corners);
    // A commit that leaves the top edge alone, and changes nothing about the bar, costs nothing.
    if (!restrip && !reshaped) {
      return;
    }
    if (m_adaptiveBuffer != shown || (touched && !average)) {
      // Mirror the window the same way the overview mirrors a card: the surface's committed buffer plus its damage, and
      // the explicit-sync acquire point when the client uses it, so the bar never samples a half-presented frame.
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
      releaseIgnoredLock();
      wlr_scene_buffer_set_buffer_with_options(m_adaptive, shown, &options);
      // The scene holds a second lock on the client's buffer; without this wlroots cannot reuse its texture for
      // in-place shm damage and rebuilds the whole window's texture on every commit.
      if (!average) {
        ignoreLock(shown);
      }
    }
    m_adaptiveBuffer = shown;
    m_adaptiveContent = buffer;
    m_adaptiveMode = style.colorSource;
    m_adaptiveStrip = strip;
    m_adaptiveTransform = transform;
    m_adaptiveWidth = m_background->width;
    m_adaptiveHeight = m_background->height;
    m_adaptiveOpacity = opacity;
    wlr_scene_buffer_set_source_box(m_adaptive, sourceBox);
    wlr_scene_buffer_set_transform(m_adaptive, transform);
    wlr_scene_buffer_set_dest_size(m_adaptive, m_background->width, m_background->height);
    wlr_scene_buffer_set_corner_radii(m_adaptive, m_background->corners);
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

  // The title the cache renders for a bar of this geometry, with the layout re-measured against the line's height so
  // the text can reach into the frame's rounded corners. The live bar and its snapshot ask the same way.
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
      const auto& glyphColor = enabled && (hovered || pressed) ? hoverGlyph : idleGlyph;
      const bool glyphVisible = style.buttonGlyph == TitleBarButtonGlyph::Always
          || (style.buttonGlyph == TitleBarButtonGlyph::Hover && (hovered || pressed));
      updateButtonIcon(
          which, glyphColor, m_alpha * (enabled ? 1.0F : 0.35F), glyphVisible,
          style.colorSource != TitleBarColorSource::Fixed ? adaptiveOutline(glyphColor[3]) : std::nullopt
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
    const TabLabel* label = measureTitle(textColor, 1.0F, parts, radius);
    // The cache is an LRU and the shadow lookup below can evict the entry `label` points at, so hold the buffer and
    // remember the size now.
    wlr_buffer* labelBuffer = label != nullptr ? wlr_buffer_lock(label->buffer) : nullptr;
    const int labelWidth = label != nullptr ? label->width : 0;
    const int labelHeight = label != nullptr ? label->height : 0;
    // A window-coloured bar can be lighter or darker than the palette, so the title keeps a dark edge under it.
    const bool shadowed = style.colorSource != TitleBarColorSource::Fixed;
    const TabLabel* shadow = nullptr;
    if (shadowed && labelBuffer != nullptr && textColor[3] > 0.0F) {
      // The shadow re-measures the same layout; keep the title's copy intact for the text below it.
      TitleBarLayout shadowParts = parts;
      shadow = measureTitle(kAdaptiveShadowColor, textColor[3], shadowParts, radius);
    }
    if (labelBuffer == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, labelBuffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, m_adaptive != nullptr ? &m_adaptive->node : &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, labelBuffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(labelWidth)), std::max(1, zoomed(labelHeight)));
      wlr_scene_node_set_position(&m_label->node, zoomed(parts.title.x), zoomed(-h + (h - labelHeight) / 2));
      clipBuffer(m_label);
    }
    if (labelBuffer != nullptr) {
      wlr_buffer_unlock(labelBuffer);
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
    // The live tree stacks palette, fill, buttons, text: copy in that order, or the fill covers the button
    // backgrounds for the length of the animation.
    copyRect(m_background);
    // A scene buffer drops its CPU-side buffer once the renderer has uploaded the texture, so a live chrome node can
    // still be drawing while its buffer field is null: fall back to a buffer this frame still holds a lock on.
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
    // The adaptive fill sits above the palette rects and below the text, exactly as in the live tree: creating it last
    // would stack it over the title and the buttons for the length of the close animation.
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
    const auto& style = config().appearance.titleBar;
    const auto& colors = config().colors.titleBar;
    const std::array<float, 4>& textColor = m_focused ? colors.text : colors.textUnfocused;
    const int radius = nestedRadius(m_geometry.cornerRadius, m_geometry.borderInset);
    TitleBarLayout parts = titleBarLayout(
        m_geometry.contentWidth, m_geometry.titleBarHeight, style.buttonPosition, visibleButtons(),
        style.buttonGeometry(), radius
    );
    const TabLabel* label = measureTitle(textColor, 1.0F, parts, radius);
    // The cache is an LRU and the shadow lookup below can evict the entry `label` points at, so hold the buffer.
    wlr_buffer* labelBuffer = label != nullptr ? wlr_buffer_lock(label->buffer) : nullptr;
    wlr_buffer* shadow = nullptr;
    if (style.colorSource != TitleBarColorSource::Fixed && textColor[3] > 0.0F) {
      TitleBarLayout shadowParts = parts;
      if (const TabLabel* rendered = measureTitle(kAdaptiveShadowColor, textColor[3], shadowParts, radius)) {
        shadow = rendered->buffer;
      }
    }
    copyRect(m_close);
    copyRect(m_maximize);
    copyRect(m_minimize);
    copyBuffer(m_labelShadow, shadow);
    copyBuffer(m_label, labelBuffer);
    if (labelBuffer != nullptr) {
      wlr_buffer_unlock(labelBuffer);
    }
    for (size_t index = 0; index < m_icons.size(); ++index) {
      // The icon's own buffer has been released too; `m_iconBuffers` holds a consumer lock for exactly this copy.
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
      // A card shows the same bar, so it needs the same window to take its colour from.
      frame->setContentSurface(m_content, m_contentGeometry);
      frame->reloadConfig();
    }
  }
} // namespace umbriel
