#pragma once

#include "config/effects.h"

#include <map>
#include <random>
#include <span>

namespace umbriel {

  enum class EffectSelectorSource : std::uint8_t { Default, Rule, Runtime };
  [[nodiscard]] std::string_view effectSelectorSourceName(EffectSelectorSource source);

  // Owned by one mapped window slot, present output, or the session cursor.
  // Clear the whole value on unmap; disabling an output does not clear it.
  struct EffectSlot {
    std::string configuredSelector;
    EffectSelectorSource configuredSource = EffectSelectorSource::Default;
    std::optional<std::string> runtimeSelector;
    std::string name;
    std::string pool;
    bool suppressed = false;
    std::map<std::string, std::string, std::less<>> history;

    [[nodiscard]] EffectSelectorSource source() const;
    [[nodiscard]] std::string_view selector() const;
    [[nodiscard]] std::string_view effectiveName() const;
    bool operator==(const EffectSlot&) const = default;
  };

  using EffectHoldCounts = std::vector<std::size_t>;
  // The caller supplies only live owners of the pool's kind. Membership is
  // checked against the new pool, so obsolete reload assignments never count.
  [[nodiscard]] EffectHoldCounts effectPoolHoldCounts(
      const EffectPool& pool, std::span<const EffectSlot* const> slots, const EffectSlot* excluded = nullptr
  );

  struct EffectPick {
    std::string name;
    std::string lastHandout;
    bool operator==(const EffectPick&) const = default;
  };
  // randomDraw is a uniform index in [0, members.size()). Other policies ignore
  // it. Missing count entries mean zero; an empty pool does not advance state.
  [[nodiscard]] EffectPick pickEffectMember(
      std::span<const std::string> members, std::span<const std::size_t> counts, EffectSelectionPolicy policy,
      std::string_view lastHandout, std::size_t randomDraw
  );

  enum class EffectSlotAction : std::uint8_t { Set, Cycle, Toggle, Reset };
  struct EffectSlotActionResult {
    std::optional<std::string> error;
    bool changed = false;
  };

  // Shared pool policy state; selections and history remain on owners.
  class EffectSelection {
  public:
    EffectSelection();
    explicit EffectSelection(std::uint64_t seed);

    // The caller records the configured winner/origin before resolving.
    // active=false permits pre-map rule evaluation without assigning a member.
    bool resolve(
        EffectSlot& slot, const Effects& effects, EffectKind kind, std::span<const std::size_t> counts = {},
        bool active = true
    );
    // Arguments and reference kinds are validated before any state/RNG change.
    [[nodiscard]] EffectSlotActionResult apply(
        EffectSlot& slot, const Effects& effects, EffectKind kind, EffectSlotAction action,
        std::string_view argument = {}, std::span<const std::size_t> counts = {}
    );
    void reconcile(const Effects& effects);
    // Prunes invalid remembered members, drops invalid runtime selectors, and
    // returns the dropped selector for the caller's owner-specific diagnostic.
    [[nodiscard]] std::optional<std::string> prune(EffectSlot& slot, const Effects& effects, EffectKind kind) const;

  private:
    struct PolicyState {
      EffectKind kind;
      EffectSelectionPolicy policy;
      std::string lastHandout;
    };
    [[nodiscard]] std::string pick(const EffectPool& pool, std::span<const std::size_t> counts);
    std::map<std::string, PolicyState, std::less<>> m_policies;
    std::mt19937_64 m_random;
  };

} // namespace umbriel
