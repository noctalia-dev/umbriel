#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <wlr/util/box.h>

struct umbriel_cursor_plane_state {
  bool valid;
  bool enabled;
  bool visible;
  double x, y;
  int width, height;
  int hotspot_x, hotspot_y;
  const void* image;
};

struct umbriel_cursor_plane_damage {
  bool paced;
  struct wlr_box leave_box;
  bool has_leave;
  struct wlr_box enter_box;
  bool has_enter;
  struct wlr_box wake_box;
  bool has_wake;
};

// Pure decision function: diffs `*state` against `now`, updates `*state` in
// place, and reports what should be damaged. `capture_needs_pacing` and
// `output_width`/`output_height` (for clamping the leave-off-output wake
// box) are passed in rather than read from a live Output.
struct umbriel_cursor_plane_damage umbriel_pace_cursor_plane(
    struct umbriel_cursor_plane_state* state, struct umbriel_cursor_plane_state now, bool capture_needs_pacing,
    int output_width, int output_height
);

#ifdef __cplusplus
}
#endif
