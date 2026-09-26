#include "output/cursor_plane_pace.h"

struct umbriel_cursor_plane_damage umbriel_pace_cursor_plane(
    struct umbriel_cursor_plane_state *state, struct umbriel_cursor_plane_state now, bool capture_needs_pacing,
    int output_width, int output_height
) {
    struct umbriel_cursor_plane_damage result = {0};

    const struct umbriel_cursor_plane_state last = *state;
    *state = now;
    state->valid = true;

    if (!last.valid) {
      return result;
    }

    const bool changed = last.enabled != now.enabled ||last.visible != now.visible || last.x != now.x
        || last.y != now.y || last.width != now.width || last.height != now.height
        || last.hotspot_x != now.hotspot_x || last.hotspot_y != now.hotspot_y || last.image != now.image;

    if (!changed || !capture_needs_pacing) {
      return result;
    }

    result.paced = true;

    if (last.enabled && last.visible && last.width > 0 && last.height > 0) {
        result.has_leave = true;
        result.leave_box = (struct wlr_box) {
            .x = last.x - last.hotspot_x,
            .y = last.y - last.hotspot_y,
            .width = last.width,
            .height = last.height,
        };

        int cx = last.x;
        if (cx < 0) {
			cx = 0;
		}

        const int max_x = output_width > 0 ? output_width - 1 : 0;
        if (cx > max_x) {
          cx = max_x;
        }

        int cy = last.y;
        if (cy < 0) {
            cy = 0;
        }

        const int max_y = output_height > 0 ? output_height - 1 : 0;
        if (cy > max_y) {
            cy = max_y;
        }
        result.has_wake = true;
        result.wake_box = (struct wlr_box) {.x = cx, .y =cy, .width = 1, .height = 1};
    }

    if (now.enabled && now.visible && now.width > 0 && now.height > 0) {
        result.has_enter = true;
        result.enter_box = (struct wlr_box) {
            .x = now.x - now.hotspot_x,
            .y = now.y - now.hotspot_y,
            .width = now.width,
            .height = now.height,
        };
    }

    return result;
}
