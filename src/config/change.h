#pragma once

#include "config/config.h"

#include <string>

namespace umbriel {

  // Which source sections a reload altered. Runtime effects are derived
  // separately because one section can invalidate several subsystems.
  struct ConfigChange {
    bool colors = false;
    bool appearance = false;
    bool animation = false;
    bool overview = false;
    bool hotCorners = false;
    bool layout = false;
    bool workspaces = false;
    bool general = false;
    bool environment = false;
    bool input = false;
    bool keybinds = false;
    bool outputs = false;
    bool windowRules = false;
    bool layerRules = false;
    bool workspaceRules = false;

    [[nodiscard]] bool any() const {
      return colors
          || appearance
          || animation
          || overview
          || hotCorners
          || layout
          || workspaces
          || general
          || environment
          || input
          || keybinds
          || outputs
          || windowRules
          || layerRules
          || workspaceRules;
    }

    // Comma-separated names of the sections that changed, empty when none did.
    [[nodiscard]] std::string summary() const;

    // What a first load reports: everything is new.
    [[nodiscard]] static ConfigChange everything();
    [[nodiscard]] static ConfigChange between(const Config& before, const Config& after);
  };

  // Runtime subsystems invalidated by a successful reload. These are effects,
  // not source sections: one source change can invalidate several consumers.
  struct ConfigEffects {
    bool outputState = false;
    // Per-frame async presentation policy. This is separate from outputState
    // so a policy reload does not reapply modes, HDR, VRR, focus, or scale.
    bool tearingPolicy = false;
    bool workspaceInventory = false;
    bool workspaceLayout = false;
    bool sceneBlur = false;
    bool viewChrome = false;
    bool layerEffects = false;
    bool animation = false;
    bool input = false;
    bool overviewPresentation = false;
    bool internalUi = false;

    [[nodiscard]] bool any() const {
      return outputState
          || tearingPolicy
          || workspaceInventory
          || workspaceLayout
          || sceneBlur
          || viewChrome
          || layerEffects
          || animation
          || input
          || overviewPresentation
          || internalUi;
    }

    [[nodiscard]] bool invalidatesOverview() const {
      return outputState || workspaceInventory || workspaceLayout || viewChrome || overviewPresentation;
    }

    [[nodiscard]] std::string summary() const;

    [[nodiscard]] static ConfigEffects everything();
    [[nodiscard]] static ConfigEffects between(const Config& before, const Config& after);
  };

  struct ConfigReloadResult {
    bool success = false;
    ConfigChange change;
    ConfigEffects effects;
  };

} // namespace umbriel
