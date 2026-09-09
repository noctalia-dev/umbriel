#pragma once

#include "config/value_parse.h"

struct wlr_output;
struct wlr_output_mode;

namespace umbriel {

  // The advertised mode a configured `mode` names, or nullptr when the output advertises no mode at that resolution.
  // A configured refresh picks the closest advertised one; without a refresh the preferred mode wins, then the
  // highest refresh.
  [[nodiscard]] wlr_output_mode* selectOutputMode(wlr_output* output, const OutputMode& configured);

  // The mode to retry with after `staged` failed to commit, or nullptr when the output offers no other one. `staged`
  // is null for a custom mode, which the preferred advertised mode still replaces.
  [[nodiscard]] wlr_output_mode* preferredFallbackMode(wlr_output* output, const wlr_output_mode* staged);

} // namespace umbriel
