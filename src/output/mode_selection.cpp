#include "output/mode_selection.h"

#include <cmath>

extern "C" {
// wlroots uses C99 array parameter syntax in headers included by wlr_output.h.
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

namespace umbriel {

  OutputModeSelection selectOutputMode(wlr_output* output, const OutputMode& configured) {
    wlr_output_mode* selected = nullptr;
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->width != configured.width || mode->height != configured.height) {
        continue;
      }
      if (configured.refreshMHz != 0) {
        if (selected == nullptr
            || std::abs(mode->refresh - configured.refreshMHz) < std::abs(selected->refresh - configured.refreshMHz)) {
          selected = mode;
        }
      } else if (
          selected == nullptr
          || (mode->preferred && !selected->preferred)
          || (mode->preferred == selected->preferred && mode->refresh > selected->refresh)
      ) {
        selected = mode;
      }
    }

    if (selected != nullptr) {
      return {.mode = selected, .choice = OutputModeChoice::Configured};
    }
    if (wlr_output_mode* preferred = wlr_output_preferred_mode(output)) {
      return {.mode = preferred, .choice = OutputModeChoice::PreferredFallback};
    }
    return {.mode = nullptr, .choice = OutputModeChoice::Custom};
  }

} // namespace umbriel
