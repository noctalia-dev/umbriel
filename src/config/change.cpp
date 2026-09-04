#include "config/change.h"

#include "output/identity.h"

#include <algorithm>
#include <string_view>

namespace umbriel {
  namespace {

    const OutputRule* findOutputRule(const Config& config, const std::string& name) {
      const auto rule = std::ranges::find_if(config.outputs, [&](const OutputRule& candidate) {
        return outputNamesEqual(candidate.name, name);
      });
      return rule != config.outputs.end() ? &*rule : nullptr;
    }

    bool outputRuleNamesChanged(const Config& before, const Config& after) {
      if (before.outputs.size() != after.outputs.size()) {
        return true;
      }
      return std::ranges::any_of(before.outputs, [&](const OutputRule& rule) {
        return findOutputRule(after, rule.name) == nullptr;
      });
    }

    template <typename Equal> bool outputProjectionChanged(const Config& before, const Config& after, Equal equal) {
      for (const OutputRule& rule : before.outputs) {
        if (!equal(&rule, findOutputRule(after, rule.name))) {
          return true;
        }
      }
      for (const OutputRule& rule : after.outputs) {
        if (findOutputRule(before, rule.name) == nullptr && !equal(nullptr, &rule)) {
          return true;
        }
      }
      return false;
    }

    bool sameOutputState(const OutputRule* before, const OutputRule* after) {
      static const OutputRule defaults;
      const OutputRule& lhs = before != nullptr ? *before : defaults;
      const OutputRule& rhs = after != nullptr ? *after : defaults;
      return lhs.enabled == rhs.enabled
          && lhs.mode == rhs.mode
          && lhs.position == rhs.position
          && lhs.scale == rhs.scale
          && lhs.transform == rhs.transform
          && lhs.vrr == rhs.vrr
          && lhs.hdr == rhs.hdr
          && lhs.sdrWhite == rhs.sdrWhite;
    }

    bool sameOutputTearingPolicy(const OutputRule* before, const OutputRule* after) {
      static const OutputRule defaults;
      const OutputRule& lhs = before != nullptr ? *before : defaults;
      const OutputRule& rhs = after != nullptr ? *after : defaults;
      return lhs.allowTearing == rhs.allowTearing;
    }

    bool sameOutputDirectScanoutPolicy(const OutputRule* before, const OutputRule* after) {
      static const OutputRule defaults;
      const OutputRule& lhs = before != nullptr ? *before : defaults;
      const OutputRule& rhs = after != nullptr ? *after : defaults;
      return lhs.directScanout == rhs.directScanout;
    }

    bool sameOutputLayout(const OutputRule* before, const OutputRule* after) {
      static const OutputRule defaults;
      const OutputRule& lhs = before != nullptr ? *before : defaults;
      const OutputRule& rhs = after != nullptr ? *after : defaults;
      return lhs.layout == rhs.layout;
    }

    bool sameWindowTearingPolicy(const Config& before, const Config& after) {
      size_t beforeIndex = 0;
      size_t afterIndex = 0;
      while (true) {
        while (beforeIndex < before.windowRules.size() && !before.windowRules[beforeIndex].allowTearing.has_value()) {
          ++beforeIndex;
        }
        while (afterIndex < after.windowRules.size() && !after.windowRules[afterIndex].allowTearing.has_value()) {
          ++afterIndex;
        }
        const bool beforeEnd = beforeIndex == before.windowRules.size();
        const bool afterEnd = afterIndex == after.windowRules.size();
        if (beforeEnd || afterEnd) {
          return beforeEnd && afterEnd;
        }

        const WindowRule& lhs = before.windowRules[beforeIndex++];
        const WindowRule& rhs = after.windowRules[afterIndex++];
        if (lhs.appIdPattern != rhs.appIdPattern
            || lhs.titlePattern != rhs.titlePattern
            || lhs.xdgTagPattern != rhs.xdgTagPattern
            || lhs.matchContentType != rhs.matchContentType
            || lhs.matchFocused != rhs.matchFocused
            || lhs.matchAtStartup != rhs.matchAtStartup
            || lhs.allowTearing != rhs.allowTearing) {
          return false;
        }
      }
    }

    bool sameWorkspaceInventory(const OutputRule* before, const OutputRule* after) {
      static const OutputRule defaults;
      const OutputRule& lhs = before != nullptr ? *before : defaults;
      const OutputRule& rhs = after != nullptr ? *after : defaults;
      return lhs.workspaces == rhs.workspaces && lhs.minWorkspaces == rhs.minWorkspaces;
    }

  } // namespace

