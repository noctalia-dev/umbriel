#include "scene/title_bar_icon.h"

#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cairo.h>

namespace umbriel {
  TitleBarGlyph titleBarGlyph(int button, int size, double width) {
    const double side = std::max(1, button);
    TitleBarGlyph glyph;
    if (size > 0) {
      glyph.size = std::min(1.0, size / side);
    }
    if (width > 0.0) {
      glyph.width = width / side;
    }
    return glyph;
  }

  wlr_buffer*
  renderTitleBarIcon(TitleBarButton button, int pixels, const std::array<float, 4>& color, const TitleBarGlyph& glyph) {
    pixels = std::max(1, pixels);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, pixels, pixels);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, glyph.width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double lo = 0.5 - (glyph.size / 2.0);
    const double hi = 0.5 + (glyph.size / 2.0);
    switch (button) {
    case TitleBarButton::Close:
      cairo_move_to(cr, lo, lo);
      cairo_line_to(cr, hi, hi);
      cairo_move_to(cr, hi, lo);
      cairo_line_to(cr, lo, hi);
      break;
    case TitleBarButton::Maximize:
      cairo_rectangle(cr, lo, lo, glyph.size, glyph.size);
      break;
    case TitleBarButton::Minimize: {
      // An underscore sits low in the symbol's square, as a text underscore does.
      const double y = 0.5 + (glyph.size * 0.4);
      cairo_move_to(cr, lo, y);
      cairo_line_to(cr, hi, y);
      break;
    }
    }
    cairo_stroke(cr);
    cairo_destroy(cr);
    return createCairoBuffer(surface);
  }
} // namespace umbriel
