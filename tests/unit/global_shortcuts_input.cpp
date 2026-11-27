#include "input/global_shortcuts_input.h"

#include "check.h"

#include <cmath>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace umbriel;

namespace {
  struct Event {
    GlobalShortcutBinding binding;
    bool pressed;
    uint32_t time;
  };

  class Fixture {
  public:
    Fixture()
        : input(registry, [this](const auto& binding, bool pressed, uint32_t time) {
            events.push_back({binding, pressed, time});
          }) {}

    GlobalShortcutSessionId bind(
        std::string_view chord = "CTRL+m", std::string_view application = "org.test.App", std::string_view id = "talk"
    ) {
      const auto session = registry.createSession(application).value();
      const auto pending = registry.beginBind(session, {{std::string(id), "Action", std::string(chord)}}).value();
      if (!registry.commitBind(
              session, GlobalShortcutGrant{std::string(32, 'a'), std::string(application), pending.approval, {}}
          )) {
        throw std::runtime_error("could not bind the test shortcut");
      }
      return session;
    }

    bool press(
        const void* source, uint32_t code, uint32_t time, xkb_keysym_t keysym = XKB_KEY_m,
        uint32_t modifiers = WLR_MODIFIER_CTRL, xkb_keysym_t raw = XKB_KEY_m
    ) {
      input.observeKey(source, code, time, true);
      return input.handlePress(source, code, time, std::array{keysym}, raw, modifiers);
    }

    GlobalShortcutsRegistry registry;
    std::vector<Event> events;
    GlobalShortcutsInput input;
    const int first = 1;
    const int second = 2;
  };
} // namespace

UMBRIEL_TEST(activationsPairWithTheirTriggerReleaseAndKeepTheEventTimestamp) {
  Fixture fixture;
  const auto session = fixture.bind();
  CHECK(fixture.press(&fixture.first, 50, 100));
  CHECK_EQ(fixture.events.size(), size_t{1});
  CHECK(fixture.events.front().pressed);
  CHECK_EQ(fixture.events.front().time, uint32_t{100});
  CHECK_EQ(fixture.events.front().binding.session, session);
  CHECK_EQ(fixture.events.front().binding.id, "talk");
  CHECK(!fixture.input.observeKey(&fixture.first, 29, 110, false));
  CHECK_EQ(fixture.events.size(), size_t{1});
  CHECK(fixture.input.observeKey(&fixture.first, 50, 120, false));
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(!fixture.events.back().pressed);
  CHECK_EQ(fixture.events.back().time, uint32_t{120});
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 130, false));
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(fixture.press(&fixture.first, 50, std::numeric_limits<uint32_t>::max()));
  CHECK(fixture.input.observeKey(&fixture.first, 50, 0, false));
  CHECK_EQ(fixture.events.back().time, uint32_t{0});
}

UMBRIEL_TEST(unhandledKeysAndRepeatedPressesCannotCreateExtraActivations) {
  Fixture fixture;
  fixture.bind();
  CHECK(!fixture.input.handlePress(
      &fixture.first, 50, 1, std::array<xkb_keysym_t, 1>{XKB_KEY_m}, XKB_KEY_m, WLR_MODIFIER_CTRL
  ));
  CHECK(!fixture.press(&fixture.first, 30, 2, XKB_KEY_a, 0, XKB_KEY_a));
  CHECK(!fixture.input.observeKey(&fixture.first, 30, 3, false));
  CHECK(fixture.events.empty());
  CHECK(fixture.press(&fixture.first, 50, 4));
  CHECK(fixture.press(&fixture.first, 50, 5));
  CHECK(fixture.press(&fixture.first, 50, 6, XKB_KEY_n, 0, XKB_KEY_n));
  CHECK_EQ(fixture.events.size(), size_t{1});
  CHECK(fixture.input.observeKey(&fixture.first, 50, 7, false));
  CHECK_EQ(fixture.events.size(), size_t{2});
}

