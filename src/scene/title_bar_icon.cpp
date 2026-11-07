#include "scene/title_bar_icon.h"

#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cairo.h>

namespace umbriel {
  wlr_buffer* renderTitleBarIcon(
      TitleBarButton button, int pixels, const std::array<float, 4>& color,
      const std::optional<std::array<float, 4>>& outline
  ) {
    pixels = std::max(1, pixels);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, pixels, pixels);
    const auto path = [&](cairo_t* context) {
      switch (button) {
      case TitleBarButton::Close:
        cairo_move_to(context, 0.25, 0.25);
        cairo_line_to(context, 0.75, 0.75);
        cairo_move_to(context, 0.75, 0.25);
        cairo_line_to(context, 0.25, 0.75);
        break;
      case TitleBarButton::Maximize:
        cairo_rectangle(context, 0.25, 0.25, 0.5, 0.5);
        break;
      case TitleBarButton::Minimize:
        cairo_move_to(context, 0.25, 0.7);
        cairo_line_to(context, 0.75, 0.7);
        break;
      }
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (outline) {
      cairo_set_source_rgba(cr, (*outline)[0], (*outline)[1], (*outline)[2], (*outline)[3]);
      cairo_set_line_width(cr, 1.0 / 12.0 * 2.5);
      path(cr);
      cairo_stroke(cr);
    }
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, 1.0 / 12.0);
    path(cr);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return createCairoBuffer(surface);
  }
} // namespace umbriel
