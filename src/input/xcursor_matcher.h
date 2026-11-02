#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct wlr_xcursor_manager;
struct wlr_xcursor_theme;

namespace umbriel {

  // A packed DRM_FORMAT_ARGB8888 image. Rows may contain trailing padding.
  struct XcursorImageView {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hotspotX = 0;
    uint32_t hotspotY = 0;
    const uint8_t* pixels = nullptr;
    size_t stride = 0;
  };

  // Compares cursor identity, ignoring only the undefined RGB channels of
  // fully transparent pixels.
  bool xcursorImagesEqual(XcursorImageView first, XcursorImageView second);

  // Matches any animation frame. A matching preferred semantic name wins over
  // aliases with identical pixels, then the theme's stable cursor order wins.
  std::optional<std::string>
  matchXcursorThemeImage(const wlr_xcursor_theme* theme, XcursorImageView image, std::string_view preferredName = {});

  // Loads and searches the manager's scale-1 theme. The returned name remains
  // owned by the caller and is valid across theme-manager replacement.
  std::optional<std::string>
  matchXcursorManagerImage(wlr_xcursor_manager* manager, XcursorImageView image, std::string_view preferredName = {});

} // namespace umbriel
