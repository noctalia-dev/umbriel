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

UMBRIEL_TEST(monotonicEasingPreservesSafeCurvesAndProjectsOvershootAcrossTheFullTimeline) {
  const umbriel::AnimationCurve ordinary{
      .easing = umbriel::Easing::CustomBezier,
      .bezier = {.x1 = 0.25, .y1 = 0.46, .x2 = 0.35, .y2 = 1.0},
  };
  const umbriel::MonotonicEasing ordinaryMotion{ordinary};
  for (int step = 0; step <= 20; ++step) {
    const double linear = static_cast<double>(step) / 20.0;
    CHECK(std::abs(ordinaryMotion.value(linear) - umbriel::evaluateCurve(ordinary, linear)) < 0.000001);
  }

  const umbriel::AnimationCurve snappy{.easing = umbriel::Easing::Snappy};
  const umbriel::MonotonicEasing safeMotion{snappy};
  double previous = 0.0;
  for (int step = 0; step <= 100; ++step) {
    const double linear = static_cast<double>(step) / 100.0;
    const double progress = safeMotion.value(linear);
    CHECK(progress >= previous);
    CHECK(progress >= 0.0);
    CHECK(progress <= 1.0);
    if (step < 100) {
      CHECK(progress < 1.0);
    }
    previous = progress;
  }
  CHECK(safeMotion.value(0.75) < safeMotion.value(1.0));
}

UMBRIEL_TEST(monotonicEasingBoundsReversingPresets) {
  for (const umbriel::Easing easing : {
           umbriel::Easing::EaseInBack,
           umbriel::Easing::EaseOutBack,
           umbriel::Easing::EaseInOutBack,
           umbriel::Easing::EaseInElastic,
           umbriel::Easing::EaseOutElastic,
           umbriel::Easing::EaseInOutElastic,
           umbriel::Easing::EaseInBounce,
           umbriel::Easing::EaseOutBounce,
           umbriel::Easing::EaseInOutBounce,
           umbriel::Easing::Spring,
       }) {
    const umbriel::MonotonicEasing motion{umbriel::AnimationCurve{.easing = easing}};
    double previous = 0.0;
    for (int step = 0; step <= 200; ++step) {
      const double progress = motion.value(static_cast<double>(step) / 200.0);
      CHECK(progress >= previous);
      CHECK(progress >= 0.0);
      CHECK(progress <= 1.0);
      previous = progress;
    }
    CHECK_EQ(motion.value(0.0), 0.0);
    CHECK_EQ(motion.value(1.0), 1.0);
  }
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

UMBRIEL_TEST(springCurveTimescaleComesFromItsParameters) {
  const auto stiff = umbriel::CurveRegistry::parse("spring:1,4000");
  const auto soft = umbriel::CurveRegistry::parse("spring:1,250");
  CHECK(stiff.has_value());
  CHECK(soft.has_value());
  if (!stiff || !soft) {
    return;
  }

  umbriel::AnimatedValue fast;
  fast.retarget(1.0, 5000, *stiff);
  umbriel::AnimatedValue slow;
  slow.retarget(1.0, 5000, *soft);

  // duration_ms never reaches a spring; the sixteenfold stiffness quarters the settle time.
  CHECK(fast.durationMs() != 5000);
  CHECK(slow.durationMs() != 5000);
  const double stiffnessRatio = static_cast<double>(slow.durationMs()) / static_cast<double>(fast.durationMs());
  CHECK(std::abs(stiffnessRatio - 4.0) < 0.05);

  // Mass is the other half of the timescale: four times the mass takes twice as long.
  const int light = umbriel::springDurationMs({.damping = 1.0, .stiffness = 1000.0, .mass = 1.0});
  const int heavy = umbriel::springDurationMs({.damping = 1.0, .stiffness = 1000.0, .mass = 4.0});
  const double massRatio = static_cast<double>(heavy) / static_cast<double>(light);
  CHECK(std::abs(massRatio - 2.0) < 0.05);

  // Damping shapes the response instead: it must not leave the timescale untouched either.
  CHECK(umbriel::springDurationMs({.damping = 2.0, .stiffness = 1000.0, .mass = 1.0}) > light);
}

UMBRIEL_TEST(springCurveSettlesOnItsTargetBeforeTheTimelineEnds) {
  struct Case {
    const char* text;
    bool overshoots;
  };
  // Underdamped, critically damped, and overdamped all have to be within a tenth of a percent of the target on the
  // last frame, otherwise the timeline's final snap is a visible jump.
  for (const Case& probe : {Case{"spring:0.4,600", true}, Case{"spring:1,600", false}, Case{"spring:2,600", false}}) {
    const auto curve = umbriel::CurveRegistry::parse(probe.text);
    CHECK(curve.has_value());
    if (!curve) {
      continue;
    }
    CHECK(std::abs(umbriel::applyEasing(*curve, 0.999) - 1.0) < 0.001);

    double peak = 0.0;
    for (int sample = 0; sample <= 1000; ++sample) {
      peak = std::max(peak, umbriel::applyEasing(*curve, static_cast<double>(sample) / 1000.0));
    }
    CHECK_EQ(peak > 1.001, probe.overshoots);
  }
}

UMBRIEL_TEST(overdampedSpringVelocityMatchesItsPositionDerivative) {
  constexpr double sampleTime = 0.075;
  constexpr double delta = 0.000001;
  const umbriel::SpringConfig spring{.damping = 2.0, .stiffness = 100.0, .mass = 1.0};

  double velocity = 0.0;
  static_cast<void>(umbriel::solveSpringPhysics(0.0, 1.0, 2.0, sampleTime, spring, &velocity));
  const double before = umbriel::solveSpringPhysics(0.0, 1.0, 2.0, sampleTime - delta, spring);
  const double after = umbriel::solveSpringPhysics(0.0, 1.0, 2.0, sampleTime + delta, spring);
  const double derivative = (after - before) / (2.0 * delta);

  CHECK(std::abs(velocity - derivative) < 0.00001);
}

UMBRIEL_TEST(springDisplacementBoundIncludesPositionAndVelocityEnergy) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 100.0, .mass = 1.0};

  CHECK(std::abs(umbriel::springDisplacementBound(1.25, 1.0, 0.0, spring) - 0.25) < 1e-12);
  CHECK(std::abs(umbriel::springDisplacementBound(1.0, 1.0, 2.0, spring) - 0.2) < 1e-12);
  CHECK(std::abs(umbriel::springDisplacementBound(1.15, 1.0, 2.0, spring) - std::hypot(0.15, 0.2)) < 1e-12);
}

