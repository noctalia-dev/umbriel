#include "check.h"
#include "output/connector_hdr_state.h"
#include "wlr_color.h"

extern "C" {
#include <wlr/backend.h>
}

using umbriel::ConnectorHdrState;

namespace {

  const wlr_backend kFirstGpu{};
  const wlr_backend kSecondGpu{};

} // namespace

UMBRIEL_TEST(unseenConnectorHoldsNoHdr) {
  const ConnectorHdrState state;
  CHECK(!state.mayHoldHdr(&kFirstGpu, "DP-3"));
}

UMBRIEL_TEST(hdrCommitIsRememberedForTheConnector) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-3", true, true);
  CHECK(state.mayHoldHdr(&kFirstGpu, "DP-3"));
}

UMBRIEL_TEST(repeatedHdrCommitsNeedOneSdrCommitToForget) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-3", true, true);
  state.recordCommit(&kFirstGpu, "DP-3", true, true);
  state.recordCommit(&kFirstGpu, "DP-3", true, false);
  CHECK(!state.mayHoldHdr(&kFirstGpu, "DP-3"));
}

UMBRIEL_TEST(sdrCommitOnAnotherConnectorKeepsHdrFlag) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-3", true, true);
  state.recordCommit(&kFirstGpu, "HDMI-A-1", true, false);
  CHECK(state.mayHoldHdr(&kFirstGpu, "DP-3"));
  CHECK(!state.mayHoldHdr(&kFirstGpu, "HDMI-A-1"));
}

UMBRIEL_TEST(commitWithoutImageDescriptionKeepsHdrFlag) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-3", true, true);
  state.recordCommit(&kFirstGpu, "DP-3", false, false);
  CHECK(state.mayHoldHdr(&kFirstGpu, "DP-3"));
}

UMBRIEL_TEST(sameConnectorNameOnAnotherBackendIsSeparate) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-1", true, true);
  CHECK(!state.mayHoldHdr(&kSecondGpu, "DP-1"));

  state.recordCommit(&kSecondGpu, "DP-1", true, false);
  CHECK(state.mayHoldHdr(&kFirstGpu, "DP-1"));
}

UMBRIEL_TEST(forgettingABackendDropsOnlyItsEntries) {
  ConnectorHdrState state;
  state.recordCommit(&kFirstGpu, "DP-1", true, true);
  state.recordCommit(&kFirstGpu, "DP-2", true, true);
  state.recordCommit(&kSecondGpu, "DP-1", true, true);

  state.forgetBackend(&kFirstGpu);

  CHECK(!state.mayHoldHdr(&kFirstGpu, "DP-1"));
  CHECK(!state.mayHoldHdr(&kFirstGpu, "DP-2"));
  CHECK(state.mayHoldHdr(&kSecondGpu, "DP-1"));
}

int main() { return RUN_TESTS(); }
