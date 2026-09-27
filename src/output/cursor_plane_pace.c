#include "output/cursor_plane_pace.h"

// Clamps a cursor coordinate into [0, extent - 1], an unset extent clamps to 0.
static int umbriel_cursor_plane_clamp(double value, int extent) {
  const int max = extent > 0 ? extent - 1 : 0;
  const int coordinate = (int)value;
  return coordinate < 0 ? 0 : (coordinate > max ? max : coordinate);
}

bool umbriel_cursor_plane_advance(
    struct umbriel_cursor_plane_state* state, struct umbriel_cursor_plane_state now,
    struct umbriel_cursor_plane_state* previous
) {
  *previous = *state;

  const bool changed = state->valid && (state->enabled != now.enabled || state->visible != now.visible ||
                                    state->x != now.x || state->y != now.y || state->width != now.width ||
                                    state->height != now.height || state->hotspot_x != now.hotspot_x ||
                                    state->hotspot_y != now.hotspot_y || state->image != now.image);

  *state = now;
  state->valid = true;

  return changed;
}

struct umbriel_cursor_plane_damage umbriel_cursor_plane_damage_for(
    const struct umbriel_cursor_plane_state* previous, const struct umbriel_cursor_plane_state* now,
    int output_width, int output_height
) {
  struct umbriel_cursor_plane_damage result = {0};

  if (previous->enabled && previous->visible && previous->width > 0 && previous->height > 0) {
    result.has_leave = true;
    result.leave_box = (struct wlr_box) {
      .x = previous->x - previous->hotspot_x,
      .y = previous->y - previous->hotspot_y,
      .width = previous->width,
      .height = previous->height,
    };

    result.has_wake = true;
    // A cursor already off the output has an empty leave box, so the 1x1 wakeup
    // is clamped into the output rather than discarded with it.
    result.wake_box = (struct wlr_box) {
      .x = umbriel_cursor_plane_clamp(previous->x, output_width),
      .y = umbriel_cursor_plane_clamp(previous->y, output_height),
      .width = 1,
      .height = 1,
    };
  }

  if (now->enabled && now->visible && now->width > 0 && now->height > 0) {
    result.has_enter = true;
    result.enter_box = (struct wlr_box) {
      .x = now->x - now->hotspot_x,
      .y = now->y - now->hotspot_y,
      .width = now->width,
      .height = now->height,
    };
  }

  return result;
 }
