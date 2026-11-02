#pragma once

#include <cstdint>

namespace umbriel {

  // Work that must finish before the next frame. The frame handler rebuilds
  // stale state once, in a fixed order.
  enum class Dirty : uint32_t {
    None = 0,
    // Layer-shell surfaces set exclusive zones, which is what defines the usable
    // area. Everything below depends on the result, so this runs first.
    LayerArrange = 1U << 0,
    // At least one workspace needs arranging. Each workspace records whether
    // it animates, including hidden workspaces.
    Layout = 1U << 1,
    // Chrome that sits over the layout: the config-error banner, the desktop
    // backdrop, the keybind cheatsheet, the session-quit confirmation.
    Banner = 1U << 2,
    Backdrop = 1U << 3,
    Cheatsheet = 1U << 4,
    QuitConfirm = 1U << 5,
  };

  // Session-lock blanking stays immediate, preventing a frame from exposing
  // desktop content after an output or mode change.

  constexpr Dirty operator|(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  constexpr Dirty operator&(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  constexpr Dirty& operator|=(Dirty& a, Dirty b) { return a = a | b; }

  constexpr bool any(Dirty set) { return static_cast<uint32_t>(set) != 0; }
  constexpr bool has(Dirty set, Dirty bit) { return any(set & bit); }

} // namespace umbriel
