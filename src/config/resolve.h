#pragma once

#include "config/config.h"

#include <cstddef>
#include <string_view>

namespace umbriel {

  // Validation uses the same inventory rules as runtime resolution.
  [[nodiscard]] bool workspaceRuleTargetExists(const Config& config, const WorkspaceConfig& rule);

  [[nodiscard]] ResolvedWindowRule resolveWindowRules(
      const Config& config, const char* appId, const char* title, std::string_view xdgTag, ContentType contentType,
      bool focused
  );
  [[nodiscard]] ResolvedLayerRule resolveLayerRules(const Config& config, const char* layerNamespace);
  [[nodiscard]] bool anyWindowRuleHasTitlePattern(const Config& config);
  // Return the sole fixed-output inventory containing this zero-based workspace
  // position. Null means no fixed owner or an ambiguous owner.
  [[nodiscard]] const OutputRule* uniqueFixedWorkspaceOwner(const Config& config, size_t index);
  [[nodiscard]] ResolvedLayoutConfig resolveGlobalLayout(const Config& config);
  [[nodiscard]] ResolvedLayoutConfig
  resolveWorkspaceLayout(const Config& config, const char* outputName, std::string_view name, size_t index);
  [[nodiscard]] ResolvedWorkspaceSet resolveWorkspacesForOutput(const Config& config, const char* outputName);

} // namespace umbriel
