#include "check.h"
#include "core/application_scope.h"

using umbriel::applicationScopeKillCommand;

UMBRIEL_TEST(applicationScopeKillTargetsOnlyThisCompositorScopes) {
  CHECK(
      applicationScopeKillCommand("/usr/bin/systemctl", 123)
      == "'/usr/bin/systemctl' --user kill --kill-whom=all --signal=SIGKILL 'app-umbriel-123-*.scope'"
  );
  CHECK(
      applicationScopeKillCommand("/a'b/systemctl", 2)
      == "'/a'\\''b/systemctl' --user kill --kill-whom=all --signal=SIGKILL 'app-umbriel-2-*.scope'"
  );
  CHECK(applicationScopeKillCommand("", 123).empty());
  CHECK(applicationScopeKillCommand("/usr/bin/systemctl", 0).empty());
  CHECK(applicationScopeKillCommand("/usr/bin/systemctl", -1).empty());
}

int main() { return RUN_TESTS(); }
