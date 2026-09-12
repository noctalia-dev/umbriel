#include "config/resolve.h"

#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace umbriel {

  namespace {

    const OutputRule* matchingOutputRule(const Config& config, const OutputIdentity& identity) {
      const OutputRule* connectorRule = nullptr;
      for (const OutputRule& rule : config.outputs) {
        switch (outputNameMatch(identity, rule.name)) {
        case OutputNameMatch::Descriptor:
          return &rule;
        case OutputNameMatch::Connector:
          connectorRule = &rule;
          break;
        case OutputNameMatch::None:
          break;
        }
      }
      return connectorRule;
    }

    const OutputRule::WorkspaceInventory*
    workspaceInventoryForIdentity(const Config& config, const OutputIdentity& identity) {
      const OutputRule* rule = matchingOutputRule(config, identity);
      return rule != nullptr && rule->workspaces ? &*rule->workspaces : nullptr;
    }

    const OutputRule::WorkspaceInventory*
    workspaceInventoryForOutput(const Config& config, std::string_view outputName) {
      const auto rule = std::ranges::find_if(config.outputs, [&](const OutputRule& candidate) {
        return outputNamesEqual(candidate.name, outputName);
      });
      if (rule != config.outputs.end() && rule->workspaces) {
        return &*rule->workspaces;
      }
      return nullptr;
    }

    size_t workspaceInventorySize(const OutputRule::WorkspaceInventory& inventory) {
      if (const auto* count = std::get_if<size_t>(&inventory)) {
        return *count;
      }
      return std::get<std::vector<std::string>>(inventory).size();
    }

    bool workspaceRuleMatches(const WorkspaceConfig& rule, const OutputRule::WorkspaceInventory& inventory) {
      if (rule.index) {
        return static_cast<size_t>(*rule.index) <= workspaceInventorySize(inventory);
      }
      const auto* names = std::get_if<std::vector<std::string>>(&inventory);
      return names != nullptr && std::ranges::find(*names, rule.name) != names->end();
    }

    bool dynamicRuleMatches(const WorkspaceConfig& rule) {
      if (rule.index) {
        return *rule.index >= 1 && static_cast<size_t>(*rule.index) <= kMaxWorkspaces;
      }
      // A name rule declares a persistent named member of every matching
      // dynamic output. Numeric-looking names remain names and are not folded
      // into positional selectors.
      return !rule.name.empty();
    }

    std::vector<std::string> dynamicWorkspaceNames(const Config& config, const OutputIdentity& identity) {
      std::vector<std::string> names;
      for (const WorkspaceConfig& rule : config.workspaceRules) {
        if (rule.name.empty()
            || (!rule.output.empty() && outputNameMatch(identity, rule.output) == OutputNameMatch::None)
            || std::ranges::find(names, rule.name) != names.end()) {
          continue;
        }
        names.push_back(rule.name);
      }
      return names;
    }

    void applyWorkspaceLayoutOverrides(
        const Config& config, ResolvedLayoutConfig& resolved, const WorkspaceLayoutOverrides& overrides
    ) {
      if (overrides.mode) {
        resolved.mode = *overrides.mode;
      }
      if (overrides.gap) {
        resolved.gap = *overrides.gap;
      }
      if (overrides.struts.left) {
        resolved.struts.left = *overrides.struts.left;
      }
      if (overrides.struts.right) {
        resolved.struts.right = *overrides.struts.right;
      }
      if (overrides.struts.top) {
        resolved.struts.top = *overrides.struts.top;
      }
      if (overrides.struts.bottom) {
        resolved.struts.bottom = *overrides.struts.bottom;
      }
      if (overrides.scrolling.defaultWidthFraction) {
        resolved.scrolling.defaultWidthFraction = overrides.scrolling.defaultWidthFraction;
      }
      if (overrides.scrolling.centerUnderfullStrip) {
        resolved.scrolling.centerUnderfullStrip = *overrides.scrolling.centerUnderfullStrip;
      }
      if (overrides.scrolling.centerFocused) {
        resolved.scrolling.centerFocused = *overrides.scrolling.centerFocused;
      }
      if (overrides.dwindle.preserveSplit) {
        resolved.dwindle.preserveSplit = *overrides.dwindle.preserveSplit;
      }
      if (overrides.master.defaultWidthFraction) {
        resolved.master.defaultWidthFraction = *overrides.master.defaultWidthFraction;
      }
      if (overrides.master.newOnTop) {
        resolved.master.newOnTop = *overrides.master.newOnTop;
      }
      if (overrides.master.newBecomesMaster) {
        resolved.master.newBecomesMaster = *overrides.master.newBecomesMaster;
      }
      if (overrides.master.position) {
        resolved.master.position = *overrides.master.position;
      }
      if (overrides.widthPresets) {
        resolved.widthPresets = *overrides.widthPresets;
      }
      const int borderWidth = config.appearance.totalBorderWidth();
      resolved.totalGap = resolved.gap + 2 * borderWidth;
      resolved.edgePad = resolved.gap + borderWidth;
    }

    // A rule without a pattern places no constraint. With one, the value must be present: a client that never set an
    // identity string matches nothing, while one that set it to empty matches a pattern accepting the empty string.
    bool
    patternMatches(const std::string& pattern, const std::regex& regex, const std::optional<std::string_view>& value) {
      if (pattern.empty()) {
        return true;
      }
      return value.has_value() && std::regex_search(value->begin(), value->end(), regex);
    }

  } // namespace

  const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, size_t index) {
    const OutputRule* owner = nullptr;
    for (const OutputRule& output : config.outputs) {
      if (!output.workspaces || index >= workspaceInventorySize(*output.workspaces)) {
        continue;
      }
      if (owner != nullptr) {
        return nullptr;
      }
      owner = &output;
    }
    return owner;
  }

  const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, std::string_view name) {
    const OutputRule* owner = nullptr;
    for (const OutputRule& output : config.outputs) {
      const auto* names = output.workspaces ? std::get_if<std::vector<std::string>>(&*output.workspaces) : nullptr;
      if (names == nullptr || std::ranges::find(*names, name) == names->end()) {
        continue;
      }
      if (owner != nullptr) {
        return nullptr;
      }
      owner = &output;
    }
    return owner;
  }

  const OutputRule* findOutputRule(const Config& config, const OutputIdentity& identity) {
    return matchingOutputRule(config, identity);
  }

  WorkspaceAxis resolveWorkspaceAxis(const Config& config, const OutputIdentity& identity) {
    const OutputRule* rule = matchingOutputRule(config, identity);
    return rule != nullptr ? rule->workspaceAxis : WorkspaceAxis::Vertical;
  }

  bool workspaceRuleTargetExists(const Config& config, const WorkspaceConfig& rule) {
    if (!rule.output.empty()) {
      const auto* inventory = workspaceInventoryForOutput(config, rule.output);
      return inventory != nullptr ? workspaceRuleMatches(rule, *inventory) : dynamicRuleMatches(rule);
    }

    if (dynamicRuleMatches(rule)) {
      return true;
    }
    for (const auto& output : config.outputs) {
      const auto* inventory = workspaceInventoryForOutput(config, output.name);
      if (inventory != nullptr && workspaceRuleMatches(rule, *inventory)) {
        return true;
      }
    }
    return false;
  }

  ResolvedWindowRule resolveWindowRules(
      const Config& config, std::optional<std::string_view> appId, std::optional<std::string_view> title,
      std::optional<std::string_view> xdgTag, ContentType contentType, const WindowRuleState& state, uint64_t uptimeMs
  ) {
    ResolvedWindowRule resolved;

    for (const auto& rule : config.windowRules) {
      if (!patternMatches(rule.appIdPattern, rule.appIdRegex, appId)
          || !patternMatches(rule.titlePattern, rule.titleRegex, title)
          || !patternMatches(rule.xdgTagPattern, rule.xdgTagRegex, xdgTag)) {
        continue;
      }
      if (rule.matchContentType && *rule.matchContentType != contentType) {
        continue;
      }
      if (rule.matchFocused && *rule.matchFocused != state.focused) {
        continue;
      }
      if (rule.matchFloating && *rule.matchFloating != state.floating) {
        continue;
      }
      if (rule.matchPinned && *rule.matchPinned != state.pinned) {
        continue;
      }
      if (rule.matchScratchpad && *rule.matchScratchpad != state.scratchpad) {
        continue;
      }
      if (rule.matchAlone && *rule.matchAlone != state.alone) {
        continue;
      }
      if (rule.matchAtStartup && *rule.matchAtStartup != (uptimeMs < kStartupWindowRuleDurationMs)) {
        continue;
      }
      // Last writer wins: overwrite each field the rule sets.
      if (rule.defaultOutput) {
        resolved.defaultOutput = rule.defaultOutput;
      }
      if (rule.defaultFloating) {
        resolved.defaultFloating = rule.defaultFloating;
      }
      if (rule.defaultFloatingSizePx) {
        resolved.defaultFloatingSizePx = rule.defaultFloatingSizePx;
      }
      if (rule.defaultFloatingSize) {
        resolved.defaultFloatingSize = rule.defaultFloatingSize;
      }
      if (rule.defaultPosition) {
        resolved.defaultPosition = rule.defaultPosition;
      }
      if (rule.defaultScrollingWidthPx) {
        resolved.defaultScrollingWidthPx = rule.defaultScrollingWidthPx;
      }
      if (rule.defaultScrollingWidth) {
        resolved.defaultScrollingWidth = rule.defaultScrollingWidth;
      }
      if (rule.defaultWorkspace) {
        resolved.defaultWorkspace = rule.defaultWorkspace;
      }
      if (rule.defaultScratchpad) {
        resolved.defaultScratchpad = rule.defaultScratchpad;
      }
      if (rule.defaultScrollingColumn) {
        resolved.defaultScrollingColumn = rule.defaultScrollingColumn;
      }
      if (rule.defaultScrollingColumnOrder) {
        resolved.defaultScrollingColumnOrder = rule.defaultScrollingColumnOrder;
      }
      if (rule.defaultFullscreen) {
        resolved.defaultFullscreen = rule.defaultFullscreen;
      }
      if (rule.defaultMaximizeToEdges) {
        resolved.defaultMaximizeToEdges = rule.defaultMaximizeToEdges;
      }
      if (rule.defaultMaximize) {
        resolved.defaultMaximize = rule.defaultMaximize;
      }
      if (rule.defaultFocused) {
        resolved.defaultFocused = rule.defaultFocused;
      }
      if (rule.defaultPinned) {
        resolved.defaultPinned = rule.defaultPinned;
      }
      if (rule.focusOnActivate) {
        resolved.focusOnActivate = rule.focusOnActivate;
      }
      if (rule.vrr) {
        resolved.vrr = rule.vrr;
      }
      if (rule.allowTearing) {
        resolved.allowTearing = rule.allowTearing;
      }
      if (rule.hdr) {
        resolved.hdr = rule.hdr;
      }
      if (rule.opacity) {
        resolved.opacity = rule.opacity;
      }
      if (rule.blur) {
        resolved.blur = rule.blur;
      }
      if (rule.blurPopups) {
        resolved.blurPopups = rule.blurPopups;
      }
      if (rule.blurIgnoreAlpha) {
        resolved.blurIgnoreAlpha = rule.blurIgnoreAlpha;
      }
      if (rule.blurOptimized) {
        resolved.blurOptimized = rule.blurOptimized;
      }
    }
    return resolved;
  }

  ResolvedLayerRule resolveLayerRules(const Config& config, std::optional<std::string_view> layerNamespace) {
    ResolvedLayerRule resolved;
    for (const auto& rule : config.layerRules) {
      if (!patternMatches(rule.namespacePattern, rule.namespaceRegex, layerNamespace)) {
        continue;
      }
      if (rule.blur) {
        resolved.blur = rule.blur;
      }
      if (rule.blurPopups) {
        resolved.blurPopups = rule.blurPopups;
      }
      if (rule.ignoreAlpha) {
        resolved.ignoreAlpha = rule.ignoreAlpha;
      }
      if (rule.optimized) {
        resolved.optimized = rule.optimized;
      }
    }
    return resolved;
  }

  std::vector<std::string>
  securityContextRuleGlobals(const Config& config, const char* sandboxEngine, const char* appId) {
    const std::string_view engineView = sandboxEngine != nullptr ? sandboxEngine : "";
    const std::string_view appIdView = appId != nullptr ? appId : "";
    std::vector<std::string> globals;
    for (const auto& rule : config.securityContextRules) {
      // Unlike window rules, the whole value must match: a substring grant
      // would let an application choose an ID embedding someone else's pattern.
      if (!rule.sandboxEnginePattern.empty()) {
        if (engineView.empty() || !std::regex_match(engineView.begin(), engineView.end(), rule.sandboxEngineRegex)) {
          continue;
        }
      }
      if (!rule.appIdPattern.empty()) {
        if (appIdView.empty() || !std::regex_match(appIdView.begin(), appIdView.end(), rule.appIdRegex)) {
          continue;
        }
      }
      for (const std::string& global : rule.allowGlobals) {
        if (std::ranges::find(globals, global) == globals.end()) {
          globals.push_back(global);
        }
      }
    }
    return globals;
  }

  bool anyWindowRuleHasTitlePattern(const Config& config) {
    return std::ranges::any_of(config.windowRules, [](const WindowRule& rule) { return !rule.titlePattern.empty(); });
  }

  ResolvedLayoutConfig resolveGlobalLayout(const Config& config) {
    ResolvedLayoutConfig resolved;
    resolved.mode = config.layout.mode;
    resolved.gap = config.layout.gap;
    resolved.struts = config.layout.struts;
    resolved.widthPresets = config.layout.widthPresets;
    resolved.scrolling.defaultWidthFraction = config.layout.scrolling.defaultWidthFraction;
    resolved.scrolling.centerUnderfullStrip = config.layout.scrolling.centerUnderfullStrip;
    resolved.scrolling.centerFocused = config.layout.scrolling.centerFocused;
    resolved.dwindle.preserveSplit = config.layout.dwindle.preserveSplit;
    resolved.master.defaultWidthFraction = config.layout.master.defaultWidthFraction;
    resolved.master.newOnTop = config.layout.master.newOnTop;
    resolved.master.newBecomesMaster = config.layout.master.newBecomesMaster;
    resolved.master.position = config.layout.master.position;
    const int borderWidth = config.appearance.totalBorderWidth();
    resolved.totalGap = resolved.gap + 2 * borderWidth;
    resolved.edgePad = resolved.gap + borderWidth;
    return resolved;
  }

  namespace {
    ResolvedLayoutConfig resolveWorkspaceLayoutImpl(
        const Config& config, const OutputIdentity& identity, std::optional<std::string_view> name, size_t index
    ) {
      ResolvedLayoutConfig resolved = resolveGlobalLayout(config);
      const OutputRule* output = matchingOutputRule(config, identity);
      if (output != nullptr) {
        if (output->layout.scrolling.defaultWidthFraction) {
          resolved.scrolling.defaultWidthFraction = output->layout.scrolling.defaultWidthFraction;
        }
        resolved.scrolling.direction = output->workspaceAxis == WorkspaceAxis::Horizontal
            ? ScrollingDirection::Vertical
            : ScrollingDirection::Horizontal;
      }
      const auto applyMatchingRules = [&](bool outputScoped) {
        for (const auto& rule : config.workspaceRules) {
          const bool outputMatches = outputScoped
              ? !rule.output.empty() && outputNameMatch(identity, rule.output) != OutputNameMatch::None
              : rule.output.empty();
          if (outputMatches
              && ((rule.index && static_cast<size_t>(*rule.index - 1) == index)
                  || (!rule.index && name && rule.name == *name))) {
            applyWorkspaceLayoutOverrides(config, resolved, rule.layout);
          }
        }
      };
      applyMatchingRules(false);
      applyMatchingRules(true);
      return resolved;
    }
  } // namespace

  ResolvedLayoutConfig
  resolveWorkspaceLayout(const Config& config, const OutputIdentity& identity, std::string_view name, size_t index) {
    return resolveWorkspaceLayoutImpl(config, identity, name, index);
  }

  ResolvedLayoutConfig
  resolveUnnamedWorkspaceLayout(const Config& config, const OutputIdentity& identity, size_t index) {
    return resolveWorkspaceLayoutImpl(config, identity, std::nullopt, index);
  }

  size_t resolveDynamicWorkspaceMinimum(const Config& config, const OutputIdentity& identity) {
    const OutputRule* rule = matchingOutputRule(config, identity);
    return rule != nullptr ? static_cast<size_t>(std::max(1, rule->minWorkspaces)) : 1;
  }

  ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const OutputIdentity& identity) {
    const auto* inventory = workspaceInventoryForIdentity(config, identity);
    ResolvedWorkspaceSet result;
    if (inventory == nullptr) {
      result.dynamic = true;
      std::vector<std::string> named = dynamicWorkspaceNames(config, identity);
      const size_t sentinels = config.workspaces.emptyAbove ? 2 : 1;
      if (named.size() > kMaxWorkspaces - sentinels) {
        result.omittedNamed = named.size() - (kMaxWorkspaces - sentinels);
        named.resize(kMaxWorkspaces - sentinels);
      }
      const size_t count = std::max(resolveDynamicWorkspaceMinimum(config, identity), named.size() + sentinels);
      result.workspaces.reserve(count);

      const auto appendUnnamed = [&] {
        const size_t index = result.workspaces.size();
        const std::string name = std::to_string(index + 1);
        result.workspaces.push_back({name, false, resolveUnnamedWorkspaceLayout(config, identity, index)});
      };
      if (config.workspaces.emptyAbove) {
        appendUnnamed();
      }
      for (std::string& name : named) {
        const size_t index = result.workspaces.size();
        ResolvedLayoutConfig layout = resolveWorkspaceLayout(config, identity, name, index);
        result.workspaces.push_back({std::move(name), true, std::move(layout)});
      }
      while (result.workspaces.size() < count) {
        appendUnnamed();
      }
      return result;
    }

    if (const auto* count = std::get_if<size_t>(inventory)) {
      result.workspaces.reserve(*count);
      for (size_t index = 0; index < *count; ++index) {
        result.workspaces.push_back(
            {std::to_string(index + 1), false, resolveUnnamedWorkspaceLayout(config, identity, index)}
        );
      }
      return result;
    }

    const auto& names = std::get<std::vector<std::string>>(*inventory);
    result.workspaces.reserve(names.size());
    for (size_t index = 0; index < names.size(); ++index) {
      const auto& name = names[index];
      result.workspaces.push_back({name, true, resolveWorkspaceLayout(config, identity, name, index)});
    }
    return result;
  }

} // namespace umbriel
