#include "scene/title_bar_icon.h"

#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cairo.h>

namespace umbriel {
  TitleBarGlyph titleBarGlyph(int button, int size, double stroke) {
    const double side = std::max(1, button);
    TitleBarGlyph glyph;
    if (size > 0) {
      glyph.size = std::min(1.0, size / side);
    }
    if (stroke > 0.0) {
      glyph.stroke = stroke / side;
    }
    return glyph;
  }

  wlr_buffer* renderTitleBarIcon(
      TitleBarButton button, int pixels, const std::array<float, 4>& color, const TitleBarGlyph& glyph,
      const std::optional<std::array<float, 4>>& outline
  ) {
    pixels = std::max(1, pixels);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, pixels, pixels);
    const double lo = 0.5 - (glyph.size / 2.0);
    const double hi = 0.5 + (glyph.size / 2.0);
    const auto path = [&](cairo_t* context) {
      switch (button) {
      case TitleBarButton::Close:
        cairo_move_to(context, lo, lo);
        cairo_line_to(context, hi, hi);
        cairo_move_to(context, hi, lo);
        cairo_line_to(context, lo, hi);
        break;
      case TitleBarButton::Maximize:
        cairo_rectangle(context, lo, lo, glyph.size, glyph.size);
        break;
      case TitleBarButton::Minimize: {
        // An underscore sits low in the symbol's square, as a text underscore does.
        const double y = 0.5 + (glyph.size * 0.4);
        cairo_move_to(context, lo, y);
        cairo_line_to(context, hi, y);
        break;
      }
      }
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (outline) {
      cairo_set_source_rgba(cr, (*outline)[0], (*outline)[1], (*outline)[2], (*outline)[3]);
      cairo_set_line_width(cr, glyph.stroke * 2.5);
      path(cr);
      cairo_stroke(cr);
    }
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, glyph.stroke);
    path(cr);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return createCairoBuffer(surface);
  }
} // namespace umbriel
