#include "scene/title_bar_icon.h"

#include "check.h"

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

#include <cstddef>
#include <cstdint>
#include <drm_fourcc.h>

namespace {
  struct Pixels {
    wlr_buffer* buffer;
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    bool accessible = false;

    Pixels(umbriel::TitleBarButton button, int size)
        : buffer(umbriel::renderTitleBarIcon(button, size, {1.0F, 1.0F, 1.0F, 1.0F})) {
      accessible = wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride);
      CHECK(accessible);
      CHECK_EQ(format, DRM_FORMAT_ARGB8888);
      CHECK_EQ(buffer->width, size);
      CHECK_EQ(buffer->height, size);
    }

    ~Pixels() {
      if (accessible)
        wlr_buffer_end_data_ptr_access(buffer);
      wlr_buffer_drop(buffer);
    }

    uint32_t at(int x, int y) const {
      const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + y * stride);
      return row[x];
    }
    uint32_t alpha(int x, int y) const { return at(x, y) >> 24; }
  };
} // namespace

UMBRIEL_TEST(buttonSymbolsHaveDistinctGeometry) {
  using umbriel::TitleBarButton;
  for (const int size : {8, 14, 18, 28, 64}) {
    const Pixels close(TitleBarButton::Close, size);
    const Pixels maximize(TitleBarButton::Maximize, size);
    const Pixels minimize(TitleBarButton::Minimize, size);
    if (!close.accessible || !maximize.accessible || !minimize.accessible)
      return;
    CHECK(close.alpha(size / 2, size / 2) > 0);
    CHECK_EQ(maximize.alpha(size / 2, size / 2), 0U);
    CHECK_EQ(minimize.alpha(size / 2, size / 2), 0U);
    CHECK(maximize.alpha(size / 2, size / 4) > 0);
    CHECK(minimize.alpha(size / 2, size * 7 / 10) > 0);
    CHECK_EQ(minimize.alpha(size / 2, size / 4), 0U);
  }
}

UMBRIEL_TEST(buttonSymbolsKeepTransparentAntialiasedEdges) {
  using umbriel::TitleBarButton;
  for (const auto button : {TitleBarButton::Close, TitleBarButton::Maximize, TitleBarButton::Minimize}) {
    const Pixels image(button, 28);
    if (!image.accessible)
      return;
    int ink = 0;
    int antialiased = 0;
    for (int y = 0; y < 28; ++y) {
      for (int x = 0; x < 28; ++x) {
        const uint32_t pixel = image.at(x, y);
        const uint32_t alpha = pixel >> 24;
        CHECK_EQ((pixel >> 16) & 255U, alpha);
        CHECK_EQ((pixel >> 8) & 255U, alpha);
        CHECK_EQ(pixel & 255U, alpha);
        ink += alpha > 0;
        antialiased += alpha > 0 && alpha < 255;
      }
    }
    CHECK(ink > 0 && ink < 28 * 28 / 2);
    CHECK(antialiased > 0);
    CHECK_EQ(image.alpha(0, 0), 0U);
    CHECK_EQ(image.alpha(27, 27), 0U);
  }
}

int main() { return RUN_TESTS(); }
