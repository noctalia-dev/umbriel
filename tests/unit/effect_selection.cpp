#include "scene/effect_selection.h"

#include "check.h"

#include <array>

using namespace umbriel;

namespace {
  Effects configuration(EffectSelectionPolicy policy = EffectSelectionPolicy::UnusedFirst) {
    Effects effects;
    for (const auto* name : {"x", "y", "z"}) {
      effects.presets.push_back({.name = name, .kind = EffectKind::Window});
    }
    effects.presets.push_back({.name = "border", .kind = EffectKind::Border});
    effects.pools.push_back({.name = "A", .kind = EffectKind::Window, .members = {"x", "y"}, .selection = policy});
    effects.pools.push_back({.name = "B", .kind = EffectKind::Window, .members = {"x", "y"}, .selection = policy});
    effects.pools.push_back({.name = "empty", .kind = EffectKind::Window, .members = {}, .selection = policy});
    return effects;
  }

  EffectSlot configured(std::string selector) {
    EffectSlot slot;
    slot.configuredSelector = std::move(selector);
    return slot;
  }

  EffectSlotActionResult action(
      EffectSelection& selection, EffectSlot& slot, const Effects& effects, EffectSlotAction operation,
      std::string_view argument = {}, std::span<const std::size_t> counts = {}
  ) {
    return selection.apply(slot, effects, EffectKind::Window, operation, argument, counts);
  }
} // namespace

UMBRIEL_TEST(unusedFirstUsesSmallestCountThenDeclarationOrder) {
  const std::vector<std::string> members{"z", "x", "y"};
  CHECK_EQ(pickEffectMember(members, {}, EffectSelectionPolicy::UnusedFirst, "", 999).name, "z");
  const std::array<std::size_t, 3> counts{4, 1, 1};
  CHECK_EQ(pickEffectMember(members, counts, EffectSelectionPolicy::UnusedFirst, "", 0).name, "x");
  const std::array<std::size_t, 3> least{1, 2, 0};
  CHECK_EQ(pickEffectMember(members, least, EffectSelectionPolicy::UnusedFirst, "", 0).name, "y");
}

UMBRIEL_TEST(pureRoundRobinAndRandomHaveExplicitInputs) {
  const std::vector<std::string> members{"z", "x", "y"};
  auto pick = pickEffectMember(members, {}, EffectSelectionPolicy::RoundRobin, "", 0);
  CHECK_EQ(pick.name, "z");
  pick = pickEffectMember(members, {}, EffectSelectionPolicy::RoundRobin, pick.lastHandout, 0);
  CHECK_EQ(pick.name, "x");
  CHECK_EQ(pickEffectMember(members, {}, EffectSelectionPolicy::RoundRobin, "y", 0).name, "z");
  CHECK_EQ(pickEffectMember(members, {}, EffectSelectionPolicy::RoundRobin, "removed", 0).name, "z");
  for (std::size_t draw = 0; draw < members.size(); ++draw) {
    CHECK_EQ(pickEffectMember(members, {}, EffectSelectionPolicy::Random, "", draw).name, members[draw]);
  }
  CHECK_EQ(pickEffectMember({}, {}, EffectSelectionPolicy::RoundRobin, "x", 0).lastHandout, "x");
}

UMBRIEL_TEST(holdCountsUseOnlyLiveUnsuppressedAssignmentsFromThisPool) {
  const auto effects = configuration();
  EffectSelection selection(1);
  auto first = configured("A");
  auto hidden = configured("A");
  auto otherPool = configured("B");
  auto plain = configured("x");
  auto suppressed = configured("A");
  auto stale = configured("A");
  for (auto* slot : {&first, &hidden, &otherPool, &plain, &suppressed, &stale}) {
    selection.resolve(*slot, effects, EffectKind::Window);
  }
  suppressed.suppressed = true;
  plain.history["A"] = "x";
  stale.name = "removed";
  const std::array<const EffectSlot*, 6> slots{&first, &hidden, &otherPool, &plain, &suppressed, &stale};
  // Shader state and visibility never enter the accounting. All fixtures are
  // inert, and the hidden mapped owner still holds its member.
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], slots), (EffectHoldCounts{2, 0}));
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], slots, &first), (EffectHoldCounts{1, 0}));
  CHECK_EQ(effectPoolHoldCounts(effects.pools[1], slots), (EffectHoldCounts{1, 0}));
}

