#pragma once

#include "core/animation.h"

struct wlr_scene_node;
struct wlr_renderer;
struct fx_animation_shader;

namespace umbriel {
  // Stable inner-to-outer composition order for effects sharing a target.
  enum class AnimationEvent : unsigned {
    DimUnfocused,
    Border,
    WindowsMove,
    WindowsIn,
    WindowsOut,
    Scratchpad,
    Layers,
    Workspaces,
    Overview
  };

  // The configured custom shader for `event`, or null.
  [[nodiscard]] fx_animation_shader* animationShader(wlr_renderer* renderer, AnimationEvent event);
  // The program a lifecycle fade composes through: the custom shader, or for windows_in and windows_out without one, a
  // built-in fade that applies the lifecycle alpha to the whole window at once. Null when buffers fade individually.
  [[nodiscard]] fx_animation_shader* lifecycleShader(wlr_renderer* renderer, AnimationEvent event);
  void prepareAnimationShaders(wlr_renderer* renderer);
  void clearAnimationShaderCache();
  void updateAnimationShader(
      wlr_scene_node* node, wlr_renderer* renderer, AnimationEvent event, const AnimatedValue& value,
      float direction = 0.0F
  );
  void updateAnimationShader(
      wlr_scene_node* node, wlr_renderer* renderer, AnimationEvent event, const AnimatedColor& value, float direction
  );
} // namespace umbriel
