#pragma once

#include <string>
#include <string_view>

struct wlr_buffer;

namespace umbriel {

  struct TextBufferParams {
    std::string markup; // Pango markup
    std::string font = "monospace 11";
    int maxWidth = 800; // logical pixels (text wrap limit)
    int padding = 14;   // logical pixels on all sides
    double scale = 1.0; // device-pixel scale (ceil of output scale)
    // Logical pixels every wrapped continuation line is indented by, so a wrapped
    // list entry stays under its own text instead of returning to the margin.
    int hangingIndent = 0;
    // One line cut to `maxWidth` with a trailing ellipsis, instead of wrapping.
    bool ellipsize = false;
    // Background colour.
    double bgR = 0.0;
    double bgG = 0.0;
    double bgB = 0.0;
    double bgA = 0.0;
  };

  struct TextBufferResult {
    wlr_buffer* buffer = nullptr; // caller must wlr_buffer_drop after handing to scene
    int logicalWidth = 0;
    int logicalHeight = 0;
  };

  [[nodiscard]] std::string escapeMarkup(std::string_view text);

  // Render Pango-markup text into a Cairo-backed wlr_buffer.
  // Returns a buffer the caller owns (drop after wlr_scene_buffer_create).
  [[nodiscard]] TextBufferResult renderTextBuffer(const TextBufferParams& params);

} // namespace umbriel
