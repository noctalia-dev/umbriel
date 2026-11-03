#include "input/xcursor_matcher.h"

#include <cstring>
#include <limits>

extern "C" {
#include <wayland-util.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
}

namespace umbriel {
  namespace {

    constexpr size_t kBytesPerPixel = 4;
    constexpr uint32_t kAlphaMask = 0xff000000U;

    bool validImage(const XcursorImageView& image) {
      if (image.width == 0 || image.height == 0 || image.pixels == nullptr) {
        return false;
      }
      return image.stride >= static_cast<size_t>(image.width) * kBytesPerPixel;
    }

    uint32_t normalizedPixel(const uint8_t* data) {
      uint32_t pixel = 0;
      std::memcpy(&pixel, data, sizeof(pixel));
      return (pixel & kAlphaMask) == 0 ? 0 : pixel;
    }

    XcursorImageView imageView(const wlr_xcursor_image* image) {
      if (image == nullptr) {
        return {};
      }
      return XcursorImageView{
          .width = image->width,
          .height = image->height,
          .hotspotX = image->hotspot_x,
          .hotspotY = image->hotspot_y,
          .pixels = image->buffer,
          .stride = static_cast<size_t>(image->width) * kBytesPerPixel,
      };
    }

    bool cursorContainsImage(const wlr_xcursor* cursor, XcursorImageView image) {
      if (cursor == nullptr || cursor->images == nullptr) {
        return false;
      }
      for (unsigned int frame = 0; frame < cursor->image_count; ++frame) {
        if (xcursorImagesEqual(imageView(cursor->images[frame]), image)) {
          return true;
        }
      }
      return false;
    }

    const wlr_xcursor* findCursor(const wlr_xcursor_theme* theme, std::string_view name) {
      if (theme->cursors == nullptr) {
        return nullptr;
      }
      for (unsigned int index = 0; index < theme->cursor_count; ++index) {
        const wlr_xcursor* cursor = theme->cursors[index];
        if (cursor != nullptr && cursor->name != nullptr && name == cursor->name) {
          return cursor;
        }
      }
      return nullptr;
    }

  } // namespace

  bool xcursorImagesEqual(XcursorImageView first, XcursorImageView second) {
    if (!validImage(first) || !validImage(second)) {
      return false;
    }
    if (first.width != second.width
        || first.height != second.height
        || first.hotspotX != second.hotspotX
        || first.hotspotY != second.hotspotY) {
      return false;
    }

    for (uint32_t y = 0; y < first.height; ++y) {
      const uint8_t* firstRow = first.pixels + static_cast<size_t>(y) * first.stride;
      const uint8_t* secondRow = second.pixels + static_cast<size_t>(y) * second.stride;
      for (uint32_t x = 0; x < first.width; ++x) {
        const size_t offset = static_cast<size_t>(x) * kBytesPerPixel;
        if (normalizedPixel(firstRow + offset) != normalizedPixel(secondRow + offset)) {
          return false;
        }
      }
    }
    return true;
  }

  std::optional<std::string>
  matchXcursorThemeImage(const wlr_xcursor_theme* theme, XcursorImageView image, std::string_view preferredName) {
    if (theme == nullptr || !validImage(image)) {
      return std::nullopt;
    }

    const wlr_xcursor* preferred = nullptr;
    if (!preferredName.empty()) {
      preferred = findCursor(theme, preferredName);
      if (cursorContainsImage(preferred, image)) {
        return std::string(preferredName);
      }
    }

    if (theme->cursors == nullptr) {
      return std::nullopt;
    }
    for (unsigned int index = 0; index < theme->cursor_count; ++index) {
      const wlr_xcursor* cursor = theme->cursors[index];
      if (cursor == preferred || cursor == nullptr || cursor->name == nullptr) {
        continue;
      }
      if (cursorContainsImage(cursor, image)) {
        return std::string(cursor->name);
      }
    }
    return std::nullopt;
  }

  std::optional<std::string>
  matchXcursorManagerImage(wlr_xcursor_manager* manager, XcursorImageView image, std::string_view preferredName) {
    constexpr float kBaseScale = 1.0F;
    if (manager == nullptr || !wlr_xcursor_manager_load(manager, kBaseScale)) {
      return std::nullopt;
    }

    wlr_xcursor_manager_theme* scaledTheme = nullptr;
    wl_list_for_each(scaledTheme, &manager->scaled_themes, link) {
      if (scaledTheme->scale == kBaseScale) {
        return matchXcursorThemeImage(scaledTheme->theme, image, preferredName);
      }
    }
    return std::nullopt;
  }

} // namespace umbriel
