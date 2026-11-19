#include "check.h"
#include "input/global_shortcuts.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <array>
#include <stdexcept>
#include <utility>

using namespace umbriel;

namespace {
  GlobalShortcutTrigger trigger(std::string_view chord) { return parseGlobalShortcutTrigger(chord).value(); }

  GlobalShortcut shortcut(std::string id, std::string_view chord = {}) {
    return {std::move(id), "Action", chord.empty() ? std::nullopt : std::optional{trigger(chord)}};
  }

  GlobalShortcutGrant grant(std::string application, std::vector<GlobalShortcut> approved) {
    return {std::string(32, 'a'), std::move(application), std::move(approved), {}};
  }

  GlobalShortcutSessionId create(GlobalShortcutsRegistry& registry, std::string_view application = "org.test.App") {
    const auto id = registry.createSession(application);
    if (!id) {
      throw std::runtime_error("could not create a test shortcut session");
    }
    return *id;
  }

  void approvePending(GlobalShortcutsRegistry& registry, GlobalShortcutSessionId id) {
    const auto state = registry.session(id).value();
    std::vector<GlobalShortcut> approved;
    approved.reserve(state.shortcuts.size());
    for (const auto& shortcut : state.shortcuts) {
      approved.push_back({shortcut.id, shortcut.description, shortcut.candidate});
    }
    CHECK(registry.commitBind(id, grant(state.application, std::move(approved))));
  }
} // namespace

UMBRIEL_TEST(sessionIdentityAndCapacityAreValidatedWithoutReusingHandles) {
  GlobalShortcutsRegistry registry;
  CHECK(!registry.createSession(""));
  CHECK(!registry.createSession(std::string(257, 'a')));
  CHECK(!registry.createSession(std::string{"bad\0id", 6}));
  CHECK(!registry.createSession(std::string(1, '\xff')));
  for (size_t index = 0; index < kMaxGlobalShortcutSessions; ++index) {
    CHECK(registry.createSession("org.test.App").has_value());
  }
  CHECK(!registry.createSession("org.test.App"));
  CHECK(registry.closeSession(1));
  const auto next = registry.createSession("org.test.App");
  CHECK(next && *next > kMaxGlobalShortcutSessions);
  CHECK(!registry.closeSession(0));
  CHECK(!registry.list(0));
  CHECK(!registry.beginBind(0, {}));
  CHECK(!registry.commitBind(0));
  CHECK(!registry.cancelBind(0));
}

UMBRIEL_TEST(aClaimedApplicationNameCannotCommitWithoutItsApprovalScope) {
  GlobalShortcutsRegistry registry;
  const auto id = create(registry);
  const auto pending = registry.beginBind(id, {{"talk", "Talk", "CTRL+m"}});
  CHECK(pending && pending->approval.size() == 1);
  CHECK(!registry.commitBind(id));
  CHECK(registry.list(id)->empty());
  CHECK(!registry.commitBind(id, grant("org.test.Other", {shortcut("talk", "CTRL+m")})));
  CHECK(!registry.commitBind(id, grant("org.test.App", {shortcut("talk", "CTRL+n")})));
  CHECK(!registry.commitBind(id, grant("org.test.App", {shortcut("other", "CTRL+m")})));
  CHECK(registry.commitBind(id, grant("org.test.App", {shortcut("talk", "CTRL+m")})));
  CHECK_EQ(registry.list(id)->size(), size_t{1});
  CHECK(registry.list(id)->front().trigger == std::optional{trigger("CTRL+m")});
  CHECK(registry.takeChanges().changed.empty());
}

