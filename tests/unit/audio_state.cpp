#include "audio/state.h"
#include "check.h"

using namespace umbriel::audio;

namespace {
  constexpr uint64_t kNow = 1'000'000'000;
  void start(Receiver& receiver) {
    receiver.begin(1, SourceType::Playback, kNow);
    CHECK_EQ(
        receiver.receive(encode(Ready{.epoch = 1, .source = SourceType::Playback}), kNow), ReceiveResult::Accepted
    );
  }
  Snapshot sample(uint64_t sequence = 1, uint64_t observation = kNow, float value = 1) {
    return {
        .epoch = 1,
        .generation = 1,
        .sequence = sequence,
        .observationNs = observation,
        .features = {.rms = value, .peak = value, .envelope = value, .bands = {}}
    };
  }
} // namespace

UMBRIEL_TEST(readyNegotiationDeadlineAndSourceType) {
  Receiver receiver;
  receiver.begin(1, SourceType::Playback, kNow);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Rejected);
  CHECK_EQ(
      receiver.receive(encode(Ready{.epoch = 1, .source = SourceType::Microphone}), kNow), ReceiveResult::Rejected
  );
  CHECK_EQ(receiver.receive(encode(Ready{.epoch = 2, .source = SourceType::Playback}), kNow), ReceiveResult::Rejected);
  CHECK(!receiver.readyExpired(kNow + kReadyDeadlineNs - 1));
  CHECK(receiver.readyExpired(kNow + kReadyDeadlineNs));
  CHECK_EQ(
      receiver.receive(encode(Ready{.epoch = 1, .source = SourceType::Playback}), kNow + kReadyDeadlineNs),
      ReceiveResult::Rejected
  );
  start(receiver);
  CHECK(receiver.ready());
  CHECK(!receiver.input().available);
}

UMBRIEL_TEST(generationCoalescingAndOrdering) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  CHECK(receiver.input().available);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Rejected);
  CHECK_EQ(receiver.receive(encode(sample(2)), kNow), ReceiveResult::Rejected);
  auto next = sample(2, kNow + 1);
  next.epoch = 2;
  CHECK_EQ(receiver.receive(encode(next), kNow + 1), ReceiveResult::Rejected);
  next.epoch = 1;
  next.generation = 5;
  next.sequence = 1;
  CHECK_EQ(receiver.receive(encode(next), kNow + 1), ReceiveResult::Accepted);
  CHECK_EQ(receiver.generation(), 5U);
  CHECK_EQ(receiver.receive(encode(sample(100, kNow + 2)), kNow + 2), ReceiveResult::Rejected);
  next.sequence = 2;
  next.observationNs = kNow + 3 + kFutureToleranceNs;
  CHECK_EQ(receiver.receive(encode(next), kNow + 2), ReceiveResult::Rejected);
}

UMBRIEL_TEST(heartbeatDoesNotRefreshMeasurementAndFadeEndsExactly) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  CHECK_EQ(receiver.receive(encode(Status{.epoch = 1, .generation = 1}), kNow + kStaleNs), ReceiveResult::Accepted);
  receiver.advance(kNow + kStaleNs);
  CHECK(!receiver.input().available);
  CHECK(receiver.decaying());
  CHECK_EQ(receiver.observationNs(), kNow);
  receiver.advance(kNow + kStaleNs + kFadeNs / 2);
  CHECK(receiver.input().features.rms > 0.49F && receiver.input().features.rms < 0.51F);
  receiver.advance(kNow + kStaleNs + kFadeNs);
  CHECK_EQ(receiver.input(), Input{});
  CHECK(!receiver.decaying());
  const auto revision = receiver.revision();
  receiver.advance(kNow + 10 * kStaleNs);
  CHECK_EQ(receiver.revision(), revision);
}

UMBRIEL_TEST(silenceDoesNotDirtyInputAndHealthyEnvelopeIsUnchanged) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample(1, kNow, 0)), kNow), ReceiveResult::Accepted);
  const auto revision = receiver.revision();
  CHECK_EQ(receiver.receive(encode(sample(2, kNow + 1, 0)), kNow + 1), ReceiveResult::Accepted);
  CHECK_EQ(receiver.revision(), revision);
  auto next = sample(3, kNow + 2, 0.8F);
  next.features.envelope = 0.2F;
  CHECK_EQ(receiver.receive(encode(next), kNow + 2), ReceiveResult::Accepted);
  CHECK_EQ(receiver.input().features.envelope, 0.2F);
  receiver.clear();
  CHECK_EQ(receiver.input(), Input{});
  CHECK(!receiver.ready());
  CHECK(!receiver.decaying());
}

UMBRIEL_TEST(unavailableImmediatelyBeginsFadeAndRejectsStaleMeasurement) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  CHECK_EQ(
      receiver.receive(encode(Status{.epoch = 1, .generation = 1, .unavailable = true}), kNow + 1),
      ReceiveResult::Accepted
  );
  CHECK(!receiver.input().available);
  CHECK(receiver.decaying());
  CHECK_EQ(receiver.receive(encode(sample(2, kNow + 1)), kNow + kStaleNs + 1), ReceiveResult::Rejected);
  receiver.advance(kNow + kFadeNs + 1);
  CHECK_EQ(receiver.input(), Input{});
}

