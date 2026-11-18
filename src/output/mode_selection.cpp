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

  static bool
  isBetterForTarget(OutputModeTarget target, const wlr_output_mode* current, const wlr_output_mode* candidate) {
    if (current == nullptr || candidate == nullptr) {
      return false;
    }

    const bool higherRefreshRate = candidate->refresh > current->refresh;
    const bool higherResolution = (candidate->width >= current->width && candidate->height >= current->height)
        && (candidate->width != current->width || candidate->height != current->height);

    const bool betterPreference = candidate->preferred && !current->preferred;
    const bool samePreference = candidate->preferred == current->preferred;

    if (target == OutputModeTarget::HighRefreshRate) {
      const bool tieBreaker = betterPreference || (samePreference && higherResolution);

      return higherRefreshRate || (candidate->refresh == current->refresh && tieBreaker);
    }
    if (target == OutputModeTarget::HighResolution) {
      const bool tieBreaker = betterPreference || (samePreference && higherRefreshRate);

      return higherResolution
          || (candidate->width == current->width && candidate->height == current->height && tieBreaker);
    }

    return false;
  }

  wlr_output_mode* selectOutputMode(wlr_output* output, const OutputMode& configured) {
    wlr_output_mode* selected = nullptr;
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (configured.target != OutputModeTarget::None
          && (selected == nullptr || isBetterForTarget(configured.target, selected, mode))) {
        selected = mode;
        continue;
      }

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

  bool liveModeMatches(const wlr_output& output, const wlr_output_mode* mode) {
    return output.enabled && output.current_mode == mode;
  }

  bool outputEnableNeedsStaging(const wlr_output& output, bool enabled) { return !(enabled && output.enabled); }

  bool colorChangeNeedsModeset(const wlr_output& output, uint32_t renderFormat, bool hdr) {
    return output.render_format != renderFormat || (output.image_description != nullptr) != hdr;
  }

} // namespace umbriel
