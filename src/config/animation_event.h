#pragma once

namespace umbriel {
  // Stable inner-to-outer composition order for effects sharing a target. Values equal the FX_SLOT_* indices.
  enum class AnimationEvent : unsigned {
    Window,
    Overlay,
    BorderEffect,
    Border,
    DimUnfocused,
    WindowsMove,
    Drag,
    WindowsIn,
    WindowsOut,
    Scratchpad,
    Layers,
    Workspaces,
    Overview
  };
} // namespace umbriel