UMBRIEL_TEST(floodIsBoundedWithBurstAndRefill) {
  Receiver receiver;
  receiver.begin(1, SourceType::Playback, kNow);
  const auto malformed = std::vector<uint8_t>{0};
  for (size_t i = 0; i < 18; ++i) {
    CHECK_EQ(receiver.receive(malformed, kNow), ReceiveResult::Rejected);
  }
  CHECK_EQ(receiver.receive(malformed, kNow), ReceiveResult::Flood);
  CHECK_EQ(
      receiver.receive(encode(Ready{.epoch = 1, .source = SourceType::Playback}), kNow + 10'000'000),
      ReceiveResult::Accepted
  );
}

UMBRIEL_TEST(failedSubmissionRetainsLatchAndFreezeAllowsOnlyInjection) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  InputLatch display;
  InputLatch capture;
  CHECK_EQ(display.latch(receiver).features.rms, 1);
  CHECK_EQ(capture.latch(receiver).features.rms, 1);
  display.submitted(false);
  capture.submitted(true);
  CHECK(display.dirty(receiver));
  CHECK(!capture.dirty(receiver));
  CHECK_EQ(receiver.receive(encode(sample(2, kNow + 1, 0.5F)), kNow + 1), ReceiveResult::Accepted);
  CHECK_EQ(display.latch(receiver).features.rms, 1);
  display.submitted(true);
  CHECK(display.dirty(receiver));
  CHECK_EQ(display.latch(receiver, true).features.rms, 1);
  CHECK_EQ(display.latch(receiver, true, true).features.rms, 0.5F);
  display.submitted(true);
  CHECK(!display.dirty(receiver));
  CHECK(capture.dirty(receiver));
}

UMBRIEL_TEST(retryBackoffStopsAfterFiveFailuresAndNoDemandClearsTimer) {
  RetryPolicy retry;
  retry.failed(kNow);
  CHECK(!retry.deadlineNs());
  retry.setDemand(true);
  uint64_t now = kNow;
  for (uint64_t delay : {1, 2, 4, 8}) {
    retry.failed(now);
    CHECK_EQ(*retry.deadlineNs(), now + delay * 1'000'000'000);
    now = *retry.deadlineNs();
  }
  retry.failed(now);
  CHECK(retry.exhausted());
  CHECK(!retry.deadlineNs());
  retry.reset();
  retry.failed(now);
  CHECK(retry.deadlineNs());
  retry.setDemand(false);
  CHECK(!retry.deadlineNs());
  retry.setDemand(true);
  retry.failed(now + 60'000'000'000);
  CHECK_EQ(*retry.deadlineNs(), now + 61'000'000'000);
}

UMBRIEL_TEST(cancelAbandonsFailedFrameBeforeSessionRestart) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  InputLatch latch;
  CHECK_EQ(latch.latch(receiver).features.rms, 1);
  latch.submitted(false);
  receiver.clear();
  latch.cancel();
  CHECK_EQ(latch.latch(receiver, true), Input{});
  latch.submitted(true);
  CHECK(!latch.dirty(receiver));
}

UMBRIEL_TEST(noDamageFrameDoesNotPinAnUnchangedLatch) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample()), kNow), ReceiveResult::Accepted);
  InputLatch latch;
  CHECK_EQ(latch.latch(receiver).features.rms, 1);
  latch.submitted(true);
  CHECK_EQ(latch.latch(receiver).features.rms, 1);
  latch.submitted(false); // no changed packed state, so there is nothing pending
  CHECK_EQ(receiver.receive(encode(sample(2, kNow + 1, 0.25F)), kNow + 1), ReceiveResult::Accepted);
  CHECK(latch.dirty(receiver));
  CHECK_EQ(latch.latch(receiver).features.rms, 0.25F);
  latch.submitted(false);
  CHECK(latch.dirty(receiver));
  latch.submitted(true);
  CHECK(!latch.dirty(receiver));
}

UMBRIEL_TEST(failedFrameRemainsDirtyWhenNewestInputReturnsToPresentedValue) {
  Receiver receiver;
  start(receiver);
  CHECK_EQ(receiver.receive(encode(sample(1, kNow, 0)), kNow), ReceiveResult::Accepted);
  InputLatch latch;
  (void)latch.latch(receiver);
  latch.submitted(true);
  CHECK_EQ(receiver.receive(encode(sample(2, kNow + 1, 1)), kNow + 1), ReceiveResult::Accepted);
  (void)latch.latch(receiver);
  latch.submitted(false);
  CHECK_EQ(receiver.receive(encode(sample(3, kNow + 2, 0)), kNow + 2), ReceiveResult::Accepted);
  CHECK(latch.dirty(receiver));
  CHECK_EQ(latch.latch(receiver).features.rms, 1);
  latch.submitted(true);
  CHECK(latch.dirty(receiver));
  CHECK_EQ(latch.latch(receiver).features.rms, 0);
  latch.submitted(true);
  CHECK(!latch.dirty(receiver));
}

int main() { return RUN_TESTS(); }
