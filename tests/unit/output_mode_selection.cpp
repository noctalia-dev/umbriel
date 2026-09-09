#include "check.h"
#include "output/mode_selection.h"

// Pulls the C++ math headers in before `static` is defined away below.
#include <cmath> // IWYU pragma: keep

extern "C" {
// wlroots uses C99 array parameter syntax in headers included by wlr_output.h.
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

using umbriel::OutputMode;
using umbriel::preferredFallbackMode;
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

UMBRIEL_TEST(configuredResolutionSelectsClosestRefresh) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(5120, 1440, 119998, true);
  wlr_output_mode closest = outputMode(5120, 1440, 143987);
  addMode(output, lower);
  addMode(output, closest);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{5120, 1440, 144000});

  CHECK(selected == &closest);
  CHECK_EQ(selected->refresh, 143987);
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshPrefersMarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode fastest = outputMode(2560, 1440, 165000);
  wlr_output_mode preferred = outputMode(2560, 1440, 59951, true);
  addMode(output, fastest);
  addMode(output, preferred);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK(selected == &preferred);
  CHECK_EQ(selected->refresh, 59951);
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshUsesHighestUnmarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(2560, 1440, 59951);
  wlr_output_mode highest = outputMode(2560, 1440, 119998);
  addMode(output, lower);
  addMode(output, highest);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK(selected == &highest);
  CHECK_EQ(selected->refresh, 119998);
}

// An unadvertised resolution stays a custom mode: the caller only leaves it behind once the commit fails.
UMBRIEL_TEST(unadvertisedResolutionSelectsNoAdvertisedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  addMode(output, preferred);

  CHECK(selectOutputMode(&output, OutputMode{5120, 1440, 143987}) == nullptr);
}

UMBRIEL_TEST(outputWithoutAdvertisedModesSelectsNoAdvertisedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(selectOutputMode(&output, OutputMode{1280, 720, 0}) == nullptr);
}

UMBRIEL_TEST(failedCustomModeFallsBackToPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  wlr_output_mode slower = outputMode(2560, 1440, 59951);
  addMode(output, preferred);
  addMode(output, slower);

  const wlr_output_mode* fallback = preferredFallbackMode(&output, nullptr);

  CHECK(fallback == &preferred);
  CHECK_EQ(fallback->refresh, 119998);
}

UMBRIEL_TEST(failedAdvertisedModeFallsBackToPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  wlr_output_mode slower = outputMode(2560, 1440, 59951);
  addMode(output, preferred);
  addMode(output, slower);

  const wlr_output_mode* fallback = preferredFallbackMode(&output, &slower);

  CHECK(fallback == &preferred);
  CHECK_EQ(fallback->refresh, 119998);
}

// Retrying the mode that just failed would only fail again.
UMBRIEL_TEST(failedPreferredModeHasNoFallback) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  addMode(output, preferred);

  CHECK(preferredFallbackMode(&output, &preferred) == nullptr);
}

UMBRIEL_TEST(outputWithoutAdvertisedModesHasNoFallback) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(preferredFallbackMode(&output, nullptr) == nullptr);
}

int main() { return RUN_TESTS(); }
