#include "output/mode_selection.h"

#include "wlr_color.h"

#include <cmath>

extern "C" {
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  bool outputCanAutoEnable(wlr_output* output) {
    if (wl_list_empty(&output->modes)) {
      return true;
    }
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->preferred) {
        return true;
      }
    }
    const auto populated = [](const char* value) { return value != nullptr && value[0] != '\0'; };
    return populated(output->make) || populated(output->model) || populated(output->serial);
  }

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