UMBRIEL_TEST(springTailFinishesOnlyInsideTheTargetPixelsRoundingCell) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 100.0, .mass = 1.0};

  umbriel::AnimatedValue halfPixel{0.5};
  halfPixel.settleSpring(0.0, spring, 0.0);
  CHECK(halfPixel.tick(1000));
  CHECK(!halfPixel.finishSpringTail(1.0));
  CHECK(halfPixel.animating());

  umbriel::AnimatedValue insidePixel{0.49};
  insidePixel.settleSpring(0.0, spring, 0.0);
  CHECK(insidePixel.tick(1000));
  CHECK(insidePixel.finishSpringTail(1.0));
  CHECK(!insidePixel.animating());
  CHECK_EQ(insidePixel.current(), 0.0);

  umbriel::AnimatedValue offCenter{0.8};
  offCenter.settleSpring(0.49, spring, 0.0);
  CHECK(offCenter.tick(1000));
  CHECK(!offCenter.finishSpringTail(1.0));
  CHECK(offCenter.animating());

  umbriel::AnimatedValue release{0.3};
  release.settleSpring(0.3, spring, 4.0);
  CHECK(release.tick(1000));
  CHECK(!release.finishSpringTail(587.0));
  CHECK(release.animating());

  umbriel::AnimatedValue durationSpring{0.0};
  durationSpring.retarget(0.1, 100, umbriel::Easing::Spring);
  CHECK(durationSpring.tick(1000));
  CHECK(!durationSpring.finishSpringTail(1.0));
  CHECK(durationSpring.animating());

  const umbriel::SpringConfig overdamped{.damping = 2.0, .stiffness = 100.0, .mass = 1.0};
  umbriel::AnimatedValue overdampedRelease{0.0};
  overdampedRelease.settleSpring(0.0, overdamped, 30.0);
  CHECK(overdampedRelease.tick(1000));
  CHECK(overdampedRelease.tick(1012));
  CHECK(!overdampedRelease.finishSpringTail(1.0));
  CHECK(overdampedRelease.animating());
}

UMBRIEL_TEST(springTailPresentationDeceleratesAtTheReportedRefreshRate) {
  constexpr double step = 587.0;
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  const auto checkDirection = [spring](double from, double target) {
    umbriel::AnimatedValue value{from};
    value.settleSpring(target, spring, 0.0);
    CHECK(value.tick(1000));

    int firstFrameAtThreePixels = -1;
    int firstFrameAtTwoPixels = -1;
    int firstFrameAtOnePixel = -1;
    int firstFrameAtTarget = -1;
    bool finishedTail = false;
    for (int frame = 1; frame < 200 && value.animating(); ++frame) {
      CHECK(value.tick(1000 + static_cast<uint64_t>(frame * 1000 / 165)));
      const int solvedOffset = static_cast<int>(std::lround((value.target() - value.current()) * step));
      finishedTail = value.finishSpringTail(step) || finishedTail;

      const int presentedOffset = static_cast<int>(std::lround((value.target() - value.current()) * step));
      CHECK_EQ(presentedOffset, solvedOffset);
      const int offset = std::abs(presentedOffset);
      if (offset == 3 && firstFrameAtThreePixels < 0) {
        firstFrameAtThreePixels = frame;
      } else if (offset == 2 && firstFrameAtTwoPixels < 0) {
        firstFrameAtTwoPixels = frame;
      } else if (offset == 1 && firstFrameAtOnePixel < 0) {
        firstFrameAtOnePixel = frame;
      } else if (offset == 0 && firstFrameAtTarget < 0) {
        firstFrameAtTarget = frame;
      }
    }

    CHECK(firstFrameAtThreePixels >= 0);
    CHECK(firstFrameAtTwoPixels > firstFrameAtThreePixels);
    CHECK(firstFrameAtOnePixel > firstFrameAtTwoPixels);
    CHECK(firstFrameAtTarget > firstFrameAtOnePixel);
    const int threeToTwoFrames = firstFrameAtTwoPixels - firstFrameAtThreePixels;
    const int twoToOneFrames = firstFrameAtOnePixel - firstFrameAtTwoPixels;
    const int oneToTargetFrames = firstFrameAtTarget - firstFrameAtOnePixel;
    CHECK(twoToOneFrames >= threeToTwoFrames);
    CHECK(oneToTargetFrames >= twoToOneFrames);
    CHECK(finishedTail);
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
