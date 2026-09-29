#include "scene/effect_selection.h"

#include <algorithm>
#include <iterator>

namespace umbriel {

  namespace {
    void assign(EffectSlot& slot, const EffectPool& pool, std::string name) {
      slot.pool = pool.name;
      slot.name = std::move(name);
      if (!slot.name.empty()) {
        slot.history[pool.name] = slot.name;
      }
    }
  } // namespace

  std::string_view effectSelectorSourceName(EffectSelectorSource source) {
    switch (source) {
    case EffectSelectorSource::Default:
      return "default";
    case EffectSelectorSource::Rule:
      return "rule";
    case EffectSelectorSource::Runtime:
      return "runtime";
    }
    return "default";
  }

  EffectSelectorSource EffectSlot::source() const {
    return runtimeSelector ? EffectSelectorSource::Runtime : configuredSource;
  }

  std::string_view EffectSlot::selector() const {
    return runtimeSelector ? std::string_view(*runtimeSelector) : std::string_view(configuredSelector);
  }

  std::string_view EffectSlot::effectiveName() const {
    return suppressed ? std::string_view{} : std::string_view(name);
  }

  EffectHoldCounts
  effectPoolHoldCounts(const EffectPool& pool, std::span<const EffectSlot* const> slots, const EffectSlot* excluded) {
    EffectHoldCounts counts(pool.members.size());
    for (const auto* slot : slots) {
      if (slot == nullptr || slot == excluded || slot->suppressed || slot->pool != pool.name) {
        continue;
      }
      const auto member = std::ranges::find(pool.members, slot->name);
      if (member != pool.members.end()) {
        ++counts[static_cast<std::size_t>(std::distance(pool.members.begin(), member))];
      }
    }
    return counts;
  }

  EffectPick pickEffectMember(
      std::span<const std::string> members, std::span<const std::size_t> counts, EffectSelectionPolicy policy,
      std::string_view lastHandout, std::size_t randomDraw
  ) {
    if (members.empty()) {
      return {.name = {}, .lastHandout = std::string(lastHandout)};
    }
    std::size_t index = 0;
    switch (policy) {
    case EffectSelectionPolicy::UnusedFirst: {
      const auto count = [counts](std::size_t at) { return at < counts.size() ? counts[at] : 0; };
      for (std::size_t candidate = 1; candidate < members.size(); ++candidate) {
        if (count(candidate) < count(index)) {
          index = candidate;
        }
      }
      break;
    }
    case EffectSelectionPolicy::RoundRobin: {
      const auto last = std::ranges::find(members, lastHandout);
      if (last != members.end()) {
        index = (static_cast<std::size_t>(std::distance(members.begin(), last)) + 1) % members.size();
      }
      break;
    }
    case EffectSelectionPolicy::Random:
      index = randomDraw % members.size();
      break;
    }
    return {
        .name = members[index],
        .lastHandout = policy == EffectSelectionPolicy::RoundRobin ? members[index] : std::string(lastHandout)
    };
  }

  EffectSelection::EffectSelection() : EffectSelection(std::random_device{}()) {}

  EffectSelection::EffectSelection(std::uint64_t seed) : m_random(seed) {}

  std::string EffectSelection::pick(const EffectPool& pool, std::span<const std::size_t> counts) {
    if (pool.members.empty()) {
      return {};
    }
    auto [entry, inserted] =
        m_policies.try_emplace(pool.name, PolicyState{.kind = pool.kind, .policy = pool.selection, .lastHandout = {}});
    auto& state = entry->second;
    if (!inserted && (state.kind != pool.kind || state.policy != pool.selection)) {
      state = {.kind = pool.kind, .policy = pool.selection, .lastHandout = {}};
    }
    const auto draw = pool.selection == EffectSelectionPolicy::Random
        ? std::uniform_int_distribution<std::size_t>(0, pool.members.size() - 1)(m_random)
        : 0;
    auto picked = pickEffectMember(pool.members, counts, pool.selection, state.lastHandout, draw);
    state.lastHandout = std::move(picked.lastHandout);
    return std::move(picked.name);
  }

