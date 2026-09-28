#include "output/cursor_plane_pace.h"

#include <cstdint>

namespace umbriel {
  bool cursorPlaneAdvance(CursorPlaneState *state, CursorPlaneState now, CursorPlaneState *previous) {
      *previous = *state;

      const bool changed = state->valid && (state->enabled != now.enabled || state->visible != now.visible ||
                                            state->x != now.x || state->y != now.y || state->width != now.width ||
                                            state->height != now.height || state->hotspot_x != now.hotspot_x ||
                                            state->hotspot_y != now.hotspot_y || state->image != now.image);
      *state = now;
      state->valid = true;

      return changed;
  }

  CursorPlaneDamage cursorPlaneDamageFor(const CursorPlaneState* previous, const CursorPlaneState* now) {
    CursorPlaneDamage result{};

    if (previous->enabled && previous->visible && previous->width > 0 && previous->height > 0) {
      result.has_leave = true;
      result.leave_box = wlr_box{
          .x = static_cast<int32_t>(previous->x - previous->hotspot_x),
          .y = static_cast<int32_t>(previous->y - previous->hotspot_y),
          .width = static_cast<uint32_t>(previous->width),
          .height = static_cast<uint32_t>(previous->height),
      };
    }

    if (now->enabled && now->visible && now->width > 0 && now->height > 0) {
      result.has_enter = true;
      result.enter_box = wlr_box{
          .x = static_cast<int32_t>(now->x - now->hotspot_x),
          .y = static_cast<int32_t>(now->y - now->hotspot_y),
          .width = static_cast<uint32_t>(now->width),
          .height = static_cast<uint32_t>(now->height),
      };
    }

    return result;
  }
} // namespace
