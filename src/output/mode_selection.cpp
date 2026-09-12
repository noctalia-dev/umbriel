#include "output/mode_selection.h"

#include "wlr_color.h"

#include <cmath>

extern "C" {
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  wlr_output_mode* selectOutputMode(wlr_output* output, const OutputMode& configured) {
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
    return selected;
  }

  wlr_output_mode* preferredFallbackMode(wlr_output* output, const wlr_output_mode* staged) {
    wlr_output_mode* preferred = wlr_output_preferred_mode(output);
    return preferred == staged ? nullptr : preferred;
  }

} // namespace umbriel
