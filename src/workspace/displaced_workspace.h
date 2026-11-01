#pragma once

#include "config/config.h"
#include "output/identity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace umbriel {

  // Own the monitor identity while its Output is absent. Temporary refuge workspaces retain explicit names, while
  // anonymous ones use their current numeric position. Layout rules and returning windows retain the original home.
  struct DisplacedWorkspace {
    std::string outputName;
    std::string outputMake;
    std::string outputModel;
    std::string outputSerial;
    std::string workspaceName;
    size_t workspaceIndex = 0;
    bool workspaceNamed = false;
    uint64_t configGeneration = 0;
    std::optional<LayoutMode> layoutModeOverride;
    // Runtime inventory and layout survive output loss until relevant configuration changes.
    std::optional<ResolvedLayoutConfig> transferredLayout;

    [[nodiscard]] OutputIdentity identity() const {
      return {
          .connector = outputName,
          .make = outputMake,
          .model = outputModel,
          .serial = outputSerial,
      };
    }
  };

} // namespace umbriel
