#include "view/drag_physics.h"

#include "check.h"

#include <array>
#include <cmath>
#include <limits>

using umbriel::DragPhysics;

namespace {
  // Ticks round(seconds / step) times.
  void run(DragPhysics& physics, double seconds, double step) {
    const auto steps = static_cast<int>(std::lround(seconds / step));
    for (int i = 0; i < steps; ++i) {
      physics.tick(step);
    }
  }
  // No folding: across sampled rows the drawn x grows strictly with u, and down sampled columns the drawn y grows
  // strictly with v.
  bool unfolded(const DragPhysics& physics, float width, float height) {
    constexpr int kSamples = 64;
    for (int line = 0; line <= 4; ++line) {
      const float across = static_cast<float>(line) / 4;
      float previousX = -std::numeric_limits<float>::infinity();
      float previousY = -std::numeric_limits<float>::infinity();
      for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / kSamples;
        const float x = t * width + physics.displacementAt(t, across)[0];
        const float y = t * height + physics.displacementAt(across, t)[1];
        if (x <= previousX || y <= previousY) {
          return false;
        }
        previousX = x;
        previousY = y;
      }
    }
    return true;
  }
  // The inverse lookup's contraction: over a grid of (u, v) steps, each axis's displacement divided by that axis's
  // extent changes by at most kContraction times the larger step.
  bool contracts(const DragPhysics& physics, float width, float height) {
    constexpr int kSamples = 32;
    constexpr float kStep = 1.0F / kSamples;
    const float extent[2] = {width, height};
    for (int j = 0; j <= kSamples; ++j) {
      for (int i = 0; i <= kSamples; ++i) {
        const float u = static_cast<float>(i) * kStep, v = static_cast<float>(j) * kStep;
        const auto here = physics.displacementAt(u, v);
        for (const auto& [du, dv] :
             {std::array{kStep, 0.0F}, std::array{0.0F, kStep}, std::array{kStep, kStep}, std::array{kStep, -kStep}}) {
          if (u + du > 1.0F + 1e-6F || v + dv > 1.0F + 1e-6F || v + dv < -1e-6F) {
            continue;
          }
          const auto there = physics.displacementAt(u + du, v + dv);
          for (int axis = 0; axis < 2; ++axis) {
            if (std::abs(there[axis] - here[axis]) / extent[axis] > DragPhysics::kContraction * kStep + 1e-5F) {
              return false;
            }
          }
        }
      }
    }
    return true;
  }
} // namespace

UMBRIEL_TEST(grabPointStaysPinnedWhileTheSheetMoves) {
  DragPhysics physics;
  physics.begin(400, 300, 0.25F, 0.8F, 1);
  for (int i = 0; i < 10; ++i) {
    physics.move(12, -4);
    physics.tick(1.0 / 60);
  }
  CHECK(physics.active());
  const auto pin = physics.displacementAt(0.25F, 0.8F);
  CHECK(std::abs(pin[0]) < 0.05F && std::abs(pin[1]) < 0.05F);
  CHECK(physics.maxDisplacement() > 5.0F);
}

UMBRIEL_TEST(motionTrailsOppositeToThePointer) {
  DragPhysics physics;
  // Off-centre but not a grid vertex: at an exact corner (0, 0) the grab weight there is 1 and the pin
  // alone forces that corner's displacement to zero, so the check below would hold by construction.
  physics.begin(400, 300, 0.1F, 0.1F, 1);
  for (int i = 0; i < 6; ++i) {
    physics.move(30, 0);
    physics.tick(1.0 / 60);
  }
  const auto sheet = physics.normalizedDisplacement();
  // The far corner (index 15) lags behind the motion: negative x displacement, clearly more than the near corner.
  CHECK(sheet[15][0] < -0.02F);
  CHECK(std::abs(sheet[0][0]) < 0.05F);
}

UMBRIEL_TEST(displacementStaysBoundedAndTheSheetNeverFoldsUnderExtremeShaking) {
  DragPhysics physics;
  // Off-centre grab, so the two displacement components differ.
  physics.begin(120, 70, 0.25F, 0.5F, 1);
  for (int i = 0; i < 400; ++i) {
    physics.move((i % 2 == 0 ? 1 : -1) * 900.0F, (i % 3 == 0 ? 1 : -1) * 700.0F);
    physics.tick(1.0 / 240);
    const auto sheet = physics.normalizedDisplacement();
    for (const auto& point : sheet) {
      CHECK(std::isfinite(point[0]) && std::isfinite(point[1]));
    }
    CHECK(physics.maxDisplacement() <= physics.displacementBound() + 1e-3F);
    CHECK(unfolded(physics, 120, 70));
    CHECK(contracts(physics, 120, 70));
  }
}

