#include "core/animation.h"

#include "check.h"

#include <cmath>

namespace {
  void
  checkColorNear(const std::array<float, 4>& actual, const std::array<float, 4>& expected, float tolerance = 0.00001F) {
    for (std::size_t channel = 0; channel < actual.size(); ++channel) {
      CHECK(std::abs(actual[channel] - expected[channel]) < tolerance);
    }
  }

  void checkShaderSeed(const std::array<float, 4>& seed) {
    for (const float channel : seed) {
      CHECK(std::isfinite(channel));
      CHECK(channel >= 0.0F);
      CHECK(channel < 1.0F);
    }
  }
} // namespace

UMBRIEL_TEST(curveParserAcceptsCanonicalFiniteForms) {
  const auto bezier = umbriel::CurveRegistry::parse("0.1, 0.2, 0.3, 1.0");
  CHECK(bezier.has_value());
  if (bezier) {
    CHECK(bezier->easing == umbriel::Easing::CustomBezier);
    CHECK_EQ(bezier->bezier.x1, 0.1);
    CHECK_EQ(bezier->bezier.y2, 1.0);
  }

  const auto spring = umbriel::CurveRegistry::parse("spring: 0.5, 200");
  CHECK(spring.has_value());
  if (spring) {
    CHECK(spring->easing == umbriel::Easing::Spring);
    CHECK_EQ(spring->spring.damping, 0.5);
    CHECK_EQ(spring->spring.stiffness, 200.0);
  }
}

UMBRIEL_TEST(curveParserRejectsNonFiniteAndTrailingValues) {
  CHECK(!umbriel::CurveRegistry::parse("nan, 0.2, 0.3, 1.0").has_value());
  CHECK(!umbriel::CurveRegistry::parse("1.1, 0.2, 0.3, 1.0").has_value());
  CHECK(!umbriel::CurveRegistry::parse("0.1, 0.2, 0.3, 1.0 trailing").has_value());
  CHECK(!umbriel::CurveRegistry::parse("spring: nan, 200").has_value());
  CHECK(!umbriel::CurveRegistry::parse("spring: 0.5, 200 trailing").has_value());
}

UMBRIEL_TEST(animatedValueReachesItsTargetOnTheConfiguredTimeline) {
  umbriel::AnimatedValue value{10.0};
  value.retarget(20.0, 100, umbriel::Easing::Linear);

  CHECK(value.tick(1000));
  CHECK_EQ(value.current(), 10.0);
  CHECK(value.animating());

  CHECK(value.tick(1050));
  CHECK(std::abs(value.current() - 15.0) < 0.0001);
  CHECK(value.animating());

  CHECK(value.tick(1100));
  CHECK_EQ(value.current(), 20.0);
  CHECK(!value.animating());
}

UMBRIEL_TEST(animationTransitionIdentityIsStableAndRefreshesOnRetarget) {
  umbriel::AnimatedValue value{10.0};
  CHECK_EQ(value.transitionId(), uint64_t{0});

  value.retarget(20.0, 100, umbriel::Easing::Linear);
  const uint64_t firstId = value.transitionId();
  const auto firstSeed = value.shaderSeed();
  CHECK(firstId != 0);
  checkShaderSeed(firstSeed);

  CHECK(value.tick(1000));
  CHECK(value.tick(1050));
  value.translate(2.0);
  value.snap(value.current());
  CHECK_EQ(value.transitionId(), firstId);
  CHECK(value.shaderSeed() == firstSeed);

  value.retarget(value.current(), 100, umbriel::Easing::Linear);
  CHECK(value.transitionId() != firstId);
  CHECK(value.shaderSeed() != firstSeed);
  checkShaderSeed(value.shaderSeed());

  const uint64_t retargetId = value.transitionId();
  const auto retargetSeed = value.shaderSeed();
  value.settleSpring(value.current(), umbriel::SpringConfig{}, 1.0);
  CHECK(value.transitionId() != retargetId);
  CHECK(value.shaderSeed() != retargetSeed);
  checkShaderSeed(value.shaderSeed());

  umbriel::AnimatedValue otherValue;
  umbriel::AnimatedColor otherColor;
  otherValue.retarget(1.0, 100);
  otherColor.retarget({1.0F, 1.0F, 1.0F, 1.0F}, 100);
  CHECK(otherValue.transitionId() != value.transitionId());
  CHECK(otherColor.transitionId() != value.transitionId());
  CHECK(otherColor.transitionId() != otherValue.transitionId());
}