UMBRIEL_TEST(unmappedResolutionAndNewMappedLifetimeDoNotReuseAssignments) {
  const auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("A");
  CHECK(!selection.resolve(slot, effects, EffectKind::Window, {}, false));
  CHECK(slot.name.empty());
  CHECK(slot.history.empty());
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "x");
  CHECK(!selection.resolve(slot, effects, EffectKind::Window));
  // Both unmapping a view and destroying an output discard the owner value.
  slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
}

UMBRIEL_TEST(historyWinsBeforeCurrentMemberReuseWhenChangingPools) {
  const auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  slot.history["B"] = "y";
  slot.configuredSelector = "B";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  slot.configuredSelector = "A";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "x");
  slot.configuredSelector = "off";
  slot.configuredSource = EffectSelectorSource::Rule;
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(slot.name.empty());
  CHECK(slot.pool.empty());
  CHECK_EQ(effectSelectorSourceName(slot.source()), "rule");
  slot.configuredSelector = "B";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  // Restoration did not hand out a new policy member from B.
  auto another = configured("B");
  selection.resolve(another, effects, EffectKind::Window);
  CHECK_EQ(another.name, "x");
}

UMBRIEL_TEST(changingPoolsReusesCurrentMemberBeforeFreshPolicyPick) {
  auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("y");
  selection.resolve(slot, effects, EffectKind::Window);
  slot.configuredSelector = "B";
  slot.history["B"] = "removed";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  effects.pools[0].members = {"z"};
  slot.configuredSelector = "A";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "z");
  auto other = configured("B");
  selection.resolve(other, effects, EffectKind::Window);
  CHECK_EQ(other.name, "x");
}

UMBRIEL_TEST(suppressionKeepsSelectorsAndHistoryWhileRulesResolveUnderneath) {
  const auto effects = configuration();
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(action(selection, slot, effects, EffectSlotAction::Set, "off").changed);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "off").changed);
  CHECK_EQ(slot.name, "x");
  CHECK_EQ(slot.pool, "A");
  CHECK(slot.effectiveName().empty());
  CHECK_EQ(slot.source(), EffectSelectorSource::Default);
  slot.configuredSelector = "y";
  slot.configuredSource = EffectSelectorSource::Rule;
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  CHECK(slot.suppressed);
  CHECK(action(selection, slot, effects, EffectSlotAction::Toggle).changed);
  CHECK_EQ(slot.effectiveName(), "y");
  CHECK(slot.history.contains("A"));
  CHECK(action(selection, slot, effects, EffectSlotAction::Toggle).changed);
  slot.configuredSelector.clear();
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(slot.name.empty());
  CHECK(action(selection, slot, effects, EffectSlotAction::Toggle).changed);
  CHECK(!slot.suppressed);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Toggle).changed);
}

UMBRIEL_TEST(explicitSetMakesFreshPolicyPickAndRuntimeWinsRules) {
  const auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(action(selection, slot, effects, EffectSlotAction::Set, "A").changed);
  CHECK_EQ(slot.name, "y");
  CHECK_EQ(slot.source(), EffectSelectorSource::Runtime);
  slot.configuredSelector = "z";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  CHECK(action(selection, slot, effects, EffectSlotAction::Set, "A").changed);
  CHECK_EQ(slot.name, "x");
  CHECK(action(selection, slot, effects, EffectSlotAction::Set, "empty").changed);
  CHECK_EQ(slot.pool, "empty");
  CHECK(slot.name.empty());
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "empty").error);
}

UMBRIEL_TEST(cycleUsesUnderlyingMemberAndDoesNotAdvancePolicyUnlessPicking) {
  auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  effects.pools.push_back({.name = "single", .kind = EffectKind::Window, .members = {"y"}});
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(action(selection, slot, effects, EffectSlotAction::Set, "off").changed);
  CHECK(action(selection, slot, effects, EffectSlotAction::Cycle).changed);
  CHECK_EQ(slot.name, "y");
  CHECK_EQ(slot.runtimeSelector, std::optional<std::string>("A"));
  CHECK(!slot.suppressed);
  auto other = configured("A");
  selection.resolve(other, effects, EffectKind::Window);
  CHECK_EQ(other.name, "y");
  CHECK(!action(selection, slot, effects, EffectSlotAction::Cycle, "single").error);
  CHECK_EQ(slot.name, "y");
  CHECK(!action(selection, slot, effects, EffectSlotAction::Cycle).error);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "z").error);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Cycle, "B").error);
  CHECK_EQ(slot.name, "x");
}