UMBRIEL_TEST(aShrinkingRetargetOfADisplacedSheetStaysAContraction) {
  // The mirrored run (flip = -1) puts the fold along the anti-diagonal.
  for (const float flip : {1.0F, -1.0F}) {
    const auto row = [flip](float v) { return flip > 0 ? v : 1.0F - v; };
    DragPhysics physics;
    physics.begin(800, 600, 0.0F, row(0.0F), 1);
    for (int i = 0; i < 4; ++i) {
      physics.move((i % 2 == 0 ? 1 : -1) * 400.0F, flip * (i % 3 == 0 ? 1 : -1) * 300.0F);
      physics.tick(1.0 / 240);
    }
    CHECK(physics.maxDisplacement() > 20.0F);
    CHECK(contracts(physics, 800, 600));
    // The window halves under the pointer and the grab lands on the far corner, re-pinning the displaced sheet there.
    physics.resize(400, 300, 1.0F, row(0.9F));
    CHECK(contracts(physics, 400, 300));
    CHECK(unfolded(physics, 400, 300));
    physics.move(-60, flip * 40);
    physics.tick(1.0 / 240);
    CHECK(contracts(physics, 400, 300));
  }
}

UMBRIEL_TEST(velocityStaysBoundedWhenAResizeScalesAFastSheet) {
  DragPhysics physics;
  physics.begin(1000, 1000, 0.5F, 0.5F, 1);
  physics.move(-30, -30);
  physics.tick(1.0 / 60);
  physics.release();
  run(physics, 225.0 / 240, 1.0 / 240);
  // Late in the settle the masses move fast relative to how far they are displaced, so growing the window until
  // the displacement nears its bound carries the speed far past its own.
  CHECK(physics.active());
  const float scale = physics.displacementBound() * 0.75F / physics.maxDisplacement();
  CHECK(physics.maxVelocity() * scale > DragPhysics::kMaxVelocity);
  physics.resize(1000 * scale, 1000 * scale, 0.5F, 0.5F);
  CHECK(physics.maxVelocity() <= DragPhysics::kMaxVelocity * (1 + 1e-5F));
  CHECK(contracts(physics, 1000 * scale, 1000 * scale));
}

UMBRIEL_TEST(reGrabbingWhileSettlingKeepsTheSheetAndItsTransition) {
  DragPhysics physics;
  physics.begin(400, 300, 0.2F, 0.2F, 7);
  for (int i = 0; i < 6; ++i) {
    physics.move(30, 10);
    physics.tick(1.0 / 60);
  }
  physics.release();
  physics.tick(1.0 / 60);
  CHECK(physics.active());
  physics.begin(400, 300, 0.7F, 0.6F, 8);
  CHECK(physics.active() && physics.grabbed());
  CHECK_EQ(physics.transitionId(), uint64_t{7});
  CHECK(physics.maxDisplacement() > 1.0F);
  const auto pin = physics.displacementAt(0.7F, 0.6F);
  CHECK(std::abs(pin[0]) < 0.05F && std::abs(pin[1]) < 0.05F);
  CHECK(contracts(physics, 400, 300));
}

UMBRIEL_TEST(settlesAfterReleaseAndWhileHeldStill) {
  DragPhysics physics;
  physics.begin(400, 300, 0.5F, 0.5F, 1);
  physics.move(60, 20);
  run(physics, 0.05, 1.0 / 240);
  CHECK(physics.active());
  run(physics, 2.5, 1.0 / 120); // still held, no motion
  CHECK(!physics.active());
  CHECK(physics.maxDisplacement() == 0.0F);

  physics.begin(400, 300, 0.5F, 0.5F, 2);
  physics.move(60, 20);
  physics.release();
  CHECK(physics.active());
  run(physics, 2.5, 1.0 / 120);
  CHECK(!physics.active());
  const auto sheet = physics.normalizedDisplacement();
  for (const auto& point : sheet) {
    CHECK(point[0] == 0.0F && point[1] == 0.0F);
  }
}

