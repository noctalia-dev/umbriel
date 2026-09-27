#include "output/cursor_plane_pace.h"

#include "check.h"

#include <cstdint>

namespace {
  const void* imageToken(std::uintptr_t value) { return reinterpret_cast<const void*>(value); }

  umbriel_cursor_plane_state cursorAt(double x, double y, std::uintptr_t image = 1) {
    return umbriel_cursor_plane_state{
        .valid = false,
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

  umbriel_cursor_plane_state hiddenCursor(double x = 0.0, double y = 0.0) {
    return umbriel_cursor_plane_state{
        .valid = false,
        .enabled = true,
        .visible = false,
        .x = x,
        .y = y,
        .width = 24,
        .height = 24,
        .hotspot_x = 0,
        .hotspot_y = 0,
        .image = nullptr,
    };
  }

  // Mirrors Output::paceCursorPlaneTransition: advance the snapshot, then ask
  // the boxes, with the consumer gate in between.
  struct Paced {
    bool paced = false;
    umbriel_cursor_plane_damage damage{};
  };

  Paced pace(umbriel_cursor_plane_state* state, umbriel_cursor_plane_state now, bool captureWantsPacing) {
    umbriel_cursor_plane_state previous;
    if (!umbriel_cursor_plane_advance(state, now, &previous) || !captureWantsPacing) {
      return {};
    }
    return {.paced = true, .damage = umbriel_cursor_plane_damage_for(&previous, &now, 800, 600)};
  }
} // namespace

UMBRIEL_TEST(firstSampleOnlySeedsTheState) {
  umbriel_cursor_plane_state state{};
  CHECK(!pace(&state, cursorAt(10.0, 10.0), true).paced);
  CHECK(state.valid);
}

UMBRIEL_TEST(aStationaryCursorIsNeverDamaged) {
  umbriel_cursor_plane_state state{};
  pace(&state, cursorAt(10.0, 10.0), true);
  CHECK(!pace(&state, cursorAt(10.0, 10.0), true).paced);
}

UMBRIEL_TEST(movingDamagesTheNewBox) {
  umbriel_cursor_plane_state state{};
  pace(&state, cursorAt(10.0, 10.0), true);
  const Paced paced = pace(&state, cursorAt(200.0, 150.0), true);
  CHECK(paced.paced);
  CHECK(paced.damage.has_enter);
  CHECK(paced.damage.enter_box.x == 200 && paced.damage.enter_box.y == 150);
}

UMBRIEL_TEST(leavingDamagesTheExitedBoxAndWakesInBounds) {
  umbriel_cursor_plane_state state{};
  pace(&state, cursorAt(10.0, 10.0), true);
  const Paced paced = pace(&state, hiddenCursor(), true);
  CHECK(paced.paced);
  CHECK(paced.damage.has_leave);
  CHECK(!paced.damage.has_enter);
  CHECK(paced.damage.leave_box.x == 10.0 && paced.damage.leave_box.y == 10.0);
  CHECK(paced.damage.has_wake);
  CHECK(paced.damage.wake_box.x >= 0 && paced.damage.wake_box.x < 800);
  CHECK(paced.damage.wake_box.y >= 0 && paced.damage.wake_box.y < 600);
}

UMBRIEL_TEST(aCursorOffTheOutputStillWakesInsideIt) {
  umbriel_cursor_plane_state state{};
  pace(&state, cursorAt(2000.0, 100.0), true);
  const Paced paced = pace(&state, hiddenCursor(), true);
  CHECK(paced.damage.has_wake);
  CHECK(paced.damage.wake_box.x == 799);
}

UMBRIEL_TEST(anImageSwapAtTheSamePlaceIsATransition) {
  umbriel_cursor_plane_state state{};
  pace(&state, cursorAt(10.0, 10.0, 1), true);
  const Paced paced = pace(&state, cursorAt(10.0, 10.0, 2), true);
  CHECK(paced.paced);
  CHECK(paced.damage.has_enter);
}

UMBRIEL_TEST(aHiddenCursorThatMovesReportsNoBoxes) {
  umbriel_cursor_plane_state state{};
  pace(&state, hiddenCursor(10.0, 10.0), true);
  const Paced paced = pace(&state, hiddenCursor(400.0, 300.0), true);
  CHECK(paced.paced);
  CHECK(!paced.damage.has_leave);
  CHECK(!paced.damage.has_enter);
  CHECK(!paced.damage.has_wake);
}

UMBRIEL_TEST(noConsumerReportsNothingButKeepsTheSampleFresh) {
  umbriel_cursor_plane_state state{};
  CHECK(!pace(&state, cursorAt(10.0, 10.0), false).paced);
  CHECK(!pace(&state, hiddenCursor(10.0, 10.0), false).paced);
  CHECK(!state.visible);
  // The next real transition diffs from the hidden sample, not the first one.
  CHECK(pace(&state, cursorAt(400.0, 300.0), true).paced);
}

UMBRIEL_TEST(advanceHandsBackTheReplacedSample) {
  umbriel_cursor_plane_state state{};
  umbriel_cursor_plane_state previous{};
  CHECK(!umbriel_cursor_plane_advance(&state, cursorAt(10.0, 10.0), &previous));
  CHECK(!previous.valid);
  CHECK(umbriel_cursor_plane_advance(&state, cursorAt(400.0, 300.0), &previous));
  CHECK(previous.valid);
  CHECK(previous.x == 10.0 && previous.y == 10.0);
}

int main() { return RUN_TESTS(); }
