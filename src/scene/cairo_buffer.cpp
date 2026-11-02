#include "scene/cairo_buffer.h"

extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
}

#include <cstddef>
#include <cstdint>
#include <drm_fourcc.h>

namespace {
  struct CairoBuffer {
    wlr_buffer base;
    cairo_surface_t* surface = nullptr;
  };

  void destroy(wlr_buffer* buffer) {
    CairoBuffer* image;
    image = wl_container_of(buffer, image, base);
    cairo_surface_destroy(image->surface);
    delete image;
  }

  bool beginDataPtrAccess(wlr_buffer* buffer, uint32_t /*flags*/, void** data, uint32_t* format, size_t* stride) {
    CairoBuffer* image;
    image = wl_container_of(buffer, image, base);
    *data = cairo_image_surface_get_data(image->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = static_cast<size_t>(cairo_image_surface_get_stride(image->surface));
    return true;
  }

  void endDataPtrAccess(wlr_buffer* /*buffer*/) {}

  const wlr_buffer_impl kBufferImpl{
      .destroy = destroy,
      .get_dmabuf = nullptr,
      .get_shm = nullptr,
      .begin_data_ptr_access = beginDataPtrAccess,
      .end_data_ptr_access = endDataPtrAccess,
  };
} // namespace

namespace umbriel {
  wlr_buffer* createCairoBuffer(cairo_surface_t* surface) {
    auto* image = new CairoBuffer;
    image->surface = surface;
    cairo_surface_flush(surface);
    wlr_buffer_init(
        &image->base, &kBufferImpl, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface)
    );
    return &image->base;
  }
} // namespace umbriel