UMBRIEL_TEST(okLabConversionRoundTripsSrgbColor) {
  const std::array<float, 4> source{0.12F, 0.48F, 0.9F, 0.35F};
  const umbriel::OkLab converted = umbriel::srgbToOkLab(source);
  const std::array<float, 4> roundTrip = umbriel::okLabToSrgb(converted, source[3]);

  checkColorNear(roundTrip, source);
}

UMBRIEL_TEST(springSettleStartsFromTheReleaseVelocityAndStops) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  // Settling back onto the row it came from still has to move: the release velocity carries it past the target
  // before the spring pulls it back.
  umbriel::AnimatedValue value;
  value.snap(0.3);
  value.settleSpring(0.3, spring, 4.0);
  CHECK(value.tick(1000));
  CHECK_EQ(value.current(), 0.3);
  CHECK(value.animating());
  CHECK(value.tick(1016));
  CHECK(value.current() > 0.3);
  CHECK(value.tick(2000));
  CHECK_EQ(value.current(), 0.3);
  CHECK(!value.animating());

  // A settle with no velocity left still lands on the new row rather than snapping to it.
  value.settleSpring(1.0, spring, 0.0);
  CHECK(value.tick(2000));
  CHECK_EQ(value.current(), 0.3);
  CHECK(value.tick(2016));
  CHECK(value.current() > 0.3);
  CHECK(value.current() < 1.0);
  CHECK(value.tick(3000));
  CHECK_EQ(value.current(), 1.0);
  CHECK(!value.animating());

  // Renumbering the rows underneath a running settle moves the whole motion, not just the target.
  value.settleSpring(2.0, spring, 0.0);
  CHECK(value.tick(3000));
  value.translate(-1.0);
  CHECK_EQ(value.target(), 1.0);
  CHECK_EQ(value.current(), 0.0);
}

UMBRIEL_TEST(springDisplacementBoundIncludesPositionAndVelocityEnergy) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 100.0, .mass = 1.0};

  CHECK(std::abs(umbriel::springDisplacementBound(1.25, 1.0, 0.0, spring) - 0.25) < 1e-12);
  CHECK(std::abs(umbriel::springDisplacementBound(1.0, 1.0, 2.0, spring) - 0.2) < 1e-12);
  CHECK(std::abs(umbriel::springDisplacementBound(1.15, 1.0, 2.0, spring) - std::hypot(0.15, 0.2)) < 1e-12);
}

UMBRIEL_TEST(springTailPixelsAdvanceWithoutAStationaryFrameOrReversal) {
  CHECK_EQ(umbriel::advanceSpringTailPixels(3, 2), 2);
  CHECK_EQ(umbriel::advanceSpringTailPixels(2, 2), 1);
  CHECK_EQ(umbriel::advanceSpringTailPixels(1, 2), 0);
  CHECK_EQ(umbriel::advanceSpringTailPixels(-3, -2), -2);
  CHECK_EQ(umbriel::advanceSpringTailPixels(-2, -2), -1);
  CHECK_EQ(umbriel::advanceSpringTailPixels(-1, -2), 0);
  CHECK_EQ(umbriel::advanceSpringTailPixels(2, -1), 0);
  CHECK_EQ(umbriel::advanceSpringTailPixels(-2, 1), 0);
}

