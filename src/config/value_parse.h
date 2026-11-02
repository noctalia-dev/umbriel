#pragma once

// Pure scalar parsers shared by the config readers. No wlroots, no I/O.

#include <array>
#include <string_view>

namespace umbriel {

  struct OutputMode {
    int width = 0;
    int height = 0;
    int refreshMHz = 0;

    bool operator==(const OutputMode&) const = default;
  };

  // "#RRGGBB" or "#RRGGBBAA". Alpha defaults to opaque. Components are normalized to 0.0-1.0. Returns false and leaves
  // `output` untouched on any malformed input.
  bool parseColor(std::string_view text, std::array<float, 4>& output);

  // "<width>x<height>" with an optional "@<hz>" suffix, e.g. "2560x1440@165" or
  // "1920x1080". Width and height are clamped to 1-16384, refresh to 0-1000 Hz.
  bool parseOutputMode(std::string_view text, OutputMode& output);

  // Environment names accepted by systemd and ordinary shells: an ASCII letter or underscore followed by ASCII
  // letters, digits, or underscores.
  bool isEnvironmentVariableName(std::string_view name);

} // namespace umbriel
