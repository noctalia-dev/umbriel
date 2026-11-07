#include "server/event_pacer.h"

#include "check.h"

#include <memory>
#include <stdexcept>
#include <wayland-server-core.h>

using umbriel::EventPacer;

namespace {

  // Long enough that a test never races the interval it is not waiting for; endInterval blocks until it passes.
  constexpr int kIntervalMs = 20;

  class PacerFixture {
  public:
    PacerFixture()
        : m_loop(wl_event_loop_create()),
          m_pacer(std::make_unique<EventPacer>(m_loop, kIntervalMs, [this] { ++emissions; })) {
      if (m_loop == nullptr) {
        throw std::runtime_error("could not create the event pacer test loop");
      }
    }
    ~PacerFixture() {
      m_pacer.reset();
      wl_event_loop_destroy(m_loop);
    }

    PacerFixture(const PacerFixture&) = delete;
    PacerFixture& operator=(const PacerFixture&) = delete;

    void schedule() const { m_pacer->schedule(); }
    void destroyPacer() { m_pacer.reset(); }
    void idlePass() const { wl_event_loop_dispatch_idle(m_loop); }
    void endInterval() const { wl_event_loop_dispatch(m_loop, 4 * kIntervalMs); }

    int emissions = 0;

  private:
    wl_event_loop* m_loop;
    std::unique_ptr<EventPacer> m_pacer;
  };

} // namespace

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

int main() { return RUN_TESTS(); }
