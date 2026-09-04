#pragma once

#include "config/config.h"
#include "output/identity.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace umbriel {

  // Validation uses the same inventory rules as runtime resolution.
  [[nodiscard]] bool workspaceRuleTargetExists(const Config& config, const WorkspaceConfig& rule);

  // Client identity a compositor may never have received. An unset value matches no pattern; a set but empty one
  // matches a pattern that accepts the empty string, such as `^$`.
  [[nodiscard]] inline std::optional<std::string_view> ruleText(const char* value) {
    return value != nullptr ? std::optional<std::string_view>(value) : std::nullopt;
  }

  inline constexpr uint64_t kStartupWindowRuleDurationMs = 60'000;

  [[nodiscard]] ResolvedWindowRule resolveWindowRules(
      const Config& config, std::optional<std::string_view> appId, std::optional<std::string_view> title,
      std::optional<std::string_view> xdgTag, ContentType contentType, bool focused, uint64_t uptimeMs
  );
  [[nodiscard]] ResolvedLayerRule
  resolveLayerRules(const Config& config, std::optional<std::string_view> layerNamespace);
  // The globals every [[security_context_rule]] matching the client's metadata
  // grants on top of the base allowed set.
  [[nodiscard]] std::vector<std::string>
  securityContextRuleGlobals(const Config& config, const char* sandboxEngine, const char* appId);
  [[nodiscard]] bool anyWindowRuleHasTitlePattern(const Config& config);
  // Return the sole fixed-output inventory containing this zero-based workspace
  // position. Null means no fixed owner or an ambiguous owner.
  [[nodiscard]] const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, size_t index);
  // Descriptor-specific output sections override connector fallbacks.
  [[nodiscard]] const OutputRule* findOutputRule(const Config& config, const OutputIdentity& identity);
  [[nodiscard]] ResolvedLayoutConfig resolveGlobalLayout(const Config& config);
  [[nodiscard]] ResolvedLayoutConfig
  resolveWorkspaceLayout(const Config& config, const OutputIdentity& identity, std::string_view name, size_t index);
  [[nodiscard]] ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const OutputIdentity& identity);
  // Workspace count a dynamic output never shrinks below.
  [[nodiscard]] size_t resolveDynamicWorkspaceMinimum(const Config& config, const OutputIdentity& identity);

} // namespace umbriel
