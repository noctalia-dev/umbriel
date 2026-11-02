#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  // What Umbriel announces to X11 clients through the XSETTINGS protocol. DPI values are in 1/1024 dots per inch.
  struct XsettingsValues {
    // Integer interface scale (Gdk/WindowScalingFactor).
    int32_t windowScalingFactor = 1;
    // Font DPI before the integer scale applies (Gdk/UnscaledDPI).
    int32_t unscaledDpi = 0;
    // Font DPI including the whole scale (Xft/DPI).
    int32_t xftDpi = 0;
    // Gtk/CursorThemeName; omitted when empty.
    std::string cursorTheme;
    // Gtk/CursorThemeSize in X pixels.
    int32_t cursorSize = 0;

    bool operator==(const XsettingsValues&) const = default;
  };

  // Settings for X11 clients drawing at `scale` X pixels per logical pixel. The scale splits into an integer interface
  // scale and the fractional rest carried by the unscaled DPI, so their product, like Xft/DPI, is 96 times `scale`.
  XsettingsValues xsettingsFor(double scale, std::string_view cursorTheme, int cursorSize);

  // The _XSETTINGS_SETTINGS property contents, little endian, with every setting stamped with `serial`.
  std::vector<uint8_t> encodeXsettings(uint32_t serial, const XsettingsValues& values);

} // namespace umbriel
