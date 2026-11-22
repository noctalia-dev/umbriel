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

  namespace {
    struct TextExtent {
      int width = 0;
      int height = 0;
    };

    // Shapes the text on a throwaway surface: its own box, without padding or the shadow's overhang.
    TextExtent measureText(const TextBufferParams& params) {
      const double scale = std::max(1.0, params.scale);

      cairo_surface_t* measureSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* measureCr = cairo_create(measureSurface);
      cairo_scale(measureCr, scale, scale);
      useGrayscaleAntialias(measureCr);
      PangoLayout* measureLayout = pango_cairo_create_layout(measureCr);
      PangoFontDescription* fontDesc = pango_font_description_from_string(params.font.c_str());
      pango_layout_set_font_description(measureLayout, fontDesc);
      pango_layout_set_width(measureLayout, params.maxWidth * PANGO_SCALE);
      if (params.ellipsize) {
        pango_layout_set_ellipsize(measureLayout, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(measureLayout, TRUE);
      } else {
        pango_layout_set_wrap(measureLayout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_indent(measureLayout, -params.hangingIndent * PANGO_SCALE);
      }
      pango_layout_set_markup(measureLayout, params.markup.c_str(), -1);

      int textW = 0;
      int textH = 0;
      pango_layout_get_pixel_size(measureLayout, &textW, &textH);
      g_object_unref(measureLayout);
      pango_font_description_free(fontDesc);
      cairo_destroy(measureCr);
      cairo_surface_destroy(measureSurface);
      return {.width = textW, .height = textH};
    }

    // The surface the text is painted on: its padding, plus the shadow's offset so the edge is not clipped.
    TextBufferSize surfaceSize(const TextBufferParams& params, const TextExtent& text, const TextShadow& shadow) {
      const int textW = params.fixedWidth ? std::max(params.maxWidth, text.width) : text.width;
      return {
          .logicalWidth = textW + params.padding * 2 + std::max(0, shadow.dx),
          .logicalHeight = text.height + params.padding * 2 + std::max(0, shadow.dy),
      };
    }
  } // namespace

  std::string escapeMarkup(std::string_view text) {
    gchar* escaped = g_markup_escape_text(text.data(), static_cast<gssize>(text.size()));
    std::string result(escaped);
    g_free(escaped);
    return result;
  }

  TextBufferSize measureTextBuffer(const TextBufferParams& params, const TextShadow& shadow) {
    return surfaceSize(params, measureText(params), shadow);
  }

  TextBufferResult renderTextBuffer(const TextBufferParams& params, const TextShadow& shadow) {
    const double scale = std::max(1.0, params.scale);
    const TextExtent text = measureText(params);
    const TextBufferSize extent = surfaceSize(params, text, shadow);
    const int textW = extent.logicalWidth - params.padding * 2 - std::max(0, shadow.dx);
    const int logicalW = extent.logicalWidth;
    const int logicalH = extent.logicalHeight;

    // Compute logical and device-pixel dimensions.
    int pixelW = static_cast<int>(std::ceil(logicalW * scale));
    int pixelH = static_cast<int>(std::ceil(logicalH * scale));
    pixelW = std::max(pixelW, 1);
    pixelH = std::max(pixelH, 1);

    // Render to a CairoBuffer at device-pixel resolution.
    cairo_surface_t* pixelSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelW, pixelH);
    cairo_t* cr = cairo_create(pixelSurface);
    cairo_scale(cr, scale, scale);
    useGrayscaleAntialias(cr);

    // Background.
    cairo_set_source_rgba(cr, params.bgR, params.bgG, params.bgB, params.bgA);
    cairo_paint(cr);

    // Text (all coordinates are logical; cairo_scale handles device mapping).
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* fontDesc = pango_font_description_from_string(params.font.c_str());
    pango_layout_set_font_description(layout, fontDesc);
    // An ellipsized line keeps the measuring width: shaping it to its own measured width can round into a second cut.
    pango_layout_set_width(layout, (params.ellipsize || params.fixedWidth ? params.maxWidth : textW) * PANGO_SCALE);
    if (params.ellipsize) {
      pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
      pango_layout_set_single_paragraph_mode(layout, TRUE);
    } else {
      pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
      pango_layout_set_indent(layout, -params.hangingIndent * PANGO_SCALE);
    }
    // The shadow first, so the text lands on top of it and the two composite into one surface.
    if (!shadow.markup.empty() && (shadow.dx != 0 || shadow.dy != 0)) {
      pango_layout_set_markup(layout, shadow.markup.c_str(), -1);
      cairo_move_to(cr, params.padding + shadow.dx, params.padding + shadow.dy);
      pango_cairo_show_layout(cr, layout);
    }
    pango_layout_set_markup(layout, params.markup.c_str(), -1);
    cairo_move_to(cr, params.padding, params.padding);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    pango_font_description_free(fontDesc);
    cairo_destroy(cr);

    return {
        .buffer = createCairoBuffer(pixelSurface),
        .logicalWidth = logicalW,
        .logicalHeight = logicalH,
        .textWidth = logicalW - std::max(0, shadow.dx),
        .textHeight = logicalH - std::max(0, shadow.dy),
    };
  }

} // namespace umbriel
