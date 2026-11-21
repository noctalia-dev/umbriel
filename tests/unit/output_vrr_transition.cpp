#include "check.h"
#include "output/vrr_transition.h"

using umbriel::VrrTransition;

UMBRIEL_TEST(vrrChangeIsTriedWhenPolicyDiffersFromLiveState) {
  const VrrTransition transition;
  CHECK(transition.shouldTry(true, false));
  CHECK(transition.shouldTry(false, true));
  CHECK(!transition.shouldTry(true, true));
  CHECK(!transition.shouldTry(false, false));
}

UMBRIEL_TEST(rejectedVrrChangeIsNotRetried) {
  VrrTransition transition;
  transition.reject(true);
  CHECK(!transition.shouldTry(true, false));
  // The opposite direction is a different change and is still tried.
  CHECK(transition.shouldTry(false, true));
}

UMBRIEL_TEST(resetAllowsRejectedVrrChangeAgain) {
  VrrTransition transition;
  transition.reject(false);
  CHECK(!transition.shouldTry(false, true));
  transition.reset();
  CHECK(transition.shouldTry(false, true));
}

int main() { return RUN_TESTS(); }
