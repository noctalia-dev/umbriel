#include "view/popup_constraint.h"

namespace umbriel {

  wlr_box calculatePopupConstraintBox(const PopupConstraintInput& input) {
    // Fullscreen views span the full output over panels; ignore tile width clamping.
    if (input.fullscreen) {
      return wlr_box{
          .x = input.outputBox.x - input.winX + input.geometry.x,
          .y = input.outputBox.y - input.winY + input.geometry.y,
          .width = input.outputBox.width,
          .height = input.outputBox.height,
      };
    }

    // Tiled views constrain horizontally to the tile so edge menus flip inward, and vertically to usableArea.
    // Box is in root toplevel surface coordinates (xdg scene root sits at geometry, not surface origin).
    const int tileWidth = input.scheduledWidth > 0 ? input.scheduledWidth : input.geometry.width;
    const int width = (input.tiled && tileWidth > 0) ? tileWidth : input.usableArea.width;
    const int x =
        (input.tiled && tileWidth > 0) ? input.geometry.x : input.usableArea.x - input.winX + input.geometry.x;

    return wlr_box{
        .x = x,
        .y = input.usableArea.y - input.winY + input.geometry.y,
        .width = width,
        .height = input.usableArea.height,
    };
  }

} // namespace umbriel