UMBRIEL_TEST(configuredStoredAndPreferredCandidatesKeepTheirPrecedence) {
  GlobalShortcutsRegistry registry;
  CHECK(registry.configure({{"org.test.App", {{"override", "Mod+Shift+M"}, {"disabled", ""}}}}, {}, WLR_MODIFIER_LOGO));
  auto approved = grant(
      "org.test.App",
      {shortcut("override", "F15"), shortcut("stored", "F16"), shortcut("disabled", "F17"), shortcut("unset")}
  );
  approved.previous = {shortcut("stored", "F16")};
  const auto id = create(registry);
  CHECK(registry.restore(id, approved));
  CHECK_EQ(registry.list(id)->size(), size_t{1});
  const auto pending = registry.beginBind(
      id,
      {{"override", "Override", "F20"},
       {"stored", "Updated label", "F21"},
       {"disabled", "Disabled", "F22"},
       {"unset", "Unassigned", "F23"},
       {"preferred", "Preferred", "F18"},
       {"typing", "Typing", "SHIFT+a"}}
  );
  CHECK(pending.has_value());
  if (!pending) {
    return;
  }
  CHECK_EQ(pending->approval.size(), size_t{2});
  CHECK_EQ(pending->shortcuts[0].source, GlobalShortcutSource::Config);
  CHECK_EQ(pending->shortcuts[0].candidate, std::optional{trigger("LOGO+SHIFT+m")});
  CHECK_EQ(pending->shortcuts[1].source, GlobalShortcutSource::Stored);
  CHECK_EQ(pending->shortcuts[1].candidate, std::optional{trigger("F16")});
  CHECK_EQ(pending->shortcuts[2].conflict, GlobalShortcutConflict::Disabled);
  CHECK(!pending->shortcuts[2].candidate);
  CHECK(!pending->shortcuts[3].candidate);
  CHECK_EQ(pending->shortcuts[4].source, GlobalShortcutSource::Preferred);
  CHECK_EQ(pending->shortcuts[5].conflict, GlobalShortcutConflict::Unassigned);
  approved.approved.insert(approved.approved.end(), pending->approval.begin(), pending->approval.end());
  CHECK(registry.commitBind(id, approved));
  CHECK_EQ(registry.list(id)->size(), size_t{3});
  CHECK(registry.list(id)->at(1).description == "Updated label");
  CHECK_EQ(approved.approved.front().trigger, std::optional{trigger("F15")});
}

UMBRIEL_TEST(onlySuccessfullyRememberedBindingsAreListedBeforeBinding) {
  GlobalShortcutsRegistry registry;
  auto approved = grant("org.test.App", {shortcut("old", "F16"), shortcut("unset")});
  approved.previous = {shortcut("old", "F16")};
  const auto id = create(registry);
  CHECK(registry.restore(id, approved));
  CHECK(registry.list(id)->front().id == "old");
  CHECK(registry.beginBind(id, {{"unset", "Unassigned", "F17"}}).has_value());
  CHECK(registry.commitBind(id));
  CHECK(registry.list(id)->empty());
  const auto empty = create(registry);
  CHECK(registry.beginBind(empty, {}).has_value());
  CHECK(registry.commitBind(empty));
  CHECK(registry.list(empty)->empty());
}

UMBRIEL_TEST(bindingAndRestorationAttemptsCannotBeRepeatedOrReordered) {
  GlobalShortcutsRegistry registry;
  const auto listed = create(registry);
  CHECK(registry.list(listed)->empty());
  CHECK(!registry.restore(listed, grant("org.test.App", {})));
  const auto failed = create(registry);
  CHECK(!registry.restore(failed, std::nullopt));
  CHECK(!registry.restore(failed, grant("org.test.App", {})));
  CHECK(registry.beginBind(failed, {{"a", "Action", "F16"}}).has_value());
  CHECK(registry.cancelBind(failed));
  CHECK(!registry.cancelBind(failed));
  CHECK(!registry.beginBind(failed, {}));
  CHECK(!registry.commitBind(failed));
  const auto duplicate = create(registry);
  CHECK(!registry.beginBind(duplicate, {{"a", "One", "F16"}, {"a", "Two", "F17"}}));
  CHECK(!registry.beginBind(duplicate, {}));
  const auto invalid = create(registry);
  CHECK(!registry.beginBind(invalid, {{"", "Action", "F16"}}));
  const auto oversized = create(registry);
  CHECK(!registry.beginBind(oversized, {{"a", std::string(1025, 'x'), "F16"}}));
  const auto invalidUtf8 = create(registry);
  CHECK(!registry.beginBind(invalidUtf8, {{"a", "Action", std::string(1, '\xff')}}));
}

