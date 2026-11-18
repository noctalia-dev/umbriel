#include "view/presentation.h"

#include "check.h"

#include <array>
#include <cmath>

// clang-format off
#include "wlr.h"
// clang-format on

namespace {
  constexpr double kScale = 1.5;
  constexpr double kOriginX = 101.0;
  constexpr double kOriginY = -37.0;
  constexpr double kStartMsec = 1000.0;
  constexpr umbriel::AnimationCurve kLinear{.easing = umbriel::Easing::Linear};

  // Supply the same endpoint contract as View: sample origins before advancing
  // position, then replace the endpoint on the tick that presents its logical rest.
  umbriel::ViewPresentation::PositionOrigins
  advancePosition(umbriel::AnimatedValue& position, double origin, double nowMsec) {
    const double shown = position.pixelAlignedCurrent(kScale, origin);
    const double target =
        position.animating() ? origin + std::round((position.target() - origin) * kScale) / kScale : shown;
    umbriel::ViewPresentation::PositionOrigins origins{
        origin - position.current(),
        origin - position.target(),
        origin - shown,
        origin - target,
    };
    position.tick(nowMsec);
    if (!position.animating()) {
      origins.to = origin - position.current();
      origins.presentedTo = origins.to;
    }
    origins.animating = position.animating();
    return origins;
  }

  struct CoordinatedPresentation {
    umbriel::ViewPresentation size;
    umbriel::AnimatedValue x{kOriginX + 10.0};
    umbriel::AnimatedValue y{kOriginY + 14.0};

    CoordinatedPresentation(int moveXMs, int moveYMs) {
      size.setSize(101, 81);
      size.animateTo(103, 83, 100, kLinear);
      x.retarget(kOriginX + 11.0, moveXMs, kLinear);
      y.retarget(kOriginY + 15.0, moveYMs, kLinear);
    }

    bool tick(double elapsedMsec) {
      const double now = kStartMsec + elapsedMsec;
      const auto originsX = advancePosition(x, kOriginX, now);
      const auto originsY = advancePosition(y, kOriginY, now);
      return size.tick(now, kScale, originsX.from, originsY.from, std::nullopt, std::nullopt, originsX, originsY);
    }

    std::array<long, 4> edges() {
      const double left = x.pixelAlignedCurrent(kScale, kOriginX) - kOriginX;
      const double top = y.pixelAlignedCurrent(kScale, kOriginY) - kOriginY;
      return {
          std::lround(left * kScale),
          std::lround(top * kScale),
          std::lround((left + size.fractionalWidth()) * kScale),
          std::lround((top + size.fractionalHeight()) * kScale),
      };
    }

    void checkRest(int width = 103, int height = 83) {
      CHECK(!x.animating());
      CHECK(!y.animating());
      CHECK(!size.animating());
      CHECK_EQ(x.current(), kOriginX + 11.0);
      CHECK_EQ(y.current(), kOriginY + 15.0);
      CHECK_EQ(size.fractionalWidth(), width);
      CHECK_EQ(size.fractionalHeight(), height);
      const auto pixels = edges();
      CHECK_EQ(pixels[0], 17);
      CHECK_EQ(pixels[1], 23);
      CHECK_EQ(pixels[2], std::lround((11.0 + width) * kScale));
      CHECK_EQ(pixels[3], std::lround((15.0 + height) * kScale));
    }
  };
} // namespace

