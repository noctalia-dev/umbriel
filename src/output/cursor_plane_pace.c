#include "output/cursor_plane_pace.h"

static bool umbriel_cursor_plane_changed(
    const struct umbriel_cursor_plane_state* last, const struct umbriel_cursor_plane_state* now
) {
  return last->enabled != now->enabled || last->visible != now->visible || last->x != now->x ||
         last->y != now->y || last->width != now->width || last->height != now->height ||
         last->hotspot_x != now->hotspot_x || last->hotspot_y != now->hotspot_y || last->image != now->image;
}

bool umbriel_cursor_plane_advance(
    struct umbriel_cursor_plane_state* state, struct umbriel_cursor_plane_state now,
    struct umbriel_cursor_plane_state* previous
) {
  if (previous != NULL) {
    *previous = *state;
  }

  const bool seeded = state->valid;
  const bool changed = seeded && umbriel_cursor_plane_changed(state, &now);

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

    int cx = previous->x;
    if (cx < 0) {
      cx = 0;
    }

    const int max_x = output_width > 0 ? output_width - 1 : 0;
    if (cx > max_x) {
      cx = max_x;
    }

    int cy = previous->y;
    if (cy < 0) {
      cy = 0;
    }

    const int max_y = output_height > 0 ? output_height - 1 : 0;
    if (cy > max_y) {
      cy = max_y;
    }

    result.has_wake = true;
    result.wake_box = (struct wlr_box) {.x = cx, .y = cy, .width = 1, .height = 1};
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
