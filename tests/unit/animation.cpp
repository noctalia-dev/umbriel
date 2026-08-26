#include "core/animation.h"

#include "check.h"

#include <cmath>

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

int main() { return RUN_TESTS(); }
