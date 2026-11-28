#pragma once

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  struct PopupConstraintInput {
    bool fullscreen = false;
    bool tiled = false;
    wlr_box geometry{};
    int scheduledWidth = 0;
    wlr_box outputBox{};
    wlr_box usableArea{};
    int winX = 0;
    int winY = 0;
  };

  wlr_box calculatePopupConstraintBox(const PopupConstraintInput& input);

} // namespace umbriel
