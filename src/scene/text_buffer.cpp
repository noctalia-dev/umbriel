#include "scene/text_buffer.h"

#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <glib.h>
#include <pango/pangocairo.h>

namespace {
  // Subpixel antialiasing assumes an opaque, pixel-aligned destination; these textures are blended, so it fringes.
  void useGrayscaleAntialias(cairo_t* cr) {
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_get_font_options(cr, options);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
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
    useGrayscaleAntialias(measureCr);
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
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelW, pixelH);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    useGrayscaleAntialias(cr);

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

    return {
        .buffer = createCairoBuffer(surface),
        .logicalWidth = logicalW,
        .logicalHeight = logicalH,
    };
  }

} // namespace umbriel
