#pragma once

#include <cairo.h>

struct wlr_buffer;

namespace umbriel {
  // Takes ownership of an ARGB32 image surface. The caller drops the returned buffer after handing it to the scene.
  [[nodiscard]] wlr_buffer* createCairoBuffer(cairo_surface_t* surface);
} // namespace umbriel