UMBRIEL_TEST(integrationIsIndependentOfTheCallerFrameRate) {
  DragPhysics slow;
  DragPhysics fast;
  slow.begin(400, 300, 0.3F, 0.3F, 1);
  fast.begin(400, 300, 0.3F, 0.3F, 2);
  slow.move(40, 10);
  fast.move(40, 10);
  run(slow, 0.5, 1.0 / 60);
  run(fast, 0.5, 1.0 / 120);
  const auto a = slow.normalizedDisplacement();
  const auto b = fast.normalizedDisplacement();
  for (int i = 0; i < DragPhysics::kPoints; ++i) {
    CHECK(std::abs(a[i][0] - b[i][0]) < 1e-4F);
    CHECK(std::abs(a[i][1] - b[i][1]) < 1e-4F);
  }
}

UMBRIEL_TEST(longPausesSettleImmediatelyAndTransitionIdPassesThrough) {
  DragPhysics physics;
  physics.begin(400, 300, 0.5F, 0.5F, 42);
  CHECK_EQ(physics.transitionId(), uint64_t{42});
  physics.move(60, 20);
  CHECK(physics.active());
  CHECK(!physics.tick(2.0));
  CHECK(!physics.active());
  physics.begin(400, 300, 0.5F, 0.5F, 43);
  CHECK_EQ(physics.transitionId(), uint64_t{43});
}

UMBRIEL_TEST(tickIgnoresNonPositiveOrNonFiniteSeconds) {
  DragPhysics neverBegun;
  CHECK(!neverBegun.tick(1.0));

  DragPhysics physics;
  physics.begin(400, 300, 0.5F, 0.5F, 1);
  physics.move(60, 20);
  CHECK(physics.active());
  const auto before = physics.normalizedDisplacement();
  CHECK(physics.tick(0.0));
  CHECK(physics.tick(-1.0));
  CHECK(physics.tick(std::numeric_limits<double>::quiet_NaN()));
  CHECK(physics.tick(std::numeric_limits<double>::infinity()));
  CHECK(physics.active());
  const auto after = physics.normalizedDisplacement();
  for (int i = 0; i < DragPhysics::kPoints; ++i) {
    CHECK(before[i][0] == after[i][0]);
    CHECK(before[i][1] == after[i][1]);
  }
}

UMBRIEL_TEST(aBorderedWindowKeepsTheGrabbedCornerUnderThePointer) {
  // The sheet spans the box the drag slot draws over: a 400x300 window inside a 24 px ring (border plus padding).
  const float boxX = -24, boxY = -24, boxWidth = 448, boxHeight = 348;
  // The pointer holds the window geometry's top-left corner, window-local (0, 0).
  const auto grab = DragPhysics::grabIn(boxX, boxY, boxWidth, boxHeight, 0.0, 0.0);
  DragPhysics physics;
  physics.begin(boxWidth, boxHeight, grab[0], grab[1], 1);
  for (int i = 0; i < 8; ++i) {
    physics.move(25, 10);
    physics.tick(1.0 / 60);
  }
  CHECK(physics.maxDisplacement() > 5.0F);
  // Drawn where the shader puts it: the box origin, plus the grab's fraction of the box, plus its displacement.
  auto at = physics.displacementAt(grab[0], grab[1]);
  CHECK(std::abs(boxX + grab[0] * boxWidth + at[0]) < 0.05F);
  CHECK(std::abs(boxY + grab[1] * boxHeight + at[1]) < 0.05F);

  // Resized with the grab at the same fraction, the deformation keeps its shape relative to the window.
  const auto before = physics.normalizedDisplacement();
  physics.resize(boxWidth * 1.5F, boxHeight * 1.25F, grab[0], grab[1]);
  const auto after = physics.normalizedDisplacement();
  for (int i = 0; i < DragPhysics::kPoints; ++i) {
    CHECK(std::abs(before[i][0] - after[i][0]) < 1e-5F && std::abs(before[i][1] - after[i][1]) < 1e-5F);
  }

  // A retarget grows the window under the same ring and keeps the pointer on the geometry's corner.
  const auto moved = DragPhysics::grabIn(boxX, boxY, 648, 448, 0.0, 0.0);
  physics.resize(648, 448, moved[0], moved[1]);
  physics.move(20, 0);
  physics.tick(1.0 / 60);
  at = physics.displacementAt(moved[0], moved[1]);
  CHECK(std::abs(boxX + moved[0] * 648 + at[0]) < 0.05F);
  CHECK(std::abs(boxY + moved[1] * 448 + at[1]) < 0.05F);
  CHECK(physics.maxDisplacement() <= physics.displacementBound() + 1e-3F);
  CHECK(contracts(physics, 648, 448));
}

int main() { return RUN_TESTS(); }
