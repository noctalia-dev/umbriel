#include "view/maximize.h"

#include "check.h"

using umbriel::maximizeRequestTargetsEdges;

UMBRIEL_TEST(defaultMaximizedColumnStaysColumnMaximized) { CHECK(!maximizeRequestTargetsEdges(false, true, true)); }

UMBRIEL_TEST(freshClientMaximizeTargetsEdges) { CHECK(maximizeRequestTargetsEdges(false, false, true)); }

UMBRIEL_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(true, true, false)); }

int main() { return RUN_TESTS(); }