UMBRIEL_TEST(resetClearsRuntimeHistoryAndCurrentMemberBeforeConfiguredPick) {
  const auto effects = configuration();
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Cycle).error);
  CHECK_EQ(slot.name, "y");
  slot.history["B"] = "y";
  slot.suppressed = true;
  CHECK(action(selection, slot, effects, EffectSlotAction::Reset).changed);
  CHECK_EQ(slot.name, "x");
  CHECK(!slot.runtimeSelector);
  CHECK(!slot.suppressed);
  CHECK_EQ(slot.history.size(), 1U);
  CHECK_EQ(slot.history.at("A"), "x");
}

UMBRIEL_TEST(rejectedActionsLeaveSlotAndRandomSequenceUntouched) {
  const auto effects = configuration(EffectSelectionPolicy::Random);
  EffectSelection selection(77);
  EffectSelection control(77);
  auto slot = configured("x");
  selection.resolve(slot, effects, EffectKind::Window);
  slot.suppressed = true;
  slot.history["B"] = "y";
  const auto before = slot;
  for (const auto* name : {"missing", "border", ""}) {
    CHECK(action(selection, slot, effects, EffectSlotAction::Set, name).error.has_value());
    CHECK_EQ(slot, before);
  }
  for (const auto* name : {"missing", "x", "off", "empty", ""}) {
    CHECK(action(selection, slot, effects, EffectSlotAction::Cycle, name).error.has_value());
    CHECK_EQ(slot, before);
  }
  CHECK(action(selection, slot, effects, EffectSlotAction::Toggle, "unexpected").error.has_value());
  CHECK_EQ(slot, before);
  auto expected = before;
  for (int iteration = 0; iteration < 20; ++iteration) {
    CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
    CHECK(!action(control, expected, effects, EffectSlotAction::Set, "A").error);
    CHECK_EQ(slot.name, expected.name);
  }
}

UMBRIEL_TEST(reloadRetainsValidAssignmentsAndPrunesOnlyInvalidHistory) {
  auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  slot.history["B"] = "y";
  selection.reconcile(effects);
  CHECK(!selection.prune(slot, effects, EffectKind::Window));
  CHECK(!selection.resolve(slot, effects, EffectKind::Window));
  slot.configuredSelector = "B";
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "y");
  effects.pools[1].members = {"x"};
  selection.reconcile(effects);
  CHECK(!selection.prune(slot, effects, EffectKind::Window));
  CHECK(!slot.history.contains("B"));
  CHECK_EQ(slot.history.at("A"), "x");
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "x");
}

UMBRIEL_TEST(roundRobinReloadTracksLastHandoutByNameAndResetsInvalidState) {
  auto effects = configuration(EffectSelectionPolicy::RoundRobin);
  EffectSelection selection(1);
  auto slot = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  effects.pools[0].members = {"z", "x", "y"};
  selection.reconcile(effects);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
  CHECK_EQ(slot.name, "y");
  effects.pools[0].members = {"z", "x"};
  selection.reconcile(effects);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
  CHECK_EQ(slot.name, "z");
  effects.pools[0].selection = EffectSelectionPolicy::UnusedFirst;
  selection.reconcile(effects);
  effects.pools[0].selection = EffectSelectionPolicy::RoundRobin;
  selection.reconcile(effects);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
  CHECK_EQ(slot.name, "z");
  effects.pools[0].kind = EffectKind::Border;
  selection.reconcile(effects);
  effects.pools[0].kind = EffectKind::Window;
  selection.reconcile(effects);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
  CHECK_EQ(slot.name, "z");
}

UMBRIEL_TEST(reloadDropsDeletedOrWrongKindOverridesButKeepsSuppression) {
  auto effects = configuration();
  EffectSelection selection(1);
  auto slot = configured("z");
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
  slot.suppressed = true;
  effects.pools[0].kind = EffectKind::Border;
  CHECK_EQ(selection.prune(slot, effects, EffectKind::Window), std::optional<std::string>("A"));
  CHECK(slot.suppressed);
  CHECK(slot.history.empty());
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "z");
  CHECK_EQ(slot.source(), EffectSelectorSource::Default);
  CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "x").error);
  effects.presets.erase(effects.presets.begin());
  CHECK_EQ(selection.prune(slot, effects, EffectKind::Window), std::optional<std::string>("x"));
  selection.resolve(slot, effects, EffectKind::Window);
  CHECK_EQ(slot.name, "z");
}

