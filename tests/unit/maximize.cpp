#include "view/maximize.h"

#include "check.h"

using umbriel::maximizeRequestTargetsEdges;

UMBRIEL_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(true, false, false)); }

UMBRIEL_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(false, true, false)); }

UMBRIEL_TEST(configuredPolicySendsFreshRequestToEdges) { CHECK(maximizeRequestTargetsEdges(true, false, true)); }

UMBRIEL_TEST(configuredPolicyUnmaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false, false, true)); }

int main() { return RUN_TESTS(); }