// A fullscreen client whose buffer does not match the output is centered rather than scaled, and that centering lives
// in the scene node's position. The wlroots xdg scene helper rewrites that position to (-geometry.x, -geometry.y) on
// every commit, so the offset has to be re-applied afterwards or an oversized fullscreen buffer drifts to the top left
// on the next frame the client draws.
UMBRIEL_TEST(fullscreenCenteringSurvivesSceneReconfiguration) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }
  wlr_scene_tree* surfaceTree = wlr_scene_tree_create(&scene->tree);
  CHECK(surfaceTree != nullptr);
  if (surfaceTree == nullptr) {
    wlr_scene_node_destroy(&scene->tree.node);
    return;
  }

  umbriel::ViewPresentation presentation;
  // Non-zero geometry origin, as a client with CSD shadows commits: the node
  // position is the centering offset minus that origin, not the offset alone.
  const wlr_box geometry{40, 30, 1920, 1080};
  presentation.updateFullscreen(true, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(presentation.offsetX(), 320);
  CHECK_EQ(presentation.offsetY(), 180);
  CHECK_EQ(surfaceTree->node.x, 280);
  CHECK_EQ(surfaceTree->node.y, 150);

  // What the scene helper does on a commit. Re-running the fullscreen update is
  // what puts the centering back.
  wlr_scene_node_set_position(&surfaceTree->node, -geometry.x, -geometry.y);
  presentation.updateFullscreen(true, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(surfaceTree->node.x, 280);
  CHECK_EQ(surfaceTree->node.y, 150);

  // Leaving fullscreen drops the centering, otherwise the tiled view renders
  // offset by half a size difference that no longer applies.
  presentation.updateFullscreen(false, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(presentation.offsetX(), 0);
  CHECK_EQ(presentation.offsetY(), 0);
  CHECK_EQ(surfaceTree->node.x, -40);
  CHECK_EQ(surfaceTree->node.y, -30);

  wlr_scene_node_destroy(&scene->tree.node);
}

UMBRIEL_TEST(fullscreenResizeReachesTheExactOutputEdgeBeforeCompletion) {
  constexpr double scale = 1.5;
  for (int logicalWidth : {1706, 1707}) {
    umbriel::ViewPresentation presentation;
    presentation.setSize(logicalWidth - 100, 967);
    presentation.animateTo(logicalWidth, 1067, 100, {.easing = umbriel::Easing::Linear});
    CHECK(presentation.tick(1000, scale, 0, 0, 2560, 1600));
    CHECK(presentation.tick(1099.9, scale, -0.2, -0.3, 2560, 1600));
    CHECK_EQ(std::lround((presentation.fractionalWidth() + 0.2) * scale), 2560);
    CHECK_EQ(std::lround((presentation.fractionalHeight() + 0.3) * scale), 1600);
    CHECK(presentation.tick(1100, scale, 0, 0, 2560, 1600));
    CHECK(!presentation.animating());
    CHECK_EQ(presentation.fractionalWidth(), logicalWidth);
    CHECK_EQ(presentation.fractionalHeight(), 1067);
  }
}

UMBRIEL_TEST(coordinatedMoveAndResizeKeepPhysicalEdgesAcrossCompletion) {
  // Move first, simultaneous completion, and resize first. The half-pixel
  // endpoints make releasing an aligned size early observable at the far edge.
  for (int moveMs : {50, 100, 200}) {
    CoordinatedPresentation presentation(moveMs, moveMs);
    CHECK(presentation.tick(0));
    auto previous = presentation.edges();
    CHECK_EQ(previous[0], 15);
    CHECK_EQ(previous[1], 21);
    CHECK_EQ(previous[2], 167);
    CHECK_EQ(previous[3], 143);
    for (double completion : {50.0, 100.0, 200.0}) {
      presentation.tick(completion - 0.01);
      const auto before = presentation.edges();
      for (size_t edge = 0; edge < before.size(); ++edge) {
        CHECK(before[edge] >= previous[edge]);
      }
      presentation.tick(completion);
      const auto atCompletion = presentation.edges();
      if (completion == 100 || completion == moveMs) {
        CHECK(atCompletion == before);
      } else {
        for (size_t edge = 0; edge < before.size(); ++edge) {
          CHECK(atCompletion[edge] >= before[edge]);
        }
      }
      presentation.tick(completion + 0.01);
      const auto after = presentation.edges();
      if (completion == 100 || completion == moveMs) {
        CHECK(after == atCompletion);
      } else {
        for (size_t edge = 0; edge < after.size(); ++edge) {
          CHECK(after[edge] >= atCompletion[edge]);
        }
      }
      previous = after;

      if (completion == 50 && moveMs == 50) {
        CHECK(!presentation.x.animating());
        CHECK(presentation.size.animating());
      }
      if (completion == 100 && moveMs == 200) {
        CHECK(!presentation.size.animation().animating());
        CHECK(presentation.size.animating());
        CHECK(presentation.x.animating());
        CHECK(std::abs(presentation.size.fractionalWidth() - 103.0) > 0.1);
        const double heldWidth = presentation.size.fractionalWidth();
        const double heldHeight = presentation.size.fractionalHeight();
        presentation.size.track(103, 83);
        CHECK_EQ(presentation.size.fractionalWidth(), heldWidth);
        CHECK_EQ(presentation.size.fractionalHeight(), heldHeight);
      }
    }
    presentation.checkRest();
    CHECK(!presentation.tick(201));
  }
}

UMBRIEL_TEST(coordinatedResizeHandlesDifferentPositionCompletionOnEachAxis) {
  for (bool swapAxes : {false, true}) {
    CoordinatedPresentation presentation(swapAxes ? 200 : 50, swapAxes ? 50 : 200);
    CHECK(presentation.tick(0));
    presentation.tick(99.99);
    const auto beforeResize = presentation.edges();
    CHECK(presentation.tick(100));
    CHECK(presentation.edges() == beforeResize);
    CHECK(!presentation.size.animation().animating());
    CHECK(presentation.size.animating());
    CHECK_EQ(presentation.x.animating(), swapAxes);
    CHECK_EQ(presentation.y.animating(), !swapAxes);
    CHECK_NEAR(presentation.size.fractionalWidth(), swapAxes ? 102.66666666666667 : 103.0, 1e-10);
    CHECK_NEAR(presentation.size.fractionalHeight(), swapAxes ? 83.0 : 82.66666666666667, 1e-10);

    presentation.tick(199.99);
    const auto beforeMove = presentation.edges();
    CHECK(presentation.tick(200));
    CHECK(presentation.edges() == beforeMove);
    presentation.checkRest();
    CHECK(!presentation.tick(201));
  }
}

UMBRIEL_TEST(coordinatedResizeRetargetStartsFromItsHeldFractionalSize) {
  CoordinatedPresentation presentation(200, 200);
  CHECK(presentation.tick(0));
  CHECK(presentation.tick(100));
  CHECK(!presentation.size.animation().animating());
  CHECK(presentation.size.animating());
  const double heldWidth = presentation.size.fractionalWidth();
  const double heldHeight = presentation.size.fractionalHeight();
  const auto heldEdges = presentation.edges();

  presentation.size.animateTo(105, 85, 60, kLinear);
  CHECK(presentation.tick(100));
  CHECK_NEAR(presentation.size.fractionalWidth(), heldWidth, 1e-10);
  CHECK_NEAR(presentation.size.fractionalHeight(), heldHeight, 1e-10);
  CHECK(presentation.edges() == heldEdges);
  CHECK(presentation.size.targeting(105, 85));

  presentation.tick(159.99);
  const auto beforeResize = presentation.edges();
  CHECK(presentation.tick(160));
  CHECK(presentation.edges() == beforeResize);
  CHECK(!presentation.size.animation().animating());
  CHECK(presentation.size.animating());
  presentation.tick(199.99);
  const auto beforeMove = presentation.edges();
  CHECK(presentation.tick(200));
  CHECK(presentation.edges() == beforeMove);
  presentation.checkRest(105, 85);
}

UMBRIEL_TEST(fixedSizePositionAlignmentPreservesBothRestingEdges) {
  for (double scale : {1.0, 1.25, 1.5, 2.0}) {
    for (double origin : {0.0, 101.0, -37.0}) {
      for (double extent : {100.0, 101.0, 100.2}) {
        for (int direction : {-1, 1}) {
          umbriel::AnimatedValue position{origin + 11.0 - direction};
          position.retarget(origin + 11.0, 100, kLinear);
          umbriel::PositionPixelAlignment alignment;
          alignment.update(position, extent, scale, origin, false);
          CHECK(alignment.from.has_value());
          CHECK(alignment.target.has_value());
          CHECK(alignment.roundingMargin > 0.0);
          CHECK(alignment.roundingMargin <= 0.25);
          const long nearTarget = std::lround((position.target() - origin) * scale);
          const long farTarget = std::lround((position.target() + extent - origin) * scale);
          if (alignment.target) {
            // The reserved margin must belong to both edges, not just the origin.
            for (double offset : {-0.99, 0.0, 0.99}) {
              const double near = *alignment.target + offset * alignment.roundingMargin;
              CHECK_EQ(std::lround(near), nearTarget);
              CHECK_EQ(std::lround(near + extent * scale), farTarget);
            }
          }
          position.tick(kStartMsec);
          const double start = position.pixelAlignedCurrent(scale, origin, alignment.target, alignment.from);
          CHECK_EQ(std::lround((start - origin) * scale), std::lround((position.from() - origin) * scale));
          CHECK_EQ(
              std::lround((start + extent - origin) * scale), std::lround((position.from() + extent - origin) * scale)
          );
          position.tick(kStartMsec + 99.99);
          const double before = position.pixelAlignedCurrent(scale, origin, alignment.target, alignment.from);
          CHECK_EQ(std::lround((before - origin) * scale), nearTarget);
          CHECK_EQ(std::lround((before + extent - origin) * scale), farTarget);
          position.tick(kStartMsec + 100);
          const double rest = position.pixelAlignedCurrent(scale, origin, alignment.target, alignment.from);
          CHECK_EQ(rest, origin + 11.0);
          CHECK_EQ(std::lround((rest - origin) * scale), nearTarget);
          CHECK_EQ(std::lround((rest + extent - origin) * scale), farTarget);
        }
      }
    }
  }
}

UMBRIEL_TEST(positionAlignmentIsStableUntilRetargetOrOutputGridChanges) {
  umbriel::AnimatedValue position{10.0};
  position.retarget(11.0, 100, kLinear);
  umbriel::PositionPixelAlignment alignment;
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK_EQ(alignment.from.value_or(-1), 15.25);
  CHECK_EQ(alignment.target.value_or(-1), 16.75);
  position.tick(kStartMsec);
  position.tick(kStartMsec + 50);
  // Derived extent and resize state must not shift an existing transition's endpoints.
  alignment.update(position, 100.0, 1.5, 0.0, true);
  CHECK_EQ(alignment.from.value_or(-1), 15.25);
  CHECK_EQ(alignment.target.value_or(-1), 16.75);

  position.retarget(13.0, 100, kLinear);
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK_EQ(alignment.from.value_or(-1), 15.75);
  CHECK_EQ(alignment.target.value_or(-1), 19.75);
  alignment.update(position, 101.0, 2.0, 0.0, false);
  CHECK_EQ(alignment.target.value_or(-1), 26.0);
  alignment.update(position, 101.0, 2.0, 0.5, false);
  CHECK_EQ(alignment.target.value_or(-1), 25.0);
}

UMBRIEL_TEST(positionAlignmentLeavesResizingAndRestingPositionsUnaligned) {
  umbriel::AnimatedValue position{10.0};
  umbriel::PositionPixelAlignment alignment;
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK(!alignment.from);
  CHECK(!alignment.target);
  position.retarget(11.0, 100, kLinear);
  alignment.update(position, 101.0, 1.5, 0.0, true);
  CHECK(!alignment.from);
  CHECK(!alignment.target);
  CHECK_EQ(alignment.roundingMargin, 0.25);
  // A resize-owned transition stays unaligned even after its extent settles.
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK(!alignment.target);
  position.retarget(12.0, 100, kLinear);
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK(alignment.from.has_value());
  CHECK(alignment.target.has_value());
  position.snap(12.0);
  alignment.update(position, 101.0, 1.5, 0.0, false);
  CHECK(!alignment.from);
  CHECK(!alignment.target);
  CHECK_EQ(alignment.roundingMargin, 0.25);
}

int main() { return RUN_TESTS(); }
