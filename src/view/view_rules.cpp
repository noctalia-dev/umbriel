#include "config/config.h"
#include "config/resolve.h"
#include "config/store.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/surface_blur.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "view/view_internal.h"
// clang-format off
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  namespace {
    constexpr Logger kLog("view");

    template <typename T>
    bool unseenInitialRuleValue(
        const std::optional<T>& current, const std::vector<ResolvedWindowRule>& history,
        const std::optional<T> ResolvedWindowRule::* member
    ) {
      return current.has_value() && std::ranges::none_of(history, [&](const ResolvedWindowRule& previous) {
               return previous.*member == current;
             });
    }

    bool unseenInitialExtentValue(
        const std::optional<int>& currentPixels, const std::optional<double>& currentFraction,
        const std::vector<ResolvedWindowRule>& history, const std::optional<int> ResolvedWindowRule::* pixelsMember,
        const std::optional<double> ResolvedWindowRule::* fractionMember
    ) {
      if (currentPixels) {
        return std::ranges::none_of(history, [&](const ResolvedWindowRule& previous) {
          return previous.*pixelsMember == currentPixels;
        });
      }
      return currentFraction.has_value() && std::ranges::none_of(history, [&](const ResolvedWindowRule& previous) {
               return !(previous.*pixelsMember).has_value() && previous.*fractionMember == currentFraction;
             });
    }
  } // namespace

  using view_detail::windowRuleWorkspace;
  using view_detail::windowRuleWorkspaceGroup;

  // True when this is the only tiled window in the workspace.
  bool View::isAloneInLayout() const {
    return m_tiled
        && m_workspace != nullptr
        && m_workspace->layout().columnOf(this) >= 0
        && m_workspace->isOnlyTiledView(this);
  }

  // Reads the rules with the alone state forced either way, so the alone effect knows what it should change, and so
  // the opening configure can read the alone rules while the view is not in the layout yet.
  ResolvedWindowRule View::resolveRulesWithAlone(bool alone) const {
    WindowRuleState state = ruleState();
    state.alone = alone;
    return resolveWindowRules(
        config(), ruleText(appId()), ruleText(title()), m_xdgTag, m_contentType, state, m_server->uptimeMs()
    );
  }

  // The difference between the alone and not-alone rules. Only the five size-related fields are kept: the others
  // are handled by the normal dynamic rules.
  ResolvedWindowRule View::aloneRuleDiff(const ResolvedWindowRule& alone, const ResolvedWindowRule& other) const {
    ResolvedWindowRule diff;
    if (alone.defaultFullscreen != other.defaultFullscreen) {
      diff.defaultFullscreen = alone.defaultFullscreen;
    }
    if (alone.defaultMaximizeToEdges != other.defaultMaximizeToEdges) {
      diff.defaultMaximizeToEdges = alone.defaultMaximizeToEdges;
    }
    if (alone.defaultMaximize != other.defaultMaximize) {
      diff.defaultMaximize = alone.defaultMaximize;
    }
    if (alone.defaultScrollingExtentPx != other.defaultScrollingExtentPx) {
      diff.defaultScrollingExtentPx = alone.defaultScrollingExtentPx;
    }
    if (alone.defaultScrollingExtent != other.defaultScrollingExtent) {
      diff.defaultScrollingExtent = alone.defaultScrollingExtent;
    }
    return diff;
  }

  // Applies one effect at a time, in the same order as at map time. Returns whether the effect was applied: if the
  // window is already there, or the layout cannot do it, nothing is claimed and leaving alone will not undo anything.
  // The exception is a state the opening configure seeded from these same rules, which this pass takes over.
  bool View::applyAloneRuleEffects(const ResolvedWindowRule& delta) {
    if (delta.defaultFullscreen && *delta.defaultFullscreen) {
      if (scheduledFullscreen() && m_aloneOpeningSeed != AloneSeed::Fullscreen) {
        return false;
      }
      setFullscreen(true);
      m_aloneAction = AloneAction::Fullscreen;
      return true;
    }
    if (delta.defaultMaximizeToEdges && *delta.defaultMaximizeToEdges) {
      if (m_maximizedToEdges || scheduledFullscreen()) {
        return false;
      }
      setMaximizedToEdges(true);
      m_aloneAction = AloneAction::MaximizeToEdges;
      return true;
    }
    if (!openingParented() && delta.defaultMaximize && *delta.defaultMaximize) {
      if (scheduledMaximized() && m_aloneOpeningSeed != AloneSeed::Maximize) {
        return false;
      }
      setMaximized(true);
      m_aloneAction = AloneAction::Maximize;
      return true;
    }
    if ((delta.defaultScrollingExtentPx || delta.defaultScrollingExtent)
        && m_workspace != nullptr
        && !scheduledFullscreen()
        && !m_maximizedToEdges
        && !scheduledMaximized()) {
      ScrollingLayout* scrolling = m_workspace->scrollingLayout();
      if (scrolling != nullptr) {
        const int column = scrolling->columnOf(this);
        if (column >= 0) {
          // Pixel rules take precedence over fraction rules
          if (delta.defaultScrollingExtentPx) {
            const int target = *delta.defaultScrollingExtentPx;
            const double current = scrolling->widthFraction(column);
            if (target != current) {
              m_aloneSavedWidthFrac = current;
              scrolling->setWidthFromPixels(column, m_workspace->scrollViewportExtent(), target);
              m_workspace->markArrange();
              m_aloneAction = AloneAction::Width;
              return true;
            }
          } else if (delta.defaultScrollingExtent) {
            const double target = *delta.defaultScrollingExtent;
            const double current = scrolling->widthFraction(column);
            if (target != current) {
              m_aloneSavedWidthFrac = current;
              scrolling->setWidthFraction(column, target);
              m_workspace->markArrange();
              m_aloneAction = AloneAction::Width;
              return true;
            }
          }
          m_aloneSavedWidthFrac = scrolling->widthFraction(-1);
          m_aloneAction = AloneAction::Width;
          return true;
        }
      }
    }
    return false;
  }

  // Undoes the effect that was applied. For the width, the value saved before is restored, it is cleared even when
  // the window no longer has a column (the layout changed).
  void View::revertAloneRuleEffects() {
    switch (m_aloneAction) {
    case AloneAction::Fullscreen:
      if (scheduledFullscreen()) {
        setFullscreen(false);
      }
      break;
    case AloneAction::MaximizeToEdges:
      if (m_maximizedToEdges) {
        setMaximizedToEdges(false);
      }
      break;
    case AloneAction::Maximize:
      if (scheduledMaximized() && !m_maximizedToEdges) {
        setMaximized(false);
      }
      break;
    case AloneAction::Width:
      if (m_workspace != nullptr) {
        ScrollingLayout* scrolling = m_workspace->scrollingLayout();
        if (scrolling != nullptr) {
          const int column = scrolling->columnOf(this);
          if (column >= 0) {
            const std::optional<double> notAloneWidth = resolveRulesWithAlone(false).defaultScrollingExtent;
            const double restore = notAloneWidth.value_or(m_aloneSavedWidthFrac.value_or(scrolling->widthFraction(-1)));
            scrolling->setWidthFraction(column, restore);
            m_workspace->markArrange();
          }
        }
        m_aloneSavedWidthFrac.reset();
      }
      break;
    case AloneAction::None:
      break;
    }
    m_aloneAction = AloneAction::None;
  }

  // Called by the workspace whenever the tiled windows change or the config reloads. If the window is no longer
  // alone, the applied effect is undone. While alone, the four settings are only re-applied when they changed.
  bool View::notifyAloneStateChanged() {
    if (!m_mapped || m_workspace == nullptr) {
      return false;
    }
    refreshStateRuleEffects();
    const bool alone = isAloneInLayout();
    if (!alone) {
      if (!m_aloneEffectsActive) {
        return false;
      }
      revertAloneRuleEffects();
      m_lastAloneDelta = ResolvedWindowRule{};
      m_aloneEffectsActive = false;
      m_workspace->ensureFocusedVisible();
      return true;
    }
    const ResolvedWindowRule delta = aloneRuleDiff(resolvedRules(), resolveRulesWithAlone(false));
    if (delta == m_lastAloneDelta) {
      return false;
    }
    bool changed = false;
    if (m_aloneEffectsActive) {
      revertAloneRuleEffects();
      changed = true;
    }
    const bool applied = applyAloneRuleEffects(delta);
    m_lastAloneDelta = delta;
    m_aloneEffectsActive = applied;
    return changed || applied;
  }

  // The opening configure states a window that would be the only tiled one from its alone rules, before the view is
  // in the layout. Now that it is, notifyAloneStateChanged hands that state to the alone effect, so leaving alone
  // undoes it again. The seed is single-use: a later pass must not take over a state the user or the client chose.
  void View::settleOpeningAloneState() {
    // Restored maximize only steals alone ownership when the compositor honors it. Otherwise the alone rule owns the
    // state like any other window, and the client's session flag is ignored.
    const bool clientRequested = m_aloneOpeningSeed == AloneSeed::Fullscreen
        ? requestedFullscreen()
        : m_aloneOpeningSeed == AloneSeed::Maximize && requestedMaximized() && config().general.honorRestoredMaximize;
    if (clientRequested) {
      // The client has asked for the state itself since the opening configure, so it owns it.
      m_aloneOpeningSeed = AloneSeed::None;
    } else if (!isAloneInLayout()) {
      // Another window reached the workspace first, so nothing owns the seeded state: neither the client nor the
      // window's not-alone rules. The arrange the attach marked carries the tiled size.
      if (m_aloneOpeningSeed == AloneSeed::Fullscreen) {
        setFullscreen(false, FullscreenExitLayout::DeferToCaller);
      } else if (m_aloneOpeningSeed == AloneSeed::Maximize) {
        setMaximized(false);
      }
    }
    notifyAloneStateChanged();
    m_aloneOpeningSeed = AloneSeed::None;
  }

  bool View::confinePointer() { return resolvedRules().confinePointer.value_or(false); }

  std::optional<bool> View::tearingRuleOverride() { return resolvedRules().allowTearing; }

  void View::applyWindowRules() {
    if (!m_mapped) {
      return;
    }
    // Late app ID or title settlement may select opening rules, but identity hints changed after map must not select
    // new one-shot behavior. is_alone never selects opening settings: alone effects are applied and undone on every
    // change to the workspace's tiled set.
    const ResolvedWindowRule rule = resolveWindowRules(
        config(), m_initialRulesAppId, m_initialRulesTitle, m_initialRulesXdgTag, m_initialRulesContentType,
        m_initialRuleState, m_server->uptimeMs()
    );
    ScratchpadManager* scratchpadManager = m_server->scratchpadManager();
    const bool wasInScratchpad = scratchpadManager != nullptr && scratchpadManager->contains(this);
    const bool scratchpadChanged =
        unseenInitialRuleValue(rule.defaultScratchpad, m_initialRuleHistory, &ResolvedWindowRule::defaultScratchpad);
    const bool defaultFloatingChanged =
        unseenInitialRuleValue(rule.defaultFloating, m_initialRuleHistory, &ResolvedWindowRule::defaultFloating);
    const bool floatingWidthChanged = unseenInitialExtentValue(
        rule.defaultFloatingWidthPx, rule.defaultFloatingWidth, m_initialRuleHistory,
        &ResolvedWindowRule::defaultFloatingWidthPx, &ResolvedWindowRule::defaultFloatingWidth
    );
    const bool floatingHeightChanged = unseenInitialExtentValue(
        rule.defaultFloatingHeightPx, rule.defaultFloatingHeight, m_initialRuleHistory,
        &ResolvedWindowRule::defaultFloatingHeightPx, &ResolvedWindowRule::defaultFloatingHeight
    );
    const bool floatingPositionChanged =
        unseenInitialRuleValue(rule.defaultPosition, m_initialRuleHistory, &ResolvedWindowRule::defaultPosition);
    bool floatingGeometryAppliedOnTransition = false;
    const bool enteringFloatingByRule = !wasInScratchpad && defaultFloatingChanged && *rule.defaultFloating && m_tiled;
    const bool placementChanged =
        (rule.defaultOutput.has_value() || rule.defaultWorkspace.has_value())
        && std::ranges::none_of(m_initialRuleHistory, [&](const ResolvedWindowRule& previous) {
             return previous.defaultOutput == rule.defaultOutput && previous.defaultWorkspace == rule.defaultWorkspace;
           });

    const bool namedScrollingColumnTupleUnseen =
        rule.defaultScrollingColumn.has_value()
        && std::ranges::none_of(m_initialRuleHistory, [&](const ResolvedWindowRule& previous) {
             return previous.defaultScrollingColumn == rule.defaultScrollingColumn
                 && previous.defaultScrollingColumnOrder == rule.defaultScrollingColumnOrder;
           });
    const bool namedScrollingColumnNameChanged =
        namedScrollingColumnTupleUnseen && rule.defaultScrollingColumn != m_namedScrollingColumnName;
    const bool namedScrollingColumnOrderChanged =
        namedScrollingColumnTupleUnseen && rule.defaultScrollingColumn == m_namedScrollingColumnName;
    std::optional<Workspace::NamedScrollingColumnChange> namedScrollingColumnChange;
    if (namedScrollingColumnNameChanged) {
      namedScrollingColumnChange = Workspace::NamedScrollingColumnChange::Name;
    } else if (namedScrollingColumnOrderChanged) {
      namedScrollingColumnChange = Workspace::NamedScrollingColumnChange::Order;
    }
    if (namedScrollingColumnChange) {
      m_namedScrollingColumnName = rule.defaultScrollingColumn;
      m_namedScrollingColumnOrder = rule.defaultScrollingColumnOrder;
    }

    if (!wasInScratchpad && placementChanged && m_workspace != nullptr) {
      const bool wasActivated = m_activated;
      WorkspaceGroup* targetGroup = windowRuleWorkspaceGroup(*m_server, rule, m_workspace->group());
      Workspace* target = windowRuleWorkspace(targetGroup, rule);
      if (target != nullptr && target != m_workspace) {
        setWorkspace(target, false);
        if (m_workspace == target) {
          if (!enteringFloatingByRule) {
            target->layoutAttach(
                this, rule.defaultScrollingExtent, rule.defaultScrollingExtentPx, LayoutAttachOrigin::OpeningView
            );
            if (m_tiled && scheduledMaximized() && !m_maximizedToEdges) {
              setMaximized(true);
            }
          }
          if (wasActivated) {
            m_server->focusView(this);
          }
        }
      }
    }

    // Identity can arrive after map. Apply a newly selected one-shot value, but never replay a value already applied at
    // map over the user's later state.
    // Move first so a simultaneous floating-size rule resolves against the destination output.
    if (!wasInScratchpad && defaultFloatingChanged) {
      const bool wantFloat = *rule.defaultFloating;
      if (wantFloat != !m_tiled) {
        if (wantFloat) {
          if (floatingWidthChanged) {
            m_pendingFloatingWidthPx = rule.defaultFloatingWidthPx;
            m_pendingFloatingWidth = rule.defaultFloatingWidth;
          }
          if (floatingHeightChanged) {
            m_pendingFloatingHeightPx = rule.defaultFloatingHeightPx;
            m_pendingFloatingHeight = rule.defaultFloatingHeight;
          }
          if (floatingPositionChanged) {
            m_pendingFloatingPosition = rule.defaultPosition;
          }
        }
        setFloating(wantFloat);
        floatingGeometryAppliedOnTransition = wantFloat && !m_tiled;
      }
    }

    if (namedScrollingColumnChange && m_workspace != nullptr) {
      m_workspace->applyNamedScrollingColumnRule(
          this, rule.defaultScrollingExtent, rule.defaultScrollingExtentPx, *namedScrollingColumnChange
      );
    }

    std::optional<std::string_view> scratchpadTarget;
    if ((!wasInScratchpad || scratchpadChanged)
        && rule.defaultScratchpad
        && scratchpadManager != nullptr
        && scratchpadManager->hasScratchpad(*rule.defaultScratchpad)) {
      scratchpadTarget = *rule.defaultScratchpad;
    } else if (wasInScratchpad) {
      scratchpadTarget = scratchpadManager->nameFor(this);
    }
    const bool updateScratchpad =
        scratchpadTarget && (scratchpadChanged || (wasInScratchpad && (placementChanged || defaultFloatingChanged)));

    bool assignedScratchpad = false;
    if (updateScratchpad) {
      Output* savedOutput = wasInScratchpad ? scratchpadManager->restoreOutputFor(this) : nullptr;
      Workspace* targetWorkspace = !wasInScratchpad ? m_workspace : nullptr;
      Output* targetOutput = targetWorkspace != nullptr && targetWorkspace->group() != nullptr
          ? targetWorkspace->group()->output()
          : savedOutput;
      if (placementChanged) {
        WorkspaceGroup* fallbackGroup = targetOutput != nullptr ? targetOutput->workspaceGroup() : nullptr;
        WorkspaceGroup* targetGroup = windowRuleWorkspaceGroup(*m_server, rule, fallbackGroup);
        targetWorkspace = windowRuleWorkspace(targetGroup, rule);
        targetOutput = targetGroup != nullptr ? targetGroup->output() : targetOutput;
      }
      if (targetOutput == nullptr) {
        targetOutput = currentOutput();
      }
      std::optional<bool> restoreTiled;
      if (!wasInScratchpad) {
        restoreTiled = m_pinned ? m_restoreTiledAfterUnpin : m_tiled;
      }
      if (defaultFloatingChanged) {
        restoreTiled = !*rule.defaultFloating;
      }
      if (targetOutput != nullptr) {
        const bool wasActivated = m_activated;
        assignedScratchpad = scratchpadManager->assignByWindowRule(
            this, *scratchpadTarget, targetOutput,
            ScratchpadManager::AutomaticAdmission{
                .restoreOutput = targetOutput,
                .restoreWorkspace = targetWorkspace,
                .focusOrigin = currentOutput(),
                .restoreTiled = restoreTiled,
                .updateRestoreLocation = !wasInScratchpad || placementChanged,
            }
        );
        if (assignedScratchpad
            && scratchpadChanged
            && rule.defaultFocused.value_or(false)
            && scratchpadManager->summon(*scratchpadTarget, targetOutput)
            && wasActivated) {
          m_server->focusView(this);
        }
      }
    }
    const bool inScratchpad = wasInScratchpad || assignedScratchpad;
    if (!inScratchpad
        && unseenInitialRuleValue(rule.defaultPinned, m_initialRuleHistory, &ResolvedWindowRule::defaultPinned)) {
      setPinned(*rule.defaultPinned, false);
    }

    ScrollingLayout* scrolling = m_workspace != nullptr ? m_workspace->scrollingLayout() : nullptr;
    const bool defaultExtentChanged = unseenInitialExtentValue(
        rule.defaultScrollingExtentPx, rule.defaultScrollingExtent, m_initialRuleHistory,
        &ResolvedWindowRule::defaultScrollingExtentPx, &ResolvedWindowRule::defaultScrollingExtent
    );
    const bool ownsNamedScrollingColumnExtent =
        m_displacedHome ? m_displacedHome->ownsNamedScrollingColumnExtent : m_ownsNamedScrollingColumnExtent;
    if (defaultExtentChanged
        && m_tiled
        && m_namedScrollingColumnName
        && m_displacedHome
        && ownsNamedScrollingColumnExtent) {
      m_displacedHome->pendingNamedScrollingColumnExtentPx = rule.defaultScrollingExtentPx;
      m_displacedHome->pendingNamedScrollingColumnExtent = rule.defaultScrollingExtent;
    }
    const bool canResizeCurrentNamedScrollingColumn =
        ownsNamedScrollingColumnExtent && (!m_displacedHome || m_ownsNamedScrollingColumnExtent);
    if (defaultExtentChanged
        && m_tiled
        && scrolling != nullptr
        && (!m_namedScrollingColumnName || canResizeCurrentNamedScrollingColumn)) {
      const int column = scrolling->columnOf(this);
      if (column >= 0) {
        if (rule.defaultScrollingExtentPx) {
          scrolling->setWidthFromPixels(column, m_workspace->scrollViewportExtent(), *rule.defaultScrollingExtentPx);
        } else if (rule.defaultScrollingExtent) {
          scrolling->setWidthFraction(column, *rule.defaultScrollingExtent);
        }
        m_workspace->markArrange();
      }
    }
    if (defaultExtentChanged && !m_tiled) {
      m_savedScrollingExtentPx = rule.defaultScrollingExtentPx;
      m_savedScrollingExtent = rule.defaultScrollingExtent;
    }

    if (!inScratchpad && !floatingGeometryAppliedOnTransition && (floatingWidthChanged || floatingHeightChanged)) {
      if (!m_tiled) {
        const wlr_box usable = floatingUsableArea();
        const SizeHints hints = sizeHints();
        auto [width, height] = floatingSize();
        if (floatingWidthChanged) {
          if (rule.defaultFloatingWidthPx) {
            width = clampWidth(*rule.defaultFloatingWidthPx, hints);
          } else if (rule.defaultFloatingWidth && usable.width > 0) {
            width = clampWidth(floatingFractionSize(*rule.defaultFloatingWidth, usable.width), hints);
          }
        }
        if (floatingHeightChanged) {
          if (rule.defaultFloatingHeightPx) {
            height = clampHeight(*rule.defaultFloatingHeightPx, hints);
          } else if (rule.defaultFloatingHeight && usable.height > 0) {
            height = clampHeight(floatingFractionSize(*rule.defaultFloatingHeight, usable.height), hints);
          }
        }
        if (width > 0 && height > 0) {
          requestFloatingSize(width, height);
          placeInUsableArea();
        }
      } else {
        if (floatingWidthChanged) {
          m_pendingFloatingWidthPx = rule.defaultFloatingWidthPx;
          m_pendingFloatingWidth = rule.defaultFloatingWidth;
        }
        if (floatingHeightChanged) {
          m_pendingFloatingHeightPx = rule.defaultFloatingHeightPx;
          m_pendingFloatingHeight = rule.defaultFloatingHeight;
        }
      }
    }

    if (!inScratchpad && !floatingGeometryAppliedOnTransition && floatingPositionChanged) {
      if (!m_tiled) {
        placeInUsableArea(rule.defaultPosition);
      } else {
        m_pendingFloatingPosition = rule.defaultPosition;
      }
    }

    if (!inScratchpad
        && unseenInitialRuleValue(rule.defaultFullscreen, m_initialRuleHistory, &ResolvedWindowRule::defaultFullscreen)
        && *rule.defaultFullscreen
        && !scheduledFullscreen()) {
      setFullscreen(true);
    }

    if (!inScratchpad
        && unseenInitialRuleValue(
            rule.defaultMaximizeToEdges, m_initialRuleHistory, &ResolvedWindowRule::defaultMaximizeToEdges
        )
        && *rule.defaultMaximizeToEdges
        && !m_maximizedToEdges) {
      setMaximizedToEdges(true);
    }

    if (!inScratchpad
        && !openingParented()
        && unseenInitialRuleValue(rule.defaultMaximize, m_initialRuleHistory, &ResolvedWindowRule::defaultMaximize)
        && *rule.defaultMaximize
        && !scheduledMaximized()) {
      setMaximized(true);
    }

    // A later initial identity signal compares against every resolution already considered, so it cannot replay an
    // earlier one-shot value over intervening user state. Keep the latest resolution for later parent changes.
    m_initialRules = rule;
    m_initialRuleHistory.push_back(rule);

    // Dynamic effects use the current identity hints, including ones changed after map.
    applyDynamicRules();
  }

  void View::refreshStartupRuleEffects() {
    m_rulesGeneration = 0;
    if (m_mapped) {
      applyDynamicRules();
    }
  }

  void View::refreshStateRuleEffects() {
    if (m_mapped && m_appliedRuleState != ruleState()) {
      applyDynamicRules();
    }
  }

  WindowRuleState View::ruleState() const {
    return {
        .focused = m_borderFocusedState,
        .floating = !m_tiled,
        .pinned = m_pinned,
        .scratchpad = m_inScratchpad,
        .alone = isAloneInLayout(),
    };
  }

  const ResolvedWindowRule& View::resolvedRules() {
    const std::optional<std::string_view> ruleAppId = ruleText(appId());
    const std::optional<std::string_view> ruleTitle = ruleText(title());
    const uint64_t generation = configStore().generation();
    const WindowRuleState state = ruleState();

    // An unset identity string is a distinct key from an empty one: only the latter matches a pattern accepting the
    // empty string, so a client that replaces a missing title with an empty one must re-resolve.
    if (m_rulesGeneration == generation
        && m_rulesState == state
        && m_rulesAppId == ruleAppId
        && m_rulesTitle == ruleTitle
        && m_rulesXdgTag == m_xdgTag
        && m_rulesContentType == m_contentType) {
      return m_rules;
    }

    m_rules = resolveWindowRules(config(), ruleAppId, ruleTitle, m_xdgTag, m_contentType, state, m_server->uptimeMs());
    m_rulesGeneration = generation;
    m_rulesState = state;
    m_rulesAppId = ruleAppId;
    m_rulesTitle = ruleTitle;
    m_rulesXdgTag = m_xdgTag;
    m_rulesContentType = m_contentType;
    return m_rules;
  }

  void View::refreshEffectSelection() {
    applyDynamicRules();
    scheduleFrame();
    m_server->scheduleIpcWindowsEvent();
  }

  void View::onEffectSelectionIdle(void* data) {
    auto* view = static_cast<View*>(data);
    view->m_effectSelectionIdle = nullptr;
    if (view->m_mapped) {
      view->refreshEffectSelection();
    }
  }

  void View::applyDynamicRules(const ResolvedWindowRule* resolved) {
    const bool frame = m_server->handlingOutputFrame();
    if (!frame && m_effectSelectionIdle != nullptr) {
      wl_event_source_remove(m_effectSelectionIdle);
      m_effectSelectionIdle = nullptr;
    }
    const ResolvedWindowRule& rule = resolved != nullptr ? *resolved : resolvedRules();
    m_appliedRuleState = ruleState();
    // Tile spacing stays on the global border width, so a decoration change redraws this window without an arrange.
    const bool ringChanged = m_decoration.applyRule(rule);
    const auto resolve = [&](EffectKind kind, const std::string& fallback, const std::optional<std::string>& selected) {
      EffectSlot& slot = m_effects.slot(kind);
      const std::string& selector = selected ? *selected : fallback;
      const auto source = selected ? EffectSelectorSource::Rule : EffectSelectorSource::Default;
      if (frame) {
        if (slot.configuredSelector == selector && slot.configuredSource == source) {
          return;
        }
        if ((!selector.empty() && selector != kEffectOff) || !slot.name.empty() || !slot.pool.empty()) {
          if (m_effectSelectionIdle == nullptr) {
            m_effectSelectionIdle =
                wl_event_loop_add_idle(wl_display_get_event_loop(m_server->display()), onEffectSelectionIdle, this);
            if (m_effectSelectionIdle == nullptr) {
              kLog.error("failed to defer effect selection after a frame-time rule change");
            }
          }
          return;
        }
        // Empty/off selectors need no allocation policy or idle source.
      }
      const auto before = std::tuple(slot.name, slot.pool, slot.source(), slot.suppressed);
      slot.configuredSelector = selector;
      slot.configuredSource = source;
      if (!frame) {
        m_server->resolveEffectSlot(slot, kind, m_mapped);
      }
      if (m_mapped && before != std::tuple(slot.name, slot.pool, slot.source(), slot.suppressed)) {
        scheduleFrame();
        m_server->scheduleIpcWindowsEvent();
      }
    };
    resolve(EffectKind::Border, config().effects.border, rule.borderEffect);
    resolve(EffectKind::Window, config().effects.window, rule.windowEffect);
    m_effects.syncNames(config().effects);
    const bool paddingChanged = m_decoration.setBorderPadding(m_effects.borderPadding());
    if (ringChanged || paddingChanged) {
      updateBorderGeometry();
      applyCornerRadius();
      updateShadow();
    }
    if (paddingChanged) {
      // Overview cards lay their rings out from the padding too.
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onViewPresentationChanged(this);
        scheduleFrame();
      }
    }
    const Config::Colors::Border& colors = m_decoration.borderColors();
    const std::array<float, 4>& targetBorder = m_borderFocusedState ? colors.focused : colors.unfocused;
    const auto& borderAnimation = config().animation.border;
    if (m_mapped && m_borderColorAnim.animating() && borderAnimation.enabled) {
      if (m_borderColorAnim.target() != targetBorder) {
        m_borderColorAnim.retarget(targetBorder, borderAnimation.durationMs, borderAnimation.curve);
        scheduleFrame();
      }
    } else {
      m_borderColorAnim.snap(targetBorder);
      m_decoration.setBorderColor(m_borderFocusedState, effectiveOpacity());
    }
    const float newOpacity = rule.opacity ? static_cast<float>(*rule.opacity) : 1.0F;
    if (newOpacity != m_ruleOpacity) {
      m_ruleOpacity = newOpacity;
      setFadeAlpha(m_fadeAlpha); // refresh effective opacity
    }
    m_presentation.setFullscreenOpaque(fullscreenOpaque());
    updateBlur();
    // updateBlur creates the full node box. Re-apply the owning output's clip immediately, because focus, title, and
    // app-id rule refreshes do not necessarily produce a later surface commit or layout pass.
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
    }
    if (m_mapped) {
      // Static effects and frozen clocks may never trigger another animation tick.
      syncAnimationEffects();
      m_server->refreshOutputPolicies();
    }
  }
} // namespace umbriel
