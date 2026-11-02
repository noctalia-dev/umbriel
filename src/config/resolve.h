#pragma once

#include "config/config.h"
#include "output/identity.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace umbriel {

  // Validation uses the same inventory rules as runtime resolution.
  bool workspaceRuleTargetExists(const Config& config, const WorkspaceConfig& rule);

  // Client identity a compositor may never have received. An unset value matches no pattern; a set but empty one
  // matches a pattern that accepts the empty string, such as `^$`.
  inline std::optional<std::string_view> ruleText(const char* value) {
    return value != nullptr ? std::optional<std::string_view>(value) : std::nullopt;
  }

  inline constexpr uint64_t kStartupRuleDurationMs = 60'000;

  ResolvedWindowRule resolveWindowRules(
      const Config& config, std::optional<std::string_view> appId, std::optional<std::string_view> title,
      std::optional<std::string_view> xdgTag, ContentType contentType, const WindowRuleState& state, uint64_t uptimeMs
  );
  ResolvedLayerRule resolveLayerRules(
      const Config& config, std::optional<std::string_view> layerNamespace, LayerShellLayer layer, uint64_t uptimeMs
  );
  // The globals every [[security_context_rule]] matching the client's metadata
  // grants on top of the base allowed set.
  std::vector<std::string>
  securityContextRuleGlobals(const Config& config, const char* sandboxEngine, const char* appId);
  bool anyWindowRuleHasTitlePattern(const Config& config);
  // Return the sole fixed-output inventory containing this zero-based workspace
  // position. Null means no fixed owner or an ambiguous owner.
  const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, size_t index);
  // Return the sole fixed-output inventory containing this exact workspace
  // name. Null means no fixed owner or an ambiguous owner.
  const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, std::string_view name);
  // Descriptor-specific output sections override connector fallbacks.
  const OutputRule* findOutputRule(const Config& config, const OutputIdentity& identity);
  // Direction the output arranges its workspaces along; vertical when no rule matches.
  WorkspaceAxis resolveWorkspaceAxis(const Config& config, const OutputIdentity& identity);
  ResolvedLayoutConfig resolveGlobalLayout(const Config& config);
  ResolvedLayoutConfig
  resolveWorkspaceLayout(const Config& config, const OutputIdentity& identity, std::string_view name, size_t index);
  // Resolve an unnamed dynamic workspace. Its numeric protocol label is not an
  // explicit name selector, so only index rules match it.
  ResolvedLayoutConfig
  resolveUnnamedWorkspaceLayout(const Config& config, const OutputIdentity& identity, size_t index);
  ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const OutputIdentity& identity);
  // Workspace count a dynamic output never shrinks below.
  size_t resolveDynamicWorkspaceMinimum(const Config& config, const OutputIdentity& identity);

} // namespace umbriel
