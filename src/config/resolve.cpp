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
      const Config& config, const char* appId, const char* title, std::string_view xdgTag, ContentType contentType,
      bool focused
  ) {
    ResolvedWindowRule resolved;
    const std::string_view appIdView = appId != nullptr ? appId : "";
    const std::string_view titleView = title != nullptr ? title : "";

    for (const auto& rule : config.windowRules) {
      if (!rule.appIdPattern.empty()) {
        if (appIdView.empty() || !std::regex_search(appIdView.begin(), appIdView.end(), rule.appIdRegex)) {
          continue;
        }
      }
      if (!rule.titlePattern.empty()) {
        if (titleView.empty() || !std::regex_search(titleView.begin(), titleView.end(), rule.titleRegex)) {
          continue;
        }
      }
      if (!rule.xdgTagPattern.empty()) {
        if (xdgTag.empty() || !std::regex_search(xdgTag.begin(), xdgTag.end(), rule.xdgTagRegex)) {
          continue;
        }
      }
      if (rule.matchContentType && *rule.matchContentType != contentType) {
        continue;
      }
      if (rule.matchFocused && *rule.matchFocused != focused) {
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

  ResolvedLayerRule resolveLayerRules(const Config& config, const char* layerNamespace) {
    ResolvedLayerRule resolved;
    const std::string_view namespaceView = layerNamespace != nullptr ? layerNamespace : "";
    for (const auto& rule : config.layerRules) {
      if (!rule.namespacePattern.empty()) {
        if (namespaceView.empty()
            || !std::regex_search(namespaceView.begin(), namespaceView.end(), rule.namespaceRegex)) {
          continue;
        }
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

  bool anyWindowRuleHasTitlePattern(const Config& config) {
    return std::ranges::any_of(config.windowRules, [](const WindowRule& rule) { return !rule.titlePattern.empty(); });
  }

  ResolvedLayoutConfig resolveGlobalLayout(const Config& config) {
    ResolvedLayoutConfig resolved;
    resolved.mode = config.layout.mode;
    resolved.gap = config.layout.gap;
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
  resolveWorkspaceLayout(const Config& config, const char* outputName, std::string_view name, size_t index) {
    const std::string_view outName = outputName != nullptr ? outputName : "";
    ResolvedLayoutConfig resolved = resolveGlobalLayout(config);
    const auto applyMatchingRules = [&](std::string_view output) {
      for (const auto& rule : config.workspaceRules) {
        if (rule.output == output
            && ((rule.index && static_cast<size_t>(*rule.index - 1) == index) || (!rule.index && rule.name == name))) {
          applyWorkspaceLayoutOverrides(config, resolved, rule.layout);
        }
      }
    };

    applyMatchingRules("");
    if (!outName.empty()) {
      applyMatchingRules(outName);
    }
    return resolved;
  }

  ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const char* outputName) {
    const std::string_view outName = outputName != nullptr ? outputName : "";
    const auto names = workspaceNamesForOutput(config, outName);
    ResolvedWorkspaceSet result;
    if (!names) {
      result.dynamic = true;
      const size_t count = config.workspaces.emptyAbove ? 2 : 1;
      result.workspaces.reserve(count);
      for (size_t index = 0; index < count; ++index) {
        const std::string name = std::to_string(index + 1);
        result.workspaces.push_back({name, resolveWorkspaceLayout(config, outputName, name, index)});
      }
      return result;
    }

    result.workspaces.reserve(names->size());
    for (size_t index = 0; index < names->size(); ++index) {
      const auto& name = (*names)[index];
      result.workspaces.push_back({name, resolveWorkspaceLayout(config, outputName, name, index)});
    }
    return result;
  }

} // namespace umbriel
