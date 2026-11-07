#include "server/event_pacer.h"

#include "check.h"
#include "core/log.h"

#include <memory>
#include <stdexcept>
#include <string_view>
#include <wayland-server-core.h>

using umbriel::EventPacer;

namespace {

  enum class SourceFailure { None, CreateTimer, AddIdle, ArmTimer };

  SourceFailure gInjectedFailure = SourceFailure::None;
  int gLoggedErrors = 0;

  // Long enough that a test never races the interval it is not waiting for; endInterval blocks until it passes.
  constexpr int kIntervalMs = 20;

  class PacerFixture {
  public:
    explicit PacerFixture(SourceFailure failure = SourceFailure::None) : m_loop(wl_event_loop_create()) {
      if (m_loop == nullptr) {
        throw std::runtime_error("could not create the event pacer test loop");
      }
      gInjectedFailure = failure;
      gLoggedErrors = 0;
      m_pacer = std::make_unique<EventPacer>(m_loop, kIntervalMs, [this] { ++emissions; });
    }
    ~PacerFixture() {
      gInjectedFailure = SourceFailure::None;
      m_pacer.reset();
      wl_event_loop_destroy(m_loop);
    }

    PacerFixture(const PacerFixture&) = delete;
    PacerFixture& operator=(const PacerFixture&) = delete;

    void schedule() const { m_pacer->schedule(); }
    void destroyPacer() { m_pacer.reset(); }
#ifdef UMBRIEL_HARNESS
    void setIntervalHeld(bool held) const { m_pacer->setIntervalHeld(held); }
#endif
    void idlePass() const { wl_event_loop_dispatch_idle(m_loop); }
    void endInterval() const { wl_event_loop_dispatch(m_loop, 4 * kIntervalMs); }

    int emissions = 0;

  private:
    wl_event_loop* m_loop;
    std::unique_ptr<EventPacer> m_pacer;
  };

} // namespace

namespace detail {
  void logMessage(LogLevel level, const char* /*section*/, std::string_view /*msg*/) {
    if (level == LogLevel::Error) {
      ++gLoggedErrors;
    }
  }
} // namespace detail

extern "C" wl_event_source* __real_wl_event_loop_add_timer(wl_event_loop*, wl_event_loop_timer_func_t, void*);
extern "C" wl_event_source* __real_wl_event_loop_add_idle(wl_event_loop*, wl_event_loop_idle_func_t, void*);
extern "C" int __real_wl_event_source_timer_update(wl_event_source*, int);

extern "C" wl_event_source*
__wrap_wl_event_loop_add_timer(wl_event_loop* loop, wl_event_loop_timer_func_t func, void* data) {
  return gInjectedFailure == SourceFailure::CreateTimer ? nullptr : __real_wl_event_loop_add_timer(loop, func, data);
}

extern "C" wl_event_source*
__wrap_wl_event_loop_add_idle(wl_event_loop* loop, wl_event_loop_idle_func_t func, void* data) {
  return gInjectedFailure == SourceFailure::AddIdle ? nullptr : __real_wl_event_loop_add_idle(loop, func, data);
}

extern "C" int __wrap_wl_event_source_timer_update(wl_event_source* source, int ms) {
  return gInjectedFailure == SourceFailure::ArmTimer ? -1 : __real_wl_event_source_timer_update(source, ms);
}

UMBRIEL_TEST(aChangeAfterAQuietIntervalEmitsOnTheNextIdlePass) {
  PacerFixture fixture;
  fixture.schedule();
  CHECK_EQ(fixture.emissions, 0);
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 1);
}

UMBRIEL_TEST(changesInOneIdlePassEmitOnce) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 1);
}

UMBRIEL_TEST(changesInLaterIdlePassesWaitForTheIntervalToEnd) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.idlePass();
  for (int pass = 0; pass < 5; ++pass) {
    fixture.schedule();
    fixture.idlePass();
  }
  CHECK_EQ(fixture.emissions, 1);
  fixture.endInterval();
  CHECK_EQ(fixture.emissions, 2);
}

UMBRIEL_TEST(anEmissionAtAnIntervalEndStartsTheNextInterval) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.idlePass();
  fixture.schedule();
  fixture.endInterval();
  CHECK_EQ(fixture.emissions, 2);
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 2);
  fixture.endInterval();
  CHECK_EQ(fixture.emissions, 3);
}

UMBRIEL_TEST(anIntervalWithoutChangesEmitsNothingAndEndsThePacing) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.idlePass();
  fixture.endInterval();
  CHECK_EQ(fixture.emissions, 1);
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 2);
}

UMBRIEL_TEST(destroyingThePacerDropsPendingEmissions) {
  PacerFixture queued;
  queued.schedule();
  queued.destroyPacer();
  queued.idlePass();
  CHECK_EQ(queued.emissions, 0);

  PacerFixture waiting;
  waiting.schedule();
  waiting.idlePass();
  waiting.schedule();
  waiting.destroyPacer();
  waiting.idlePass();
  CHECK_EQ(waiting.emissions, 1);
}

UMBRIEL_TEST(aTimerThatCannotBeCreatedLogsOnceAndEmitsUnpaced) {
  PacerFixture fixture(SourceFailure::CreateTimer);
  CHECK_EQ(gLoggedErrors, 1);
  fixture.schedule();
  fixture.idlePass();
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 2);
  CHECK_EQ(gLoggedErrors, 1);
}

UMBRIEL_TEST(anIdleSourceThatCannotBeRegisteredLogsTheDroppedChange) {
  PacerFixture fixture(SourceFailure::AddIdle);
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 0);
  CHECK_EQ(gLoggedErrors, 1);
}

UMBRIEL_TEST(aTimerThatCannotBeArmedLogsEachUnpacedEmission) {
  PacerFixture fixture(SourceFailure::ArmTimer);
  fixture.schedule();
  fixture.idlePass();
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 2);
  CHECK_EQ(gLoggedErrors, 2);
}

#ifdef UMBRIEL_HARNESS
UMBRIEL_TEST(aHoldDefersAChangeAlreadyQueuedForTheIdlePass) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.setIntervalHeld(true);
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 0);
  fixture.setIntervalHeld(false);
  CHECK_EQ(fixture.emissions, 1);
}

UMBRIEL_TEST(aHoldDefersChangesEvenWithNoIntervalRunning) {
  PacerFixture fixture;
  fixture.setIntervalHeld(true);
  fixture.schedule();
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 0);
  fixture.setIntervalHeld(false);
  CHECK_EQ(fixture.emissions, 1);
}

UMBRIEL_TEST(aHeldIntervalOutlastsItsTimer) {
  PacerFixture fixture;
  fixture.schedule();
  fixture.idlePass();
  fixture.setIntervalHeld(true);
  fixture.schedule();
  fixture.endInterval();
  CHECK_EQ(fixture.emissions, 1);
  fixture.setIntervalHeld(false);
  CHECK_EQ(fixture.emissions, 2);
}

UMBRIEL_TEST(releasingAHoldWithoutChangesEmitsNothingAndEndsThePacing) {
  PacerFixture fixture;
  fixture.setIntervalHeld(true);
  fixture.setIntervalHeld(false);
  CHECK_EQ(fixture.emissions, 0);
  fixture.schedule();
  fixture.idlePass();
  CHECK_EQ(fixture.emissions, 1);
}
#endif

int main() { return RUN_TESTS(); }
