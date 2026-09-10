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

int main() { return RUN_TESTS(); }
