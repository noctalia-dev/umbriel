#include "check.h"
#include "output/mode_selection.h"

#include <cmath>

extern "C" {
// wlroots uses C99 array parameter syntax in headers included by wlr_output.h.
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

using umbriel::OutputMode;
using umbriel::OutputModeChoice;
using umbriel::OutputModeSelection;
using umbriel::selectOutputMode;

namespace {
  wlr_output_mode outputMode(int width, int height, int refresh, bool preferred = false) {
    return {
        .width = width,
        .height = height,
        .refresh = refresh,
        .preferred = preferred,
        .picture_aspect_ratio = WLR_OUTPUT_MODE_ASPECT_RATIO_NONE,
        .link = {},
    };
  }

  void addMode(wlr_output& output, wlr_output_mode& mode) { wl_list_insert(output.modes.prev, &mode.link); }
} // namespace

UMBRIEL_TEST(unavailableConfiguredResolutionFallsBackToPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  addMode(output, preferred);

  const OutputModeSelection selection = selectOutputMode(&output, OutputMode{5120, 1440, 143987});

  CHECK_EQ(selection.mode, &preferred);
  CHECK(selection.choice == OutputModeChoice::PreferredFallback);
}

UMBRIEL_TEST(configuredResolutionSelectsClosestRefresh) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(5120, 1440, 119998, true);
  wlr_output_mode closest = outputMode(5120, 1440, 143987);
  addMode(output, lower);
  addMode(output, closest);

  const OutputModeSelection selection = selectOutputMode(&output, OutputMode{5120, 1440, 144000});

  CHECK_EQ(selection.mode, &closest);
  CHECK(selection.choice == OutputModeChoice::Configured);
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshPrefersMarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode fastest = outputMode(2560, 1440, 165000);
  wlr_output_mode preferred = outputMode(2560, 1440, 59951, true);
  addMode(output, fastest);
  addMode(output, preferred);

  const OutputModeSelection selection = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK_EQ(selection.mode, &preferred);
  CHECK(selection.choice == OutputModeChoice::Configured);
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshUsesHighestUnmarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(2560, 1440, 59951);
  wlr_output_mode highest = outputMode(2560, 1440, 119998);
  addMode(output, lower);
  addMode(output, highest);

  const OutputModeSelection selection = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK_EQ(selection.mode, &highest);
  CHECK(selection.choice == OutputModeChoice::Configured);
}

UMBRIEL_TEST(outputWithoutAdvertisedModesUsesCustomMode) {
  wlr_output output{};
  wl_list_init(&output.modes);

  const OutputModeSelection selection = selectOutputMode(&output, OutputMode{1280, 720, 0});

  CHECK_EQ(selection.mode, nullptr);
  CHECK(selection.choice == OutputModeChoice::Custom);
}

int main() { return RUN_TESTS(); }
