#include "audio/service.h"
#include "audio/supervisor.h"
#include "check.h"
#include "core/process.h"

#include <chrono>
#include <csignal>
#include <wayland-server-core.h>

using namespace umbriel::audio;

namespace {
  template <typename Predicate> bool dispatchUntil(wl_event_loop* loop, Predicate predicate, int limitMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limitMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      if (wl_event_loop_dispatch(loop, 10) < 0) {
        return false;
      }
    }
    return predicate();
  }

  struct Loop {
    wl_event_loop* loop = wl_event_loop_create();
    ~Loop() { wl_event_loop_destroy(loop); }
  };
} // namespace

UMBRIEL_TEST(syntheticReadyFeatureDemandAndLockUseFreshEpoch) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, UMBRIEL_AUDIO_SYNTHETIC, {});
  size_t updates = 0;
  supervisor.changed = [&] { ++updates; };
  CHECK(!supervisor.running());
  CHECK(!supervisor.demanded());
  supervisor.setDemand(true);
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().input().available; }));
  CHECK(supervisor.receiver().ready());
  CHECK(supervisor.receiver().input().features.rms > 0.35F);
  CHECK(updates > 0);
  const auto epoch = supervisor.receiver().epoch();
  supervisor.setSessionActive(false);
  CHECK_EQ(supervisor.receiver().input(), Input{});
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
  supervisor.setSessionActive(true);
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().input().available; }));
  CHECK(supervisor.receiver().epoch() > epoch);
  supervisor.setDemand(false);
  CHECK_EQ(supervisor.receiver().input(), Input{});
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
  CHECK(!supervisor.receiver().decaying());
  struct sigaction action{};
  CHECK_EQ(sigaction(SIGCHLD, nullptr, &action), 0);
  CHECK(action.sa_handler != SIG_DFL && action.sa_handler != SIG_IGN);
}

UMBRIEL_TEST(steadySyntheticSilenceDoesNotCreateInputRevisions) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, UMBRIEL_AUDIO_SYNTHETIC, {"--silence"});
  supervisor.setDemand(true);
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().input().available; }));
  const auto revision = supervisor.receiver().revision();
  const auto sequence = supervisor.receiver().sequence();
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().sequence() >= sequence + 4; }));
  CHECK_EQ(supervisor.receiver().revision(), revision);
  CHECK_EQ(supervisor.receiver().input().features, Features{});
  supervisor.setDemand(false);
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
}

UMBRIEL_TEST(missingExecutableIsUnavailableAndCanStopWithoutRetryDemand) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, "/definitely-absent-umbriel-audio", {});
  supervisor.setDemand(true);
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
  CHECK(!supervisor.receiver().ready());
  CHECK(!supervisor.receiver().input().available);
  supervisor.setDemand(false);
  CHECK(!supervisor.demanded());
}

UMBRIEL_TEST(unresponsiveHelperEscalatesThroughTermAndKillWithoutWaitStatus) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, UMBRIEL_AUDIO_SYNTHETIC, {"--ignore-eof"});
  const auto started = std::chrono::steady_clock::now();
  supervisor.setDemand(true);
  CHECK(supervisor.running());
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= std::chrono::milliseconds(400));
  CHECK(elapsed < std::chrono::seconds(2));
  supervisor.setDemand(false);
}

UMBRIEL_TEST(serviceSharesAcquisitionCapsDemandAndInspectionIsPure) {
  Loop loop;
  Service service(loop.loop);
  for (int i = 0; i < 5; ++i) {
    const auto name = std::to_string(i);
    CHECK(
        service.define(name, {.configuration = {}, .executable = UMBRIEL_AUDIO_SYNTHETIC, .arguments = {"--silence"}})
    );
    CHECK(service.inspect(name) == nullptr);
  }
  CHECK_EQ(service.demandedSources(), 0U);
  CHECK(service.acquire("0", 1));
  CHECK(service.acquire("0", 1));
  CHECK(service.acquire("0", 2));
  CHECK_EQ(service.consumers("0"), 2U);
  CHECK_EQ(service.demandedSources(), 1U);
  const auto* shared = service.inspect("0");
  CHECK(shared != nullptr);
  service.release("0", 1);
  CHECK_EQ(service.inspect("0"), shared);
  CHECK_EQ(service.demandedSources(), 1U);
  for (int i = 1; i < 4; ++i) {
    CHECK(service.acquire(std::to_string(i), 1));
  }
  CHECK_EQ(service.demandedSources(), 4U);
  CHECK(!service.acquire("4", 1));
  CHECK(service.inspect("4") == nullptr);
  CHECK(dispatchUntil(loop.loop, [&] { return shared->input().available; }));
  service.setSessionActive(false);
  CHECK_EQ(shared->input(), Input{});
  service.release("0", 2);
  CHECK_EQ(service.demandedSources(), 3U);
  CHECK(service.acquire("4", 1));
  CHECK_EQ(service.demandedSources(), 4U);
  for (int i = 1; i < 5; ++i) {
    service.release(std::to_string(i), 1);
  }
  CHECK_EQ(service.demandedSources(), 0U);
}

UMBRIEL_TEST(heartbeatsCannotPostponeStaleMeasurementWatchdog) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, UMBRIEL_AUDIO_SYNTHETIC, {"--heartbeat-only"});
  supervisor.setDemand(true);
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().input().available; }));
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.receiver().input().available; }, 500));
  CHECK(dispatchUntil(loop.loop, [&] { return supervisor.receiver().input().features == Features{}; }, 300));
  CHECK(supervisor.running());
  CHECK(supervisor.receiver().ready());
  supervisor.setDemand(false);
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }));
}

UMBRIEL_TEST(preReadyChatterCannotPostponeNegotiationDeadline) {
  Loop loop;
  Supervisor supervisor(loop.loop, Configuration{}, UMBRIEL_AUDIO_SYNTHETIC, {"--chatter-before-ready"});
  supervisor.setDemand(true);
  CHECK(supervisor.running());
  CHECK(dispatchUntil(loop.loop, [&] { return !supervisor.running(); }, 1500));
  CHECK(!supervisor.receiver().ready());
  supervisor.setDemand(false);
}

int main() {
  // Exercise pidfd observation with native Xwayland's process-wide child reaper.
  umbriel::installChildReaper();
  return RUN_TESTS();
}
