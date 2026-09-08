#pragma once

#include "config/value_parse.h"

struct wlr_output;
struct wlr_output_mode;

namespace umbriel {

  enum class OutputModeChoice {
    Configured,
    PreferredFallback,
    Custom,
  };

  struct OutputModeSelection {
    wlr_output_mode* mode;
    OutputModeChoice choice;
  };

  [[nodiscard]] OutputModeSelection selectOutputMode(wlr_output* output, const OutputMode& configured);

} // namespace umbriel
