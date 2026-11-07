#include "workspace/mirror_fit.h"

#include "check.h"

#include <optional>

using umbriel::fitMirror;
using umbriel::MirrorFit;

UMBRIEL_TEST(mirrorFitRejectsAnEmptyOutput) {
  CHECK(!fitMirror({0, 0, 0, 720}, {0, 0, 960, 720}));
  CHECK(!fitMirror({0, 0, 1280, 0}, {0, 0, 960, 720}));
  CHECK(!fitMirror({0, 0, 1280, 720}, {0, 0, -960, 720}));
  CHECK(!fitMirror({0, 0, 1280, 720}, {0, 0, 960, 0}));
}

UMBRIEL_TEST(mirrorFitScalesByTheTighterAxisAndCentersTheOther) {
  // A narrower receiver fits by width and leaves bands above and below.
  CHECK(fitMirror({0, 0, 1280, 720}, {1280, 0, 960, 720}) == (MirrorFit{.scale = 0.75, .x = 1280, .y = 90}));
  // A wider one fits by height and leaves bands left and right, so the bottom of the source is never cropped.
  CHECK(fitMirror({0, 0, 1280, 720}, {1280, 0, 1280, 540}) == (MirrorFit{.scale = 0.75, .x = 1440, .y = 0}));
  // The treadmill case: 2560x1440 onto 1920x1080 keeps the aspect ratio and fills the receiver.
  CHECK(fitMirror({0, 0, 2560, 1440}, {2560, 200, 1920, 1080}) == (MirrorFit{.scale = 0.75, .x = 2560, .y = 200}));
}

UMBRIEL_TEST(mirrorFitIgnoresWhereTheSourceSits) {
  // Only the receiver's position places the copy; the source origin is subtracted by the callers.
  CHECK(fitMirror({4000, 300, 1280, 720}, {0, 0, 960, 720}) == fitMirror({0, 0, 1280, 720}, {0, 0, 960, 720}));
}

int main() { return RUN_TESTS(); }
