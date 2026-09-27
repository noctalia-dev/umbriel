#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <wlr/util/box.h>

struct umbriel_cursor_plane_state {
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

struct umbriel_cursor_plane_damage {
  struct wlr_box leave_box;
  bool has_leave;
  struct wlr_box enter_box;
  bool has_enter;
  struct wlr_box wake_box;
  bool has_wake;
};

// `previous` (which may be NULL when the caller has no use for it), and reports
// whether the two differ. The seeding call and an unchanged sample both report
// false. Reads no compositor state, so it stays usable without the runtime.
bool umbriel_cursor_plane_advance(
    struct umbriel_cursor_plane_state* state, struct umbriel_cursor_plane_state now,
    struct umbriel_cursor_plane_state* previous
);

// Pure: the boxes to damage for a transition umbriel_cursor_plane_advance has
// already reported -- the box the cursor left, the box it entered, and a 1x1
// in-bounds wakeup covering a cursor that was already off the output.
// `output_width`/`output_height` clamp that wakeup.
struct umbriel_cursor_plane_damage umbriel_cursor_plane_damage_for(
    const struct umbriel_cursor_plane_state* previous, const struct umbriel_cursor_plane_state* now, int output_width,
    int output_height
);

#ifdef __cplusplus
}
#endif