  bool EffectSelection::resolve(
      EffectSlot& slot, const Effects& effects, EffectKind kind, std::span<const std::size_t> counts, bool active
  ) {
    if (!active) {
      return false;
    }
    const auto before = slot;
    const auto selector = slot.selector();
    if (const auto* pool = findEffectPool(effects, selector); pool != nullptr && pool->kind == kind) {
      if (slot.pool == pool->name && std::ranges::contains(pool->members, slot.name)) {
        slot.history[pool->name] = slot.name;
      } else if (
          const auto remembered = slot.history.find(pool->name);
          remembered != slot.history.end() && std::ranges::contains(pool->members, remembered->second)
      ) {
        assign(slot, *pool, remembered->second);
      } else if (std::ranges::contains(pool->members, slot.name)) {
        assign(slot, *pool, slot.name);
      } else {
        assign(slot, *pool, pick(*pool, counts));
      }
    } else {
      slot.pool.clear();
      const auto* preset = findEffectPreset(effects, selector);
      slot.name = preset != nullptr && preset->kind == kind ? preset->name : std::string{};
    }
    return slot != before;
  }

  EffectSlotActionResult EffectSelection::apply(
      EffectSlot& slot, const Effects& effects, EffectKind kind, EffectSlotAction action, std::string_view argument,
      std::span<const std::size_t> counts
  ) {
    const EffectPool* pool = nullptr;
    std::string selected(argument);
    if (action == EffectSlotAction::Set) {
      if (selected.empty()) {
        return {.error = "effect name is required"};
      }
      if (auto error = effectReferenceError(effects, selected, kind, true, EffectReferenceConstraint::PresetOrPool)) {
        return {.error = std::move(error)};
      }
      pool = findEffectPool(effects, selected);
    } else if (action == EffectSlotAction::Cycle) {
      if (selected.empty()) {
        selected = slot.pool;
        if (selected.empty()) {
          return {.error = "no pool to cycle"};
        }
      }
      if (auto error = effectReferenceError(effects, selected, kind, false, EffectReferenceConstraint::PoolRequired)) {
        return {.error = std::move(error)};
      }
      pool = findEffectPool(effects, selected);
      if (pool == nullptr || pool->members.empty()) {
        return {.error = "pool is empty"};
      }
    } else if (!selected.empty()) {
      return {.error = "unexpected effect argument"};
    }

    const auto before = slot;
    switch (action) {
    case EffectSlotAction::Set:
      if (selected == kEffectOff) {
        slot.suppressed = true;
      } else {
        slot.runtimeSelector = selected;
        slot.suppressed = false;
        if (pool != nullptr) {
          assign(slot, *pool, pick(*pool, counts));
        } else {
          slot.name = selected;
          slot.pool.clear();
        }
      }
      break;
    case EffectSlotAction::Cycle: {
      const auto current = std::ranges::find(pool->members, slot.name);
      auto name = current == pool->members.end()
          ? pick(*pool, counts)
          : pool->members
                [(static_cast<std::size_t>(std::distance(pool->members.begin(), current)) + 1) % pool->members.size()];
      slot.runtimeSelector = selected;
      slot.suppressed = false;
      assign(slot, *pool, std::move(name));
      break;
    }
    case EffectSlotAction::Toggle:
      if (slot.suppressed || !slot.name.empty()) {
        slot.suppressed = !slot.suppressed;
      }
      break;
    case EffectSlotAction::Reset:
      slot.runtimeSelector.reset();
      slot.suppressed = false;
      slot.history.clear();
      slot.name.clear();
      slot.pool.clear();
      resolve(slot, effects, kind, counts);
      break;
    }
    return {.error = std::nullopt, .changed = slot != before};
  }

  void EffectSelection::reconcile(const Effects& effects) {
    std::erase_if(m_policies, [&effects](auto& entry) {
      const auto* pool = findEffectPool(effects, entry.first);
      const auto& state = entry.second;
      return pool == nullptr
          || pool->kind != state.kind
          || pool->selection != state.policy
          || (!state.lastHandout.empty() && !std::ranges::contains(pool->members, state.lastHandout));
    });
  }

  std::optional<std::string> EffectSelection::prune(EffectSlot& slot, const Effects& effects, EffectKind kind) const {
    std::erase_if(slot.history, [&effects, kind](const auto& entry) {
      const auto* pool = findEffectPool(effects, entry.first);
      return pool == nullptr || pool->kind != kind || !std::ranges::contains(pool->members, entry.second);
    });
    if (slot.runtimeSelector
        && effectReferenceError(effects, *slot.runtimeSelector, kind, false, EffectReferenceConstraint::PresetOrPool)) {
      auto dropped = std::move(slot.runtimeSelector);
      slot.runtimeSelector.reset();
      return dropped;
    }
    return std::nullopt;
  }

} // namespace umbriel