UMBRIEL_TEST(earlyRoutingGuardsKeepObservedHeldKeysFromActivatingOnRepeat) {
  Fixture fixture;
  fixture.bind();
  // A guard owns this press, so matching is skipped for this event.
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 1, true));
  CHECK(!fixture.press(&fixture.first, 50, 2));
  CHECK(fixture.events.empty());
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 3, false));
  CHECK(fixture.press(&fixture.first, 50, 4));
  fixture.input.suspend(5);
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(!fixture.events.back().pressed);
  CHECK(fixture.press(&fixture.first, 50, 6));
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(fixture.input.observeKey(&fixture.first, 50, 7, false));
  CHECK(fixture.press(&fixture.first, 50, 8));
  CHECK_EQ(fixture.events.size(), size_t{3});
}

UMBRIEL_TEST(sourceIdentitySeparatesIdenticalKeycodesAndRemovalRetiresOnlyItsOwnActivations) {
  Fixture fixture;
  fixture.bind();
  CHECK(fixture.press(&fixture.first, 50, 1));
  CHECK(fixture.press(&fixture.second, 50, 2));
  CHECK_EQ(fixture.events.size(), size_t{2});
  fixture.input.forgetSource(&fixture.first, 3);
  CHECK_EQ(fixture.events.size(), size_t{3});
  CHECK(!fixture.events.back().pressed);
  CHECK_EQ(fixture.events.back().time, uint32_t{3});
  fixture.input.forgetSource(&fixture.first, 4);
  CHECK_EQ(fixture.events.size(), size_t{3});
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 5, false));
  CHECK(fixture.input.observeKey(&fixture.second, 50, 6, false));
  CHECK_EQ(fixture.events.size(), size_t{4});
}

UMBRIEL_TEST(suspensionRetiresEveryActivationOnceAndPreservesUnconsumedPresses) {
  Fixture fixture;
  fixture.bind();
  CHECK(fixture.press(&fixture.first, 50, 1));
  CHECK(fixture.press(&fixture.second, 50, 2));
  CHECK(!fixture.input.observeKey(&fixture.first, 30, 3, true));
  fixture.input.suspend(4);
  fixture.input.suspend(5);
  CHECK_EQ(fixture.events.size(), size_t{4});
  if (fixture.events.size() != 4) {
    return;
  }
  CHECK_EQ(fixture.events.at(2).time, uint32_t{4});
  CHECK_EQ(fixture.events.at(3).time, uint32_t{4});
  CHECK(!fixture.input.handlePress(
      &fixture.first, 30, 6, std::array<xkb_keysym_t, 1>{XKB_KEY_m}, XKB_KEY_m, WLR_MODIFIER_CTRL
  ));
  CHECK(!fixture.input.observeKey(&fixture.first, 30, 7, false));
  CHECK(fixture.input.observeKey(&fixture.first, 50, 8, false));
  CHECK(fixture.input.observeKey(&fixture.second, 50, 9, false));
  CHECK_EQ(fixture.events.size(), size_t{4});
}

UMBRIEL_TEST(rebindingRetiresChangedChordsWhilePreservingOtherHeldActivations) {
  Fixture fixture;
  fixture.bind();
  fixture.bind("F16", "org.test.Other");
  CHECK(fixture.press(&fixture.first, 50, 1));
  CHECK(fixture.press(&fixture.second, 100, 2, XKB_KEY_F16, 0, XKB_KEY_F16));
  CHECK(fixture.registry.configure({{"org.test.App", {{"talk", "CTRL+n"}}}}, {}, 0));
  fixture.input.reconcile(3);
  fixture.input.reconcile(4);
  CHECK_EQ(fixture.events.size(), size_t{3});
  CHECK_EQ(fixture.events.back().binding.id, "talk");
  CHECK_EQ(fixture.events.back().time, uint32_t{3});
  CHECK(!fixture.events.back().pressed);
  CHECK(fixture.press(&fixture.first, 50, 5, XKB_KEY_n, WLR_MODIFIER_CTRL, XKB_KEY_n));
  CHECK_EQ(fixture.events.size(), size_t{3});
  CHECK(fixture.input.observeKey(&fixture.first, 50, 6, false));
  CHECK(fixture.press(&fixture.first, 50, 7, XKB_KEY_n, WLR_MODIFIER_CTRL, XKB_KEY_n));
  CHECK_EQ(fixture.events.size(), size_t{4});
  CHECK(fixture.input.observeKey(&fixture.second, 100, 8, false));
  CHECK_EQ(fixture.events.size(), size_t{5});
}

