#include "check.h"
#include "output/capture_demand.h"

#include <array>

namespace {
  int firstSource, secondSource;
  using Consumer = umbriel::OutputCaptureConsumer;
  template <std::size_t N> bool requested(int locks, const std::array<Consumer, N>& consumers) {
    return umbriel::outputCaptureRequested(locks, consumers.begin(), consumers.end(), [](Consumer c) { return c; });
  }
} // namespace

UMBRIEL_TEST(idleAndPendingSessionLifetimes) {
  std::array consumers{Consumer{&firstSource, false}};
  CHECK(!requested(1, consumers)); // allocated frame not yet capturing, or no frame
  consumers[0].pending = true;
  CHECK(requested(1, consumers));
  consumers[0].pending = false; // ready destroys the wlroots frame before the client fence wait
  CHECK(!requested(1, consumers));
  CHECK(!requested(0, consumers));
}

UMBRIEL_TEST(sharedSourceKeepsOtherCaptureProtocolsActive) {
  const std::array consumers{Consumer{&firstSource, false}, Consumer{&firstSource, false}};
  CHECK(!requested(1, consumers));
  CHECK(requested(2, consumers)); // one source lock plus one screencopy lock
}

UMBRIEL_TEST(anySessionOnASharedSourceCanRequestCapture) {
  const std::array consumers{Consumer{&firstSource, false}, Consumer{&firstSource, true}};
  CHECK(requested(1, consumers));
}

UMBRIEL_TEST(distinctSourcesAndUnrelatedOutputs) {
  const std::array consumers{Consumer{&firstSource, false}, Consumer{nullptr, true}, Consumer{&secondSource, false}};
  CHECK(!requested(2, consumers));
  CHECK(requested(3, consumers));
}

UMBRIEL_TEST(unknownLocksRemainConservative) {
  const std::array<Consumer, 0> consumers{};
  CHECK(requested(1, consumers));
  CHECK(!requested(0, consumers));
  CHECK(!requested(-1, consumers));
}

int main() { return RUN_TESTS(); }
