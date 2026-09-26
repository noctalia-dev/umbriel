#include "output/cursor_plane_pace.h"

#include "check.h"

#include <cstdint>

namespace {
  const void* imageToken(std::uintptr_t value) { return reinterpret_cast<const void*>(value); }

  umbriel_cursor_plane_state cursorAt(double x, double y, std::uintptr_t image = 1) {
    return umbriel_cursor_plane_state{
        .valid = true,
        .enabled = true,
        .visible = true,
        .x = x,
        .y = y,
        .width = 24,
        .height = 24,
        .hotspot_x = 0,
        .hotspot_y = 0,
        .image = imageToken(image),
    };
  }

  umbriel_cursor_plane_state hiddenCursor() {
    return umbriel_cursor_plane_state{
        .valid = true,
        .enabled = true,
        .visible = false,
        .x = 0.0,
        .y = 0.0,
        .width = 24,
        .height = 24,
        .hotspot_x = 0,
        .hotspot_y = 0,
        .image = nullptr,
    };
  }
} // namespace

UMBRIEL_TEST(firstSampleOnlySeedsTheState) {
  umbriel_cursor_plane_state state{};
  CHECK(!umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), true, 800, 600).paced);
  CHECK(state.valid);
}

UMBRIEL_TEST(movingDamagesTheNewBox) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), true, 800, 600);
  const umbriel_cursor_plane_damage damage = umbriel_pace_cursor_plane(&state, cursorAt(200.0, 150.0), true, 800, 600);
  CHECK(damage.paced);
  CHECK(damage.has_enter);
  CHECK(damage.enter_box.x == 200 && damage.enter_box.y == 150);
}

UMBRIEL_TEST(leavingDamagesTheExitedBoxAndWakesInBounds) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), true, 800, 600);
  const umbriel_cursor_plane_damage damage = umbriel_pace_cursor_plane(&state, hiddenCursor(), true, 800, 600);
  CHECK(damage.paced);
  CHECK(damage.has_leave);
  CHECK(!damage.has_enter);
  CHECK(damage.leave_box.x == 10.0 && damage.leave_box.y == 10.0);
  CHECK(damage.has_wake);
  CHECK(damage.wake_box.x >= 0 && damage.wake_box.x < 800);
  CHECK(damage.wake_box.y >= 0 && damage.wake_box.y < 600);
}

UMBRIEL_TEST(aCursorOffTheOutputStillWakesInsideIt) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(2000.0, 100.0), true, 800, 600);
  const umbriel_cursor_plane_damage damage = umbriel_pace_cursor_plane(&state, hiddenCursor(), true, 800, 600);
  CHECK(damage.has_wake);
  CHECK(damage.wake_box.x == 799);
}

UMBRIEL_TEST(anImageSwapAtTheSamePlaceIsATransition) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0, 1), true, 800, 600);
  const umbriel_cursor_plane_damage damage = umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0, 2), true, 800, 600);
  CHECK(damage.paced);
  CHECK(damage.has_enter);
}

UMBRIEL_TEST(aStationaryCursorIsNeverDamaged) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), true, 800, 600);
  CHECK(!umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), true, 800, 600).paced);
}

UMBRIEL_TEST(noConsumerReportsNothingButKeepsTheSampleFresh) {
  umbriel_cursor_plane_state state{};
  umbriel_pace_cursor_plane(&state, cursorAt(10.0, 10.0), false, 800, 600);
  CHECK(!umbriel_pace_cursor_plane(&state, hiddenCursor(), false, 800, 600).paced);
  CHECK(!state.visible);
}

int main() { return RUN_TESTS(); }
