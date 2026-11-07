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
  // Only the receiver's position places the copy; the mapping subtracts the source origin.
  CHECK(fitMirror({4000, 300, 1280, 720}, {0, 0, 960, 720}) == fitMirror({0, 0, 1280, 720}, {0, 0, 960, 720}));
}

UMBRIEL_TEST(mirrorMappingPlacesSourceBoxesOnTheReceiver) {
  // Source 1280x720 at (0,0) shown on a 960x720 receiver at (1280,0): scale 0.75, 90 pixels of band on top.
  const auto mapping = umbriel::mirrorMapping({0, 0, 1280, 720}, {1280, 0, 960, 720});
  CHECK(mapping.has_value());
  if (mapping) {
    const wlr_box window = mapping->toReceiver({100, 100, 400, 300});
    CHECK_EQ(window.x, 1355);
    CHECK_EQ(window.y, 165);
    CHECK_EQ(window.width, 300);
    CHECK_EQ(window.height, 225);
  }
  CHECK(!umbriel::mirrorMapping({0, 0, 0, 720}, {1280, 0, 960, 720}));
}

UMBRIEL_TEST(mirrorMappingReturnsReceivingPointsToTheSource) {
  // A source on the right of the layout, so its origin matters: the inverse lands back where the point was drawn from.
  const auto mapping = umbriel::mirrorMapping({2560, 0, 1920, 1080}, {0, 200, 1280, 540});
  CHECK(mapping.has_value());
  if (mapping) {
    CHECK_NEAR(mapping->toSourceX(mapping->toReceiverX(3000)), 3000.0, 2.0);
    CHECK_NEAR(mapping->toSourceY(mapping->toReceiverY(700)), 700.0, 2.0);
    // The receiver's top-left corner of the fitted copy is the source's origin.
    CHECK_NEAR(mapping->toSourceX(mapping->fit.x), 2560.0, 1e-9);
    CHECK_NEAR(mapping->toSourceY(mapping->fit.y), 0.0, 1e-9);
  }
}

int main() { return RUN_TESTS(); }
