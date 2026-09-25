#include "scene/tab_bar_geometry.h"

#include "check.h"
#include "layout/layout.h"

#include <cstddef>

using umbriel::tabIndexAt;
using umbriel::TabSlot;
using umbriel::tabSlot;

UMBRIEL_TEST(slotsTileTheBarExactlyWithTheRemainderUpFront) {
  constexpr int kWidth = 100;
  constexpr size_t kCount = 3;
  int next = 0;
  for (size_t index = 0; index < kCount; ++index) {
    const TabSlot slot = tabSlot(kWidth, kCount, index);
    CHECK_EQ(slot.x, next);
    next += slot.width;
  }
  CHECK_EQ(next, kWidth);
  CHECK_EQ(tabSlot(kWidth, kCount, 0).width, 34);
  CHECK_EQ(tabSlot(kWidth, kCount, 2).width, 33);
  CHECK_EQ(tabSlot(kWidth, kCount, 3).width, 0);
  CHECK_EQ(tabSlot(kWidth, 0, 0).width, 0);
}

UMBRIEL_TEST(hitTestingAgreesWithTheSlots) {
  constexpr int kWidth = 100;
  constexpr size_t kCount = 3;
  for (size_t index = 0; index < kCount; ++index) {
    const TabSlot slot = tabSlot(kWidth, kCount, index);
    CHECK_EQ(tabIndexAt(kWidth, kCount, slot.x), static_cast<int>(index));
    CHECK_EQ(tabIndexAt(kWidth, kCount, slot.x + slot.width - 0.5), static_cast<int>(index));
  }
  CHECK_EQ(tabIndexAt(kWidth, kCount, -0.5), -1);
  CHECK_EQ(tabIndexAt(kWidth, kCount, kWidth), -1);
  CHECK_EQ(tabIndexAt(kWidth, 0, 10.0), -1);
}

UMBRIEL_TEST(erasingARowKeepsTheSelectionOnItsViewOrItsPredecessor) {
  size_t active = 2;
  umbriel::tabRowErased(active, 0, 3); // an earlier row leaves: same view, one index down
  CHECK_EQ(active, size_t{1});
  umbriel::tabRowErased(active, 1, 2); // the shown row leaves: its predecessor
  CHECK_EQ(active, size_t{0});
  umbriel::tabRowErased(active, 0, 1); // the first row leaves while shown: its successor slides in
  CHECK_EQ(active, size_t{0});
  umbriel::tabRowErased(active, 0, 0);
  CHECK_EQ(active, size_t{0});
}

UMBRIEL_TEST(insertingAndSwappingRowsFollowTheShownView) {
  size_t active = 1;
  umbriel::tabRowInserted(active, 1, 3); // at the shown row: it moves down one
  CHECK_EQ(active, size_t{2});
  umbriel::tabRowInserted(active, 3, 4); // after it: unchanged
  CHECK_EQ(active, size_t{2});
  umbriel::tabRowsSwapped(active, 2, 0);
  CHECK_EQ(active, size_t{0});
  umbriel::tabRowsSwapped(active, 1, 3);
  CHECK_EQ(active, size_t{0});
}

int main() { return RUN_TESTS(); }
