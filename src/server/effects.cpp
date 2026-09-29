#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "output/identity.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cassert>

namespace umbriel {
  namespace {
    constexpr Logger kLog("effects");
  } // namespace

  EffectHoldCounts Server::effectHoldCounts(const EffectPool& pool, const EffectSlot* excluded) const {
    assert(!handlingOutputFrame());
    std::vector<const EffectSlot*> slots;
    if (pool.kind == EffectKind::Border || pool.kind == EffectKind::Window) {
      for (const auto& view : views()) {
        if (view->mapped()) {
          slots.push_back(&view->effectSlot(pool.kind));
        }
      }
    } else if (pool.kind == EffectKind::Screen) {
      for (const auto& output : outputs()) {
        slots.push_back(&output->screenEffectSlot());
      }
    } else if (pool.kind == EffectKind::Cursor) {
      slots.push_back(&m_cursorEffectSlot);
    }
    return effectPoolHoldCounts(pool, slots, excluded);
  }

  std::vector<std::string> Server::runtimeEffectSelectors() const {
    std::vector<std::string> selectors;
    const auto add = [&](const EffectSlot& slot) {
      if (slot.runtimeSelector) {
        selectors.push_back(*slot.runtimeSelector);
      }
    };
    for (const auto& view : views()) {
      if (view->mapped()) {
        add(view->effectSlot(EffectKind::Border));
        add(view->effectSlot(EffectKind::Window));
      }
    }
    for (const auto& output : outputs()) {
      add(output->screenEffectSlot());
    }
    add(m_cursorEffectSlot);
    return selectors;
  }

  std::vector<View*> Server::sortedEffectViews() const {
    std::vector<View*> sorted;
    for (const auto& view : views()) {
      if (view->mapped()) {
        sorted.push_back(view.get());
      }
    }
    std::ranges::stable_sort(sorted, [](const View* lhs, const View* rhs) {
      const char* left = lhs->extForeignIdentifier();
      const char* right = rhs->extForeignIdentifier();
      if (left == nullptr || right == nullptr) {
        return left != nullptr && right == nullptr;
      }
      return std::string_view(left) < std::string_view(right);
    });
    return sorted;
  }

  void Server::resolveEffectSlot(EffectSlot& slot, EffectKind kind, bool active) {
    const Effects& settings = config().effects;
    EffectHoldCounts counts;
    // Restoring a cached member never needs a walk of other owners.
    if (const EffectPool* pool = findEffectPool(settings, slot.selector());
        active && pool != nullptr && pool->kind == kind && !pool->members.empty()) {
      const auto remembered = slot.history.find(pool->name);
      const bool rememberedValid =
          remembered != slot.history.end() && std::ranges::contains(pool->members, remembered->second);
      if (!std::ranges::contains(pool->members, slot.name) && !rememberedValid) {
        counts = effectHoldCounts(*pool, &slot);
      }
    }
    m_effectSelection.resolve(slot, settings, kind, counts, active);
  }

  void Server::resolveOutputEffect(Output& output) {
    EffectSlot& slot = output.screenEffectSlot();
    const OutputRule* rule = findOutputRule(config(), output.identity());
    const bool overridden = rule != nullptr && rule->screenEffect.has_value();
    slot.configuredSelector = overridden ? *rule->screenEffect : config().effects.screen;
    slot.configuredSource = overridden ? EffectSelectorSource::Rule : EffectSelectorSource::Default;
    resolveEffectSlot(slot, EffectKind::Screen);
  }

  void Server::reconcileEffectSelections() {
    const Effects& settings = config().effects;
    m_effectSelection.reconcile(settings);
    const auto prune = [&](EffectSlot& slot, EffectKind kind, std::string_view owner) {
      if (const auto dropped = m_effectSelection.prune(slot, settings, kind)) {
        kLog.warn("{}: dropping invalid {} effect override '{}'", owner, effectKindName(kind), *dropped);
      }
    };
    for (View* view : sortedEffectViews()) {
      const char* id = view->extForeignIdentifier();
      const std::string owner = "window " + std::string(id != nullptr ? id : "<unidentified>");
      prune(view->effectSlot(EffectKind::Border), EffectKind::Border, owner);
      prune(view->effectSlot(EffectKind::Window), EffectKind::Window, owner);
    }
    std::vector<Output*> sorted;
    for (const auto& output : outputs()) {
      prune(output->screenEffectSlot(), EffectKind::Screen, "output " + std::string(output->identity().connector));
      sorted.push_back(output.get());
    }
    std::ranges::sort(sorted, {}, [](const Output* output) { return output->identity().connector; });
    prune(m_cursorEffectSlot, EffectKind::Cursor, "cursor");
    for (Output* output : sorted) {
      resolveOutputEffect(*output);
    }
    m_cursorEffectSlot.configuredSelector = settings.cursor;
    resolveEffectSlot(m_cursorEffectSlot, EffectKind::Cursor);
    // Pruning can change source before the later rule refresh sees it; overlay
    // metadata can change without changing the selected border name.
    scheduleIpcWindowsEvent();
  }

  EffectSlotActionResult
  Server::applyEffectAction(EffectSlot& slot, EffectKind kind, EffectSlotAction action, std::string_view argument) {
    std::string_view selector = argument;
    if (action == EffectSlotAction::Reset) {
      selector = slot.configuredSelector;
    } else if (argument.empty()) {
      selector = slot.selector();
    }
    const EffectPool* pool = findEffectPool(config().effects, selector);
    const EffectHoldCounts counts = pool != nullptr ? effectHoldCounts(*pool, &slot) : EffectHoldCounts{};
    return m_effectSelection.apply(slot, config().effects, kind, action, argument, counts);
  }
} // namespace umbriel