  ConfigEffects ConfigEffects::between(const Config& before, const Config& after) {
    // A new descriptor rule can override an existing connector rule for a live
    // output. Without runtime identities here, a name-set change must
    // conservatively refresh every output projection.
    const bool outputNamesChanged = outputRuleNamesChanged(before, after);
    const bool outputState = outputNamesChanged || outputProjectionChanged(before, after, sameOutputState);
    const bool tearingPolicy = outputNamesChanged
        || outputProjectionChanged(before, after, sameOutputTearingPolicy)
        || !sameWindowTearingPolicy(before, after);
    const bool directScanoutPolicy =
        outputNamesChanged || outputProjectionChanged(before, after, sameOutputDirectScanoutPolicy);
    const bool workspaceInventory = outputNamesChanged
        || outputProjectionChanged(before, after, sameWorkspaceInventory)
        || before.workspaces.emptyAbove != after.workspaces.emptyAbove;
    const bool outputLayout = outputNamesChanged || outputProjectionChanged(before, after, sameOutputLayout);
    const bool sceneBlur =
        before.appearance.blur != after.appearance.blur || before.optimizedBlurNeeded() != after.optimizedBlurNeeded();
    const bool focusDim = before.animation.enabled != after.animation.enabled
        || before.animation.dimUnfocused != after.animation.dimUnfocused;
    return {
        .outputState = outputState,
        .tearingPolicy = tearingPolicy,
        .directScanoutPolicy = directScanoutPolicy,
        .workspaceInventory = workspaceInventory,
        .workspaceLayout = workspaceInventory
            || outputLayout
            || before.layout != after.layout
            || before.workspaceRules != after.workspaceRules
            || before.appearance.totalBorderWidth() != after.appearance.totalBorderWidth(),
        .sceneBlur = sceneBlur,
        // [colors] owns the border, backdrop, insert-hint, and shadow colors, so
        // any color edit refreshes window chrome.
        .viewChrome = before.appearance != after.appearance
            || before.colors != after.colors
            || before.windowRules != after.windowRules
            || focusDim,
        .layerEffects = sceneBlur || before.layerRules != after.layerRules,
        .animation = before.animation != after.animation,
        .input = before.input != after.input || before.hotCorners != after.hotCorners,
        .overviewPresentation = before.overview != after.overview || before.colors != after.colors,
        .internalUi = before.colors != after.colors || before.general.modKey != after.general.modKey,
    };
  }

  ConfigEffects ConfigEffects::everything() {
    return {
        .outputState = true,
        .tearingPolicy = true,
        .directScanoutPolicy = true,
        .workspaceInventory = true,
        .workspaceLayout = true,
        .sceneBlur = true,
        .viewChrome = true,
        .layerEffects = true,
        .animation = true,
        .input = true,
        .overviewPresentation = true,
        .internalUi = true,
    };
  }

  ConfigChange ConfigChange::everything() {
    return {
        .colors = true,
        .appearance = true,
        .animation = true,
        .overview = true,
        .hotCorners = true,
        .layout = true,
        .workspaces = true,
        .general = true,
        .environment = true,
        .events = true,
        .input = true,
        .keybinds = true,
        .outputs = true,
        .windowRules = true,
        .layerRules = true,
        .securityContextRules = true,
        .workspaceRules = true,
    };
  }

  ConfigChange ConfigChange::between(const Config& before, const Config& after) {
    return {
        .colors = before.colors != after.colors,
        .appearance = before.appearance != after.appearance,
        .animation = before.animation != after.animation,
        .overview = before.overview != after.overview,
        .hotCorners = before.hotCorners != after.hotCorners,
        .layout = before.layout != after.layout,
        .workspaces = before.workspaces != after.workspaces,
        .general = before.general != after.general,
        .environment = before.environment != after.environment,
        .events = before.events != after.events,
        .input = before.input != after.input,
        .keybinds = before.keybinds != after.keybinds,
        .outputs = before.outputs != after.outputs,
        .windowRules = before.windowRules != after.windowRules,
        .layerRules = before.layerRules != after.layerRules,
        .securityContextRules = before.securityContextRules != after.securityContextRules,
        .workspaceRules = before.workspaceRules != after.workspaceRules,
    };
  }

  std::string ConfigChange::summary() const {
    std::string out;
    const auto add = [&out](bool changed, std::string_view name) {
      if (!changed) {
        return;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += name;
    };
    add(colors, "colors");
    add(appearance, "appearance");
    add(animation, "animation");
    add(overview, "overview");
    add(hotCorners, "hot corners");
    add(layout, "layout");
    add(workspaces, "workspaces");
    add(general, "general");
    add(environment, "environment");
    add(events, "events");
    add(input, "input");
    add(keybinds, "keybinds");
    add(outputs, "outputs");
    add(windowRules, "window rules");
    add(layerRules, "layer rules");
    add(securityContextRules, "security context rules");
    add(workspaceRules, "workspace rules");
    return out;
  }

  std::string ConfigEffects::summary() const {
    std::string out;
    const auto add = [&out](bool changed, std::string_view name) {
      if (!changed) {
        return;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += name;
    };
    add(outputState, "output state");
    add(tearingPolicy, "tearing policy");
    add(directScanoutPolicy, "direct scanout policy");
    add(workspaceInventory, "workspace inventory");
    add(workspaceLayout, "workspace layout");
    add(sceneBlur, "scene blur");
    add(viewChrome, "view chrome");
    add(internalUi, "internal UI");
    add(animation, "animation state");
    add(layerEffects, "layer effects");
    add(input, "input");
    add(overviewPresentation, "overview presentation");
    return out;
  }
} // namespace umbriel
