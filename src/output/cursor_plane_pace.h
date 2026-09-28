#pragma once

#include <stdbool.h>
#include <wlr/util/box.h>

namespace umbriel {

  struct CursorPlaneState {
    // Output-side only: false until the first umbriel_cursor_plane_advance seeds
    // the snapshot. A `now` sample's valid field is never read.
    bool valid;
    bool enabled;
    bool visible;
    double x, y;
    int width, height;
    int hotspot_x, hotspot_y;
    const void* image;
  };

  struct CursorPlaneDamage {
    struct wlr_box leave_box;
    bool has_leave;
    struct wlr_box enter_box;
    bool has_enter;
  };

  // Copies `state` to `previous`, stores `now`, and reports whether the two
  // differ. The seeding call and an unchanged sample both report false. Reads no
  // compositor state, so it stays usable without the runtime.
  bool cursorPlaneAdvance(CursorPlaneState* state, CursorPlaneState now, CursorPlaneState* previous)

      // Pure: the boxes to damage for a transition cursorPlaneAdvance has already
      // reported -- the box the cursor left and the box it entered.
      CursorPlaneDamage cursorPlaneDamageFor(const CursorPlaneState* previous, const CursorPlaneState* now)

} // namespace umbriel
