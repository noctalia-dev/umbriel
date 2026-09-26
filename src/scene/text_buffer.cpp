#include "scene/text_buffer.h"

extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
}

#include <cairo.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <drm_fourcc.h>
#include <glib.h>
#include <pango/pangocairo.h>

namespace {

  struct CairoBuffer {
    wlr_buffer base;
    cairo_surface_t* surface = nullptr;
  };

  void cairoBufferDestroy(wlr_buffer* wlrBuf) {
    CairoBuffer* buf;
    buf = wl_container_of(wlrBuf, buf, base);
    cairo_surface_destroy(buf->surface);
    delete buf;
  }

  bool
  cairoBufferBeginDataPtrAccess(wlr_buffer* wlrBuf, uint32_t /*flags*/, void** data, uint32_t* format, size_t* stride) {
    CairoBuffer* buf;
    buf = wl_container_of(wlrBuf, buf, base);
    *data = cairo_image_surface_get_data(buf->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = static_cast<size_t>(cairo_image_surface_get_stride(buf->surface));
    return true;
  }

  void cairoBufferEndDataPtrAccess(wlr_buffer* /*wlrBuf*/) {}

  const wlr_buffer_impl kCairoBufferImpl = {
      .destroy = cairoBufferDestroy,
      .get_dmabuf = nullptr,
      .get_shm = nullptr,
      .begin_data_ptr_access = cairoBufferBeginDataPtrAccess,
      .end_data_ptr_access = cairoBufferEndDataPtrAccess,
  };

  CairoBuffer* createCairoBuffer(int width, int height) {
    auto* buf = new CairoBuffer;
    buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    wlr_buffer_init(&buf->base, &kCairoBufferImpl, width, height);
    return buf;
  }

} // namespace

namespace umbriel {

  std::string escapeMarkup(std::string_view text) {
    gchar* escaped = g_markup_escape_text(text.data(), static_cast<gssize>(text.size()));
    std::string result(escaped);
    g_free(escaped);
    return result;
  }

  TextBufferResult renderTextBuffer(const TextBufferParams& params) {
    const double scale = std::max(1.0, params.scale);

    // Measure text with a throwaway 1×1 surface, scaled for HiDPI.
    cairo_surface_t* measureSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* measureCr = cairo_create(measureSurface);
    cairo_scale(measureCr, scale, scale);
    PangoLayout* measureLayout = pango_cairo_create_layout(measureCr);
    PangoFontDescription* fontDesc = pango_font_description_from_string(params.font.c_str());
    pango_layout_set_font_description(measureLayout, fontDesc);
    const auto shapeLayout = [&params](PangoLayout* layout, int width) {
      pango_layout_set_width(layout, width * PANGO_SCALE);
      if (params.ellipsize) {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(layout, TRUE);
      } else {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_indent(layout, -params.hangingIndent * PANGO_SCALE);
      }
    };
    shapeLayout(measureLayout, params.maxWidth);
    pango_layout_set_markup(measureLayout, params.markup.c_str(), -1);

    // pango_layout_get_pixel_size returns logical (pre-transform) dimensions.
    int textW = 0;
    int textH = 0;
    pango_layout_get_pixel_size(measureLayout, &textW, &textH);
    g_object_unref(measureLayout);
    cairo_destroy(measureCr);
    cairo_surface_destroy(measureSurface);

    // Compute logical and device-pixel dimensions.
    const int logicalW = textW + params.padding * 2;
    const int logicalH = textH + params.padding * 2;
    int pixelW = static_cast<int>(std::ceil(logicalW * scale));
    int pixelH = static_cast<int>(std::ceil(logicalH * scale));
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);

    // Render to a CairoBuffer at device-pixel resolution.
    CairoBuffer* cairoBuf = createCairoBuffer(pixelW, pixelH);
    cairo_t* cr = cairo_create(cairoBuf->surface);
    cairo_scale(cr, scale, scale);

    // Background.
    cairo_set_source_rgba(cr, params.bgR, params.bgG, params.bgB, params.bgA);
    cairo_paint(cr);

    // Text (all coordinates are logical; cairo_scale handles device mapping).
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, fontDesc);
    // An ellipsized line keeps the measuring width: shaping it to its own measured width can round into a second cut.
    shapeLayout(layout, params.ellipsize ? params.maxWidth : textW);
    pango_layout_set_markup(layout, params.markup.c_str(), -1);
    cairo_move_to(cr, params.padding, params.padding);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    pango_font_description_free(fontDesc);
    cairo_destroy(cr);
    cairo_surface_flush(cairoBuf->surface);

    return {
        .buffer = &cairoBuf->base,
        .logicalWidth = logicalW,
        .logicalHeight = logicalH,
    };
  }

} // namespace umbriel