UMBRIEL_TEST(shortcutCountLimitsApplyBeforeApprovalAndAllowTheExactBoundary) {
  GlobalShortcutsRegistry registry;
  std::vector<GlobalShortcutRequest> requests;
  requests.reserve(kMaxGlobalShortcuts + 1);
  for (size_t index = 0; index <= kMaxGlobalShortcuts; ++index) {
    requests.push_back({std::format("action-{}", index), "Action", "F16"});
  }
  const auto oversized = create(registry);
  CHECK(!registry.beginBind(oversized, requests));
  CHECK(!registry.beginBind(oversized, {}));
  requests.pop_back();
  const auto exact = create(registry);
  const auto pending = registry.beginBind(exact, requests);
  CHECK(pending && pending->approval.size() == kMaxGlobalShortcuts);
  approvePending(registry, exact);
  CHECK_EQ(registry.list(exact)->size(), size_t{1});
}

UMBRIEL_TEST(reservedChordsAreUnavailableFromEveryCandidateSource) {
  GlobalShortcutsRegistry registry;
  auto configured = trigger("LOGO+q");
  configured.modifiers |= WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2;
  configured.keysym = XKB_KEY_Q;
  CHECK(registry.configure(
      {{"org.test.App",
        {{"escape", "Ctrl+Alt+Escape"},
         {"vt", "Ctrl+Alt+Shift+F2"},
         {"device", "XF86Switch_VT_1"},
         {"configured", "Mod+Q"}}}},
      std::array{configured}, WLR_MODIFIER_LOGO
  ));
  const auto id = create(registry);
  CHECK(registry
            .beginBind(
                id,
                {{"escape", "Escape", "F20"},
                 {"vt", "VT", "F21"},
                 {"device", "Device", "F22"},
                 {"configured", "Configured", "F23"}}
            )
            .has_value());
  approvePending(registry, id);
  CHECK(registry.list(id)->empty());
  const auto state = registry.session(id);
  for (const auto& shortcut : state->shortcuts) {
    CHECK_EQ(shortcut.conflict, GlobalShortcutConflict::Reserved);
  }
  const auto stored = create(registry, "org.test.Stored");
  CHECK(registry.restore(stored, grant("org.test.Stored", {shortcut("escape", "CTRL+ALT+Escape")})));
  CHECK(registry.beginBind(stored, {{"escape", "Escape", "F20"}}).has_value());
  CHECK(registry.commitBind(stored));
  CHECK(registry.list(stored)->empty());
  const auto preferred = create(registry, "org.test.Preferred");
  CHECK(registry.beginBind(preferred, {{"escape", "Escape", "CTRL+ALT+Escape"}}).has_value());
  approvePending(registry, preferred);
  CHECK(registry.list(preferred)->empty());
}

UMBRIEL_TEST(liveBindOrderWinsAndOwnerClosurePromotesApprovedWaiters) {
  GlobalShortcutsRegistry registry;
  auto remembered = grant("org.test.App", {shortcut("talk", "F16")});
  remembered.previous = remembered.approved;
  const auto older = create(registry);
  CHECK(registry.restore(older, remembered));
  CHECK_EQ(registry.list(older)->size(), size_t{1});
  const auto owner = create(registry);
  CHECK(registry.beginBind(owner, {{"talk", "Talk", "F16"}}).has_value());
  approvePending(registry, owner);
  CHECK_EQ(registry.list(owner)->size(), size_t{1});
  CHECK(registry.beginBind(older, {{"talk", "Talk", "F17"}}).has_value());
  CHECK(registry.commitBind(older));
  CHECK(registry.list(older)->empty());
  const auto later = create(registry, "org.test.Other");
  const auto pending = registry.beginBind(later, {{"talk", "Talk", "F16"}});
  CHECK(pending && pending->shortcuts.front().conflict == GlobalShortcutConflict::InUse);
  approvePending(registry, later);
  CHECK(registry.list(later)->empty());
  CHECK(registry.closeSession(owner));
  CHECK_EQ(registry.list(older)->size(), size_t{1});
  CHECK(registry.list(later)->empty());
  CHECK_EQ(registry.takeChanges().changed, std::vector{older});
  CHECK(registry.closeSession(older));
  CHECK_EQ(registry.list(later)->size(), size_t{1});
  CHECK_EQ(registry.takeChanges().changed, std::vector{later});
}

