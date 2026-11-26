#include "view/popup_constraint.h"

#include "check.h"

namespace {

  using umbriel::calculatePopupConstraintBox;
  using umbriel::PopupConstraintInput;

  void checkBox(const wlr_box& actual, int x, int y, int width, int height) {
    CHECK_EQ(actual.x, x);
    CHECK_EQ(actual.y, y);
    CHECK_EQ(actual.width, width);
    CHECK_EQ(actual.height, height);
  }

} // namespace

UMBRIEL_TEST(lucioleFullscreenRegressionUsesOutputBox) {
  const PopupConstraintInput input{
      .fullscreen = true,
      .tiled = true,
      .geometry = {.x = 0, .y = 0, .width = 948, .height = 522},
      .scheduledWidth = 1920,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 0,
      .winY = 0,
  };

  const wlr_box box = calculatePopupConstraintBox(input);
  checkBox(box, 0, 0, 1920, 1080);

  // Verify a 628x601 popup positioned at (646, 241) fits completely inside the constraint box.
  const wlr_box popup{.x = 646, .y = 241, .width = 628, .height = 601};
  CHECK(popup.x >= box.x);
  CHECK(popup.y >= box.y);
  CHECK(popup.x + popup.width <= box.x + box.width);
  CHECK(popup.y + popup.height <= box.y + box.height);
}

UMBRIEL_TEST(tiledNonFullscreenUsesTileWidthAndUsableArea) {
  const PopupConstraintInput withScheduled{
      .fullscreen = false,
      .tiled = true,
      .geometry = {.x = 0, .y = 0, .width = 948, .height = 1039},
      .scheduledWidth = 948,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 0,
      .winY = 41,
  };
  checkBox(calculatePopupConstraintBox(withScheduled), 0, 0, 948, 1039);

  const PopupConstraintInput fallbackToGeometryWidth{
      .fullscreen = false,
      .tiled = true,
      .geometry = {.x = 0, .y = 0, .width = 800, .height = 1039},
      .scheduledWidth = 0,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 960,
      .winY = 41,
  };
  checkBox(calculatePopupConstraintBox(fallbackToGeometryWidth), 0, 0, 800, 1039);
}

UMBRIEL_TEST(tiledResizeBeforeCommitPrefersScheduledWidth) {
  const PopupConstraintInput input{
      .fullscreen = false,
      .tiled = true,
      .geometry = {.x = 0, .y = 0, .width = 500, .height = 1039},
      .scheduledWidth = 1000,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 100,
      .winY = 41,
  };
  checkBox(calculatePopupConstraintBox(input), 0, 0, 1000, 1039);
}

UMBRIEL_TEST(floatingViewUsesUsableAreaRelativeToWindow) {
  const PopupConstraintInput input{
      .fullscreen = false,
      .tiled = false,
      .geometry = {.x = 0, .y = 0, .width = 600, .height = 400},
      .scheduledWidth = 0,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 200,
      .winY = 150,
  };
  checkBox(calculatePopupConstraintBox(input), -200, -109, 1920, 1039);
}

UMBRIEL_TEST(scrolledFullscreenViewOffsetsByWindowPosition) {
  const PopupConstraintInput input{
      .fullscreen = true,
      .tiled = true,
      .geometry = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .scheduledWidth = 1920,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 200,
      .winY = 0,
  };
  checkBox(calculatePopupConstraintBox(input), -200, 0, 1920, 1080);
}

UMBRIEL_TEST(csdShadowGeometryOffsetCancelsOut) {
  const PopupConstraintInput fullscreenCsd{
      .fullscreen = true,
      .tiled = false,
      .geometry = {.x = 26, .y = 23, .width = 1920, .height = 1080},
      .scheduledWidth = 1920,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 200,
      .winY = 0,
  };
  checkBox(calculatePopupConstraintBox(fullscreenCsd), -174, 23, 1920, 1080);

  const PopupConstraintInput tiledCsd{
      .fullscreen = false,
      .tiled = true,
      .geometry = {.x = 26, .y = 23, .width = 948, .height = 1000},
      .scheduledWidth = 948,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 0,
      .winY = 41,
  };
  checkBox(calculatePopupConstraintBox(tiledCsd), 26, 23, 948, 1039);

  const PopupConstraintInput floatingCsd{
      .fullscreen = false,
      .tiled = false,
      .geometry = {.x = 26, .y = 23, .width = 600, .height = 400},
      .scheduledWidth = 0,
      .outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080},
      .usableArea = {.x = 0, .y = 41, .width = 1920, .height = 1039},
      .winX = 100,
      .winY = 141,
  };
  checkBox(calculatePopupConstraintBox(floatingCsd), -74, -77, 1920, 1039);
}

int main() { return RUN_TESTS(); }
