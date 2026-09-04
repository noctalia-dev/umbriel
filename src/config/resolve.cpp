#include "config/resolve.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
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

    std::optional<std::vector<std::string>>
    workspaceNamesForIdentity(const Config& config, const OutputIdentity& identity) {
      const OutputRule* rule = matchingOutputRule(config, identity);
      return rule != nullptr && rule->workspaces ? rule->workspaces : std::nullopt;
    }

    std::optional<std::vector<std::string>> workspaceNamesForOutput(const Config& config, std::string_view outputName) {
      const auto rule = std::ranges::find_if(config.outputs, [&](const OutputRule& candidate) {
        return candidate.name == outputName;
      });
      if (rule != config.outputs.end() && rule->workspaces) {
        return rule->workspaces;
      }
      return std::nullopt;
    }

    bool workspaceRuleMatches(const WorkspaceConfig& rule, const std::vector<std::string>& names) {
      if (rule.index) {
        return static_cast<size_t>(*rule.index) <= names.size();
      }
      return std::ranges::find(names, rule.name) != names.end();
    }

    bool dynamicRuleMatches(const WorkspaceConfig& rule) {
      if (rule.index) {
        return *rule.index >= 1 && static_cast<size_t>(*rule.index) <= kMaxWorkspaces;
      }
      if (rule.name.empty()
          || !std::ranges::all_of(rule.name, [](char value) { return value >= '0' && value <= '9'; })) {
        return false;
      }
      size_t index = 0;
      const auto [end, error] = std::from_chars(rule.name.data(), rule.name.data() + rule.name.size(), index);
      return error == std::errc{}
      && end == rule.name.data() + rule.name.size()
          && index >= 1
          && index <= kMaxWorkspaces;
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
      if (overrides.scrolling.direction) {
        resolved.scrolling.direction = *overrides.scrolling.direction;
      }
      if (overrides.scrolling.expandSingleColumn) {
        resolved.scrolling.expandSingleColumn = *overrides.scrolling.expandSingleColumn;
      }
      if (overrides.master.defaultWidthFraction) {
        resolved.master.defaultWidthFraction = *overrides.master.defaultWidthFraction;
      }
      if (overrides.master.newOnTop) {
        resolved.master.newOnTop = *overrides.master.newOnTop;
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
      if (!output.workspaces || index >= output.workspaces->size()) {
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

  bool workspaceRuleTargetExists(const Config& config, const WorkspaceConfig& rule) {
    if (!rule.output.empty()) {
      const auto names = workspaceNamesForOutput(config, rule.output);
      return names ? workspaceRuleMatches(rule, *names) : dynamicRuleMatches(rule);
    }

    if (dynamicRuleMatches(rule)) {
      return true;
    }
    for (const auto& output : config.outputs) {
      const auto names = workspaceNamesForOutput(config, output.name);
      if (names && workspaceRuleMatches(rule, *names)) {
        return true;
      }
    }
    return false;
  }

  ResolvedWindowRule resolveWindowRules(
      const Config& config, std::optional<std::string_view> appId, std::optional<std::string_view> title,
      std::optional<std::string_view> xdgTag, ContentType contentType, bool focused, uint64_t uptimeMs
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
      if (rule.matchFocused && *rule.matchFocused != focused) {
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
      if (rule.defaultSize) {
        resolved.defaultSize = rule.defaultSize;
      }
      if (rule.defaultPosition) {
        resolved.defaultPosition = rule.defaultPosition;
      }
      if (rule.defaultWidth) {
        resolved.defaultWidth = rule.defaultWidth;
      }
      if (rule.defaultHeight) {
        resolved.defaultHeight = rule.defaultHeight;
      }
      if (rule.defaultWorkspace) {
        resolved.defaultWorkspace = rule.defaultWorkspace;
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
    resolved.scrolling.direction = config.layout.scrolling.direction;
    resolved.scrolling.expandSingleColumn = config.layout.scrolling.expandSingleColumn;
    resolved.dwindle.preserveSplit = config.layout.dwindle.preserveSplit;
    resolved.master.defaultWidthFraction = config.layout.master.defaultWidthFraction;
    resolved.master.newOnTop = config.layout.master.newOnTop;
    resolved.master.position = config.layout.master.position;
    const int borderWidth = config.appearance.totalBorderWidth();
    resolved.totalGap = resolved.gap + 2 * borderWidth;
    resolved.edgePad = resolved.gap + borderWidth;
    return resolved;
  }

  ResolvedLayoutConfig
  resolveWorkspaceLayout(const Config& config, const OutputIdentity& identity, std::string_view name, size_t index) {
    ResolvedLayoutConfig resolved = resolveGlobalLayout(config);
    if (const OutputRule* output = matchingOutputRule(config, identity);
        output != nullptr && output->layout.scrolling.defaultWidthFraction) {
      resolved.scrolling.defaultWidthFraction = output->layout.scrolling.defaultWidthFraction;
    }
    const auto applyMatchingRules = [&](bool outputScoped) {
      for (const auto& rule : config.workspaceRules) {
        const bool outputMatches = outputScoped
            ? !rule.output.empty() && outputNameMatch(identity, rule.output) != OutputNameMatch::None
            : rule.output.empty();
        if (outputMatches
            && ((rule.index && static_cast<size_t>(*rule.index - 1) == index) || (!rule.index && rule.name == name))) {
          applyWorkspaceLayoutOverrides(config, resolved, rule.layout);
        }
      }
    };
    applyMatchingRules(false);
    applyMatchingRules(true);
    return resolved;
  }

  size_t resolveDynamicWorkspaceMinimum(const Config& config, const OutputIdentity& identity) {
    const OutputRule* rule = matchingOutputRule(config, identity);
    return rule != nullptr ? static_cast<size_t>(std::max(1, rule->minWorkspaces)) : 1;
  }

  ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const OutputIdentity& identity) {
    const auto names = workspaceNamesForIdentity(config, identity);
    ResolvedWorkspaceSet result;
    if (!names) {
      result.dynamic = true;
      // The optional leading empty is an extra entry only while it exceeds the
      // configured minimum.
      const size_t sentinels = config.workspaces.emptyAbove ? 2 : 1;
      const size_t count = std::max(resolveDynamicWorkspaceMinimum(config, identity), sentinels);
      result.workspaces.reserve(count);
      for (size_t index = 0; index < count; ++index) {
        const std::string name = std::to_string(index + 1);
        result.workspaces.push_back({name, resolveWorkspaceLayout(config, identity, name, index)});
      }
      return result;
    }

    result.workspaces.reserve(names->size());
    for (size_t index = 0; index < names->size(); ++index) {
      const auto& name = (*names)[index];
      result.workspaces.push_back({name, resolveWorkspaceLayout(config, identity, name, index)});
    }
    return result;
  }

} // namespace umbriel