UMBRIEL_TEST(pendingApprovalDoesNotReserveKeysAndDuplicateChordsHaveOneOwner) {
  GlobalShortcutsRegistry registry;
  const auto pending = create(registry);
  CHECK(registry.beginBind(pending, {{"waiting", "Waiting", "F16"}}).has_value());
  const auto owner = create(registry, "org.test.Other");
  CHECK(registry.beginBind(owner, {{"first", "First", "F16"}, {"second", "Second", "F16"}}).has_value());
  approvePending(registry, owner);
  CHECK_EQ(registry.list(owner)->size(), size_t{1});
  CHECK(registry.list(owner)->front().id == "first");
  CHECK(registry.session(owner)->shortcuts[1].conflict == GlobalShortcutConflict::InUse);
  approvePending(registry, pending);
  CHECK(registry.list(pending)->empty());
}

UMBRIEL_TEST(configReloadPublishesChangedSubsetsAndCancelsPendingApproval) {
  GlobalShortcutsRegistry registry;
  const auto owner = create(registry);
  CHECK(registry.beginBind(owner, {{"talk", "Talk", "F16"}}).has_value());
  approvePending(registry, owner);
  const auto waiter = create(registry, "org.test.Other");
  CHECK(registry.beginBind(waiter, {{"talk", "Talk", "F16"}}).has_value());
  approvePending(registry, waiter);
  const auto pending = create(registry, "org.test.Pending");
  CHECK(registry.beginBind(pending, {{"wait", "Wait", "F20"}}).has_value());
  CHECK(registry.configure({{"org.test.App", {{"talk", ""}}}}, {}, WLR_MODIFIER_LOGO));
  CHECK(registry.list(owner)->empty());
  CHECK_EQ(registry.list(waiter)->size(), size_t{1});
  const auto changes = registry.takeChanges();
  CHECK_EQ(changes.changed, (std::vector{owner, waiter}));
  CHECK_EQ(changes.cancelled, std::vector{pending});
  CHECK(registry.takeChanges().changed.empty());
  CHECK(!registry.commitBind(pending));
  CHECK(!registry.beginBind(pending, {}));
  CHECK(registry.configure({{"org.test.App", {{"talk", "F17"}}}}, {}, WLR_MODIFIER_LOGO));
  CHECK(registry.list(owner)->front().trigger == std::optional{trigger("F17")});
  CHECK_EQ(registry.takeChanges().changed, std::vector{owner});
}

UMBRIEL_TEST(invalidOverridesPreserveThePreviousConfigurationAndPendingBind) {
  for (const std::string_view chord : {"Mod", "Ctrl+MouseLeft", "Mod+WheelUp", "submap[test],Mod+h", "unknown_key"}) {
    GlobalShortcutsRegistry registry;
    CHECK(registry.configure({{"org.test.App", {{"a", "a"}}}}, {}, WLR_MODIFIER_LOGO));
    const auto id = create(registry);
    CHECK(registry.beginBind(id, {{"a", "Action", "F16"}}).has_value());
    CHECK(!registry.configure({{"org.test.App", {{"a", std::string(chord)}}}}, {}, WLR_MODIFIER_LOGO));
    CHECK(registry.session(id)->pending);
    approvePending(registry, id);
    CHECK(registry.list(id)->front().trigger == std::optional{trigger("a")});
    CHECK(registry.takeChanges().cancelled.empty());
  }
}