UMBRIEL_TEST(springTailPresentationMovesEveryFrameAtTheReportedRefreshRate) {
  constexpr double step = 587.0;
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  const auto checkDirection = [spring](double from, double target) {
    umbriel::AnimatedValue value{from};
    value.settleSpring(target, spring, 0.0);
    CHECK(value.tick(1000));

    bool enteredTail = false;
    int terminalFrames = 0;
    for (int frame = 1; frame < 200 && value.animating(); ++frame) {
      const double previous = value.current();
      const int previousOffset = static_cast<int>(std::lround((value.target() - previous) * step));
      CHECK(value.tick(1000 + static_cast<uint64_t>(frame * 1000 / 165)));
      if (!umbriel::advanceSpringTail(value, previous, step, 3.0)) {
        continue;
      }
      enteredTail = true;
      ++terminalFrames;
      const int presentedOffset = static_cast<int>(std::lround((value.target() - value.current()) * step));
      CHECK(std::abs(presentedOffset) < std::abs(previousOffset));
    }
    CHECK(enteredTail);
    CHECK(terminalFrames >= 2);
    CHECK(!value.animating());
    CHECK_EQ(value.current(), target);
  };

  checkDirection(0.0, 4.0);
  checkDirection(4.0, 0.0);
}

UMBRIEL_TEST(physicsSpringKeepsShaderIdentityWhenProgressReverses) {
  umbriel::AnimatedValue value;
  value.settleSpring(1.0, umbriel::SpringConfig{.damping = 0.1, .stiffness = 100.0, .mass = 1.0}, 0.0);
  const uint64_t transitionId = value.transitionId();
  const auto seed = value.shaderSeed();

  CHECK(value.tick(1000));
  CHECK(value.tick(1320));
  const double forwardProgress = value.progress();
  CHECK(value.tick(1640));
  CHECK(value.progress() < forwardProgress);
  CHECK_EQ(value.transitionId(), transitionId);
  CHECK(value.shaderSeed() == seed);
}

UMBRIEL_TEST(animatedColorRefreshesCachedEndpointsWhenRetargeted) {
  const std::array<float, 4> red{1.0F, 0.0F, 0.0F, 0.2F};
  const std::array<float, 4> green{0.0F, 1.0F, 0.0F, 0.6F};
  const std::array<float, 4> blue{0.0F, 0.0F, 1.0F, 0.8F};
  umbriel::AnimatedColor color{red};

  color.retarget(green, 100, umbriel::Easing::Linear);
  CHECK(color.tick(1000));
  CHECK(color.tick(1050));
  const std::array<float, 4> firstMidpoint = color.current();

  color.retarget(blue, 100, umbriel::Easing::Linear);
  CHECK(color.tick(2000));
  CHECK(color.tick(2050));

  const umbriel::OkLab expectedLab =
      umbriel::interpolateOkLab(umbriel::srgbToOkLab(firstMidpoint), umbriel::srgbToOkLab(blue), 0.5F);
  const std::array<float, 4> expected = umbriel::okLabToSrgb(expectedLab, std::lerp(firstMidpoint[3], blue[3], 0.5F));
  checkColorNear(color.current(), expected);
}

UMBRIEL_TEST(animatedColorRefreshesShaderIdentityOnlyWhenRetargeted) {
  umbriel::AnimatedColor color{1.0F, 0.0F, 0.0F};
  color.retarget(0.0F, 1.0F, 0.0F, 1.0F, 100, umbriel::AnimationCurve{});
  const uint64_t firstId = color.transitionId();
  const auto firstSeed = color.shaderSeed();
  CHECK(firstId != 0);
  checkShaderSeed(firstSeed);

  CHECK(color.tick(1000));
  CHECK(color.tick(1050));
  color.snap(color.current());
  CHECK_EQ(color.transitionId(), firstId);
  CHECK(color.shaderSeed() == firstSeed);

  color.retarget(color.current(), 100, umbriel::Easing::Linear);
  CHECK(color.transitionId() != firstId);
  CHECK(color.shaderSeed() != firstSeed);
  checkShaderSeed(color.shaderSeed());
}

int main() { return RUN_TESTS(); }
