#include "view/deferred_unfullscreen.h"

#include "check.h"

using umbriel::DeferredUnfullscreen;
using umbriel::FullscreenRequestDisposition;

namespace {
  using namespace std::chrono_literals;
  constexpr DeferredUnfullscreen::TimePoint kStart{};
} // namespace

UMBRIEL_TEST(inactiveUnfullscreenRequestIsParked) {
  DeferredUnfullscreen state;
  CHECK(state.observeClientRequest(false, false, true, kStart) == FullscreenRequestDisposition::Park);
  CHECK(state.pending());
}

UMBRIEL_TEST(activationConsumesAFreshParkedRequest) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace));
  CHECK(!state.pending());
  CHECK(!state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace));
}

UMBRIEL_TEST(expiredParkedRequestIsDiscarded) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(!state.takeOnActivation(kStart + DeferredUnfullscreen::kGrace + 1ms));
  CHECK(!state.pending());
}

UMBRIEL_TEST(laterFullscreenRequestSupersedesParkedUnfullscreen) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.observeClientRequest(true, false, true, kStart + 1ms) == FullscreenRequestDisposition::Acknowledge);
  CHECK(!state.pending());
  CHECK(!state.takeOnActivation(kStart + 2ms));
}

UMBRIEL_TEST(applyingAnotherRequestClearsParkedUnfullscreen) {
  DeferredUnfullscreen state;
  static_cast<void>(state.observeClientRequest(false, false, true, kStart));
  CHECK(state.observeClientRequest(true, true, false, kStart + 1ms) == FullscreenRequestDisposition::Apply);
  CHECK(!state.pending());
}

int main() { return RUN_TESTS(); }