UMBRIEL_TEST(revocationClosesAllApplicationSessionsAndRetainsOtherApplications) {
  GlobalShortcutsRegistry registry;
  const auto owner = create(registry);
  CHECK(registry.beginBind(owner, {{"talk", "Talk", "F16"}}).has_value());
  approvePending(registry, owner);
  const auto pending = create(registry);
  CHECK(registry.beginBind(pending, {{"wait", "Wait", "F20"}}).has_value());
  const auto waiter = create(registry, "org.test.Other");
  CHECK(registry.beginBind(waiter, {{"talk", "Talk", "F16"}}).has_value());
  approvePending(registry, waiter);
  CHECK_EQ(registry.revokeApplication("org.test.App"), (std::vector{owner, pending}));
  CHECK(!registry.session(owner));
  CHECK(!registry.session(pending));
  CHECK_EQ(registry.list(waiter)->size(), size_t{1});
  CHECK_EQ(registry.takeChanges().changed, std::vector{waiter});
  CHECK_EQ(registry.revokeAll(), std::vector{waiter});
  CHECK(!registry.session(waiter));
}

UMBRIEL_TEST(inspectionPreservesRestoreEligibilityAndQueuedChanges) {
  GlobalShortcutsRegistry registry;
  const auto id = create(registry);
  auto saved = grant("org.test.App", {shortcut("talk", "CTRL+m"), shortcut("unused", "F16")});
  saved.previous = {shortcut("talk", "CTRL+m")};
  const auto before = registry.inspect(std::array{saved});
  CHECK_EQ(before.size(), size_t{1});
  CHECK(before.front().live);
  CHECK_EQ(before.front().shortcuts.size(), size_t{2});
  CHECK(registry.canRestore(id));
  CHECK(registry.restore(id, saved));
  CHECK(registry.beginBind(id, {{"talk", "Current description", "F20"}}).has_value());
  CHECK(registry.commitBind(id));
  CHECK(registry.configure({{"org.test.App", {{"talk", "Ctrl+K"}}}}, {}, WLR_MODIFIER_LOGO));
  const auto inspected = registry.inspect(std::array{saved});
  CHECK_EQ(inspected.front().shortcuts.size(), size_t{2});
  const auto& live = inspected.front().shortcuts.front();
  CHECK_EQ(live.session, std::optional{id});
  CHECK(!live.pending);
  CHECK_EQ(live.shortcut.description, "Current description");
  CHECK_EQ(live.shortcut.source, GlobalShortcutSource::Config);
  CHECK_EQ(live.shortcut.trigger, std::optional{trigger("CTRL+k")});
  const auto& offline = inspected.front().shortcuts.back();
  CHECK(!offline.session);
  CHECK(!offline.shortcut.trigger);
  CHECK_EQ(offline.shortcut.candidate, std::optional{trigger("F16")});
  CHECK_EQ(registry.takeChanges().changed, std::vector{id});
  CHECK(registry.owns({id, "talk", trigger("CTRL+k")}));
}