UMBRIEL_TEST(repeatedResolutionInspectionAndRecoveryDoNotConsumeRandomDraws) {
  const auto effects = configuration(EffectSelectionPolicy::Random);
  EffectSelection selection(2026);
  EffectSelection control(2026);
  auto slot = configured("A");
  auto expected = configured("A");
  selection.resolve(slot, effects, EffectKind::Window);
  control.resolve(expected, effects, EffectKind::Window);
  const auto before = slot;
  for (int iteration = 0; iteration < 20; ++iteration) {
    CHECK(!selection.resolve(slot, effects, EffectKind::Window));
    CHECK_EQ(slot.source(), EffectSelectorSource::Default);
    CHECK_EQ(slot.selector(), "A");
    CHECK_EQ(slot.effectiveName(), before.name);
    const std::array<const EffectSlot*, 1> owners{&slot};
    CHECK_EQ(effectPoolHoldCounts(effects.pools[0], owners).size(), 2U);
  }
  CHECK_EQ(slot, before);
  for (int iteration = 0; iteration < 20; ++iteration) {
    CHECK(!action(selection, slot, effects, EffectSlotAction::Set, "A").error);
    CHECK(!action(control, expected, effects, EffectSlotAction::Set, "A").error);
    CHECK_EQ(slot.name, expected.name);
  }
}

UMBRIEL_TEST(presentScreenOwnersRetainDisabledStateAndReconnectStartsFresh) {
  Effects effects;
  effects.presets = {
      {.name = "screen-z", .kind = EffectKind::Screen}, {.name = "screen-a", .kind = EffectKind::Screen}
  };
  effects.pools.push_back({.name = "screens", .kind = EffectKind::Screen, .members = {"screen-z", "screen-a"}});
  EffectSelection selection(33);
  auto first = configured("screens");
  auto second = configured("screens");
  selection.resolve(first, effects, EffectKind::Screen);
  std::array<const EffectSlot*, 2> present{&first, &second};
  selection.resolve(second, effects, EffectKind::Screen, effectPoolHoldCounts(effects.pools[0], present, &second));
  CHECK_EQ(first.name, "screen-z");
  CHECK_EQ(second.name, "screen-a");
  // Output enablement is deliberately absent from holding/selection inputs.
  // A present disabled output retains the exact owner-local value and holding.
  CHECK(!selection.resolve(first, effects, EffectKind::Screen));
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], present), (EffectHoldCounts{1, 1}));
  CHECK(selection.apply(first, effects, EffectKind::Screen, EffectSlotAction::Set, "off").changed);
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], present), (EffectHoldCounts{0, 1}));
  CHECK_EQ(first.history.at("screens"), "screen-z");
  CHECK(selection.apply(first, effects, EffectKind::Screen, EffectSlotAction::Set, "screen-a").changed);
  CHECK(first.runtimeSelector.has_value());
  first = {}; // Output destruction drops the slot, even if the connector reconnects unchanged.
  first.configuredSelector = "screens";
  selection.resolve(first, effects, EffectKind::Screen, effectPoolHoldCounts(effects.pools[0], present, &first));
  CHECK_EQ(first.name, "screen-z");
  CHECK(!first.runtimeSelector);
  CHECK(!first.suppressed);
  CHECK_EQ(first.source(), EffectSelectorSource::Default);
  CHECK_EQ(first.history.size(), size_t{1});
}

UMBRIEL_TEST(reloadBatchCountsIgnoreRemovedAssignmentsBeforeAnyOwnerIsResolved) {
  auto effects = configuration();
  EffectSelection selection(33);
  auto first = configured("A");
  auto second = configured("A");
  auto third = configured("A");
  std::array<const EffectSlot*, 3> present{&first, &second, &third};
  for (auto* slot : {&first, &second, &third}) {
    selection.resolve(*slot, effects, EffectKind::Window, effectPoolHoldCounts(effects.pools[0], present, slot));
  }
  CHECK_EQ(first.name, "x");
  CHECK_EQ(second.name, "y");
  CHECK_EQ(third.name, "x");
  effects.pools[0].members = {"z", "y"};
  selection.reconcile(effects);
  for (auto* slot : {&first, &second, &third}) {
    CHECK(!selection.prune(*slot, effects, EffectKind::Window));
  }
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], present), (EffectHoldCounts{0, 1}));
  for (auto* slot : {&first, &second, &third}) {
    selection.resolve(*slot, effects, EffectKind::Window, effectPoolHoldCounts(effects.pools[0], present, slot));
  }
  CHECK_EQ(first.name, "z");
  CHECK_EQ(second.name, "y");
  CHECK_EQ(third.name, "z");
  CHECK_EQ(effectPoolHoldCounts(effects.pools[0], present), (EffectHoldCounts{2, 1}));
}

int main() { return RUN_TESTS(); }
