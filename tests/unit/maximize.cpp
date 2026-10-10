#include "view/maximize.h"

#include "check.h"

using umbriel::maximizeRequestTargetsEdges;

UMBRIEL_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(true, false, false)); }

UMBRIEL_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(false, true, false)); }

UMBRIEL_TEST(configuredPolicySendsFreshRequestToEdges) { CHECK(maximizeRequestTargetsEdges(true, false, true)); }

UMBRIEL_TEST(configuredPolicyKeepsEdgesRequestsOnEdges) { CHECK(maximizeRequestTargetsEdges(true, true, true)); }

UMBRIEL_TEST(freshClientUnmaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false, false, false)); }

UMBRIEL_TEST(configuredPolicyUnmaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false, false, true)); }

UMBRIEL_TEST(configuredPolicyKeepsEdgesUnmaximizeOnEdges) { CHECK(maximizeRequestTargetsEdges(false, true, true)); }

int main() { return RUN_TESTS(); }