UMBRIEL_TEST(inspectionReportsRememberedCandidatesWithoutAssigningThemLiveOwnership) {
  GlobalShortcutsRegistry registry;
  CHECK(registry.configure(
      {{"org.test.Offline", {{"disabled", ""}, {"reserved", "Ctrl+N"}, {"typing", "a"}}}},
      std::array{trigger("CTRL+n")}, WLR_MODIFIER_LOGO
  ));
  const auto owner = create(registry);
  CHECK(registry.beginBind(owner, {{"talk", "Owner", "CTRL+m"}}).has_value());
  approvePending(registry, owner);
  auto offline = grant(
      "org.test.Offline",
      {shortcut("talk", "CTRL+m"), shortcut("disabled", "F16"), shortcut("reserved", "F17"), shortcut("typing"),
       shortcut("unassigned")}
  );
  offline.id = std::string(32, 'b');
  offline.previous = {shortcut("talk", "F18")};
  offline.previous.front().description = "Most recent description";
  const auto inspected = registry.inspect(std::array{offline});
  CHECK_EQ(inspected.size(), size_t{2});
  const auto& application = inspected.back();
  CHECK_EQ(application.application, "org.test.Offline");
  CHECK(!application.live);
  CHECK_EQ(application.shortcuts.size(), size_t{5});
  for (const auto& entry : application.shortcuts) {
    CHECK(!entry.session && !entry.pending && !entry.shortcut.trigger);
  }
  CHECK_EQ(application.shortcuts[0].shortcut.conflict, GlobalShortcutConflict::InUse);
  CHECK_EQ(application.shortcuts[0].shortcut.description, "Most recent description");
  CHECK_EQ(application.shortcuts[0].shortcut.candidate, std::optional{trigger("CTRL+m")});
  CHECK_EQ(application.shortcuts[1].shortcut.conflict, GlobalShortcutConflict::Disabled);
  CHECK_EQ(application.shortcuts[2].shortcut.conflict, GlobalShortcutConflict::Reserved);
  CHECK_EQ(application.shortcuts[2].shortcut.candidate, std::optional{trigger("CTRL+n")});
  CHECK_EQ(application.shortcuts[3].shortcut.source, GlobalShortcutSource::Config);
  CHECK_EQ(application.shortcuts[3].shortcut.candidate, std::optional{trigger("a")});
  CHECK_EQ(application.shortcuts[4].shortcut.conflict, GlobalShortcutConflict::Unassigned);
  CHECK_EQ(application.shortcuts[4].shortcut.source, GlobalShortcutSource::None);
  CHECK(registry.owns({owner, "talk", trigger("CTRL+m")}));
  CHECK(registry.takeChanges().changed.empty());
}

UMBRIEL_TEST(inspectionKeepsSameApplicationSessionsAndGrantScopesSeparate) {
  GlobalShortcutsRegistry registry;
  auto first = grant("org.test.App", {shortcut("talk", "F16"), shortcut("hidden", "F18")});
  auto second = grant("org.test.App", {shortcut("talk", "F17"), shortcut("hidden", "F19")});
  second.id = std::string(32, 'b');
  const auto owner = create(registry);
  CHECK(registry.restore(owner, first));
  CHECK(registry.beginBind(owner, {{"talk", "First", "F21"}}).has_value());
  CHECK(registry.commitBind(owner));
  const auto pending = create(registry);
  CHECK(registry.beginBind(pending, {{"talk", "Unverified claim", "F20"}}).has_value());
  const auto other = create(registry);
  CHECK(registry.restore(other, second));
  CHECK(registry.beginBind(other, {{"talk", "Second", "F21"}}).has_value());
  CHECK(registry.commitBind(other));
  const auto inspected = registry.inspect(std::array{first, second});
  CHECK_EQ(inspected.size(), size_t{1});
  const auto& shortcuts = inspected.front().shortcuts;
  CHECK_EQ(shortcuts.size(), size_t{5});
  CHECK_EQ(shortcuts[0].session, std::optional{owner});
  CHECK_EQ(shortcuts[0].shortcut.trigger, std::optional{trigger("F16")});
  CHECK_EQ(shortcuts[1].session, std::optional{pending});
  CHECK(shortcuts[1].pending);
  CHECK_EQ(shortcuts[1].shortcut.source, GlobalShortcutSource::Preferred);
  CHECK_EQ(shortcuts[1].shortcut.candidate, std::optional{trigger("F20")});
  CHECK(!shortcuts[1].shortcut.trigger);
  CHECK_EQ(shortcuts[2].session, std::optional{other});
  CHECK_EQ(shortcuts[2].shortcut.trigger, std::optional{trigger("F17")});
  CHECK(!shortcuts[3].session && !shortcuts[4].session);
  CHECK_EQ(shortcuts[3].shortcut.candidate, std::optional{trigger("F18")});
  CHECK_EQ(shortcuts[4].shortcut.candidate, std::optional{trigger("F19")});
  CHECK(registry.session(pending)->pending);
}

int main() { return RUN_TESTS(); }