UMBRIEL_TEST(ownerClosurePromotesWaitersWithoutReplayingAnAlreadyHeldKey) {
  Fixture fixture;
  const auto owner = fixture.bind();
  const auto waiter = fixture.bind("CTRL+m", "org.test.Other");
  CHECK(fixture.press(&fixture.first, 50, 1));
  CHECK(fixture.registry.closeSession(owner));
  fixture.input.reconcile(2);
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(!fixture.events.back().pressed);
  CHECK_EQ(fixture.events.back().binding.session, owner);
  CHECK(fixture.press(&fixture.first, 50, 3));
  CHECK_EQ(fixture.events.size(), size_t{2});
  CHECK(fixture.input.observeKey(&fixture.first, 50, 4, false));
  CHECK(fixture.press(&fixture.first, 50, 5));
  CHECK_EQ(fixture.events.size(), size_t{3});
  CHECK_EQ(fixture.events.back().binding.session, waiter);
}

UMBRIEL_TEST(matchingChoosesTheEarliestSuccessfulBindingAcrossEffectiveAndRawAliases) {
  Fixture fixture;
  const auto later = fixture.registry.createSession("org.test.Other").value();
  const auto first = fixture.bind("CTRL+Cyrillic_ef");
  const auto pending = fixture.registry.beginBind(later, {{"latin", "Latin", "CTRL+a"}}).value();
  CHECK(fixture.registry.commitBind(
      later, GlobalShortcutGrant{std::string(32, 'b'), "org.test.Other", pending.approval, {}}
  ));
  CHECK(fixture.press(&fixture.first, 30, 1, XKB_KEY_Cyrillic_EF, WLR_MODIFIER_CTRL | WLR_MODIFIER_CAPS, XKB_KEY_a));
  CHECK_EQ(fixture.events.front().binding.session, first);
  CHECK(fixture.input.observeKey(&fixture.first, 30, 2, false));
  CHECK(fixture.registry.closeSession(first));
  CHECK(fixture.press(&fixture.first, 30, 3, XKB_KEY_Cyrillic_ef, WLR_MODIFIER_CTRL, XKB_KEY_a));
  CHECK_EQ(fixture.events.back().binding.session, later);
}

UMBRIEL_TEST(restoredHistoryAndPendingOrUnassignedEntriesCannotActivate) {
  Fixture fixture;
  const auto id = fixture.registry.createSession("org.test.App").value();
  const GlobalShortcut shortcut{"talk", "Action", parseGlobalShortcutTrigger("CTRL+m")};
  CHECK(
      fixture.registry.restore(id, GlobalShortcutGrant{std::string(32, 'a'), "org.test.App", {shortcut}, {shortcut}})
  );
  CHECK(!fixture.press(&fixture.first, 50, 1));
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 2, false));
  CHECK(fixture.registry.beginBind(id, {{"talk", "Action", "CTRL+m"}}).has_value());
  CHECK(!fixture.press(&fixture.first, 50, 3));
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 4, false));
  CHECK(fixture.registry.commitBind(id));
  fixture.input.observeKey(&fixture.first, 50, 5, true);
  CHECK(!fixture.input.handlePress(&fixture.first, 50, 5, {}, XKB_KEY_m, WLR_MODIFIER_CTRL));
  CHECK(!fixture.input.observeKey(&fixture.first, 50, 6, false));
  CHECK(fixture.registry.configure({{"org.test.App", {{"talk", ""}}}}, {}, 0));
  CHECK(!fixture.press(&fixture.first, 50, 7));
  CHECK(fixture.events.empty());
}

int main() { return RUN_TESTS(); }
