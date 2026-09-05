#include "scene/animation_shader.h"

#include "config/config.h"

#include <array>
#include <memory>

extern "C" {
#include <umbrielfx/render/animation.h>
}

namespace umbriel {
  namespace {
    struct CacheEntry {
      wlr_renderer* renderer = nullptr;
      std::optional<AnimationShaderSource> source;
      std::shared_ptr<fx_animation_shader> shader;
    };
    std::array<CacheEntry, FX_ANIMATION_SLOTS> cache;
    static_assert(static_cast<unsigned>(AnimationEvent::Overview) + 1 == FX_ANIMATION_SLOTS);

    template <typename Value>
    void update(
        wlr_scene_node* node, wlr_renderer* renderer, AnimationEvent event, const Value& value, float progress,
        float direction
    ) {
      if (node == nullptr) {
        return;
      }
      const fx_animation_parameters parameters{
          .progress = progress, .linear_progress = static_cast<float>(value.progress()), .direction = direction
      };
      wlr_scene_node_set_animation(
          node, static_cast<unsigned>(event), value.animating() ? animationShader(renderer, event) : nullptr,
          &parameters
      );
    }
  } // namespace

  fx_animation_shader* animationShader(wlr_renderer* renderer, AnimationEvent event) {
    const auto& settings = config().animation;
    const std::optional<AnimationShaderSource>* source = nullptr;
    bool enabled = false;
    const char* label = "animation";
#define EVENT(id, field, name)                                                                                         \
  case AnimationEvent::id:                                                                                             \
    source = &settings.field.shader;                                                                                   \
    enabled = settings.field.enabled;                                                                                  \
    label = "animation." name;                                                                                         \
    break
    switch (event) {
      EVENT(DimUnfocused, dimUnfocused, "dim_unfocused");
      EVENT(Border, border, "border");
      EVENT(WindowsMove, windowsMove, "windows_move");
      EVENT(WindowsIn, windowsIn, "windows_in");
      EVENT(WindowsOut, windowsOut, "windows_out");
      EVENT(Scratchpad, scratchpad, "scratchpad");
      EVENT(Layers, layers, "layers");
      EVENT(Workspaces, workspaces, "workspaces");
      EVENT(Overview, overview, "overview");
    }
#undef EVENT
    auto& entry = cache[static_cast<unsigned>(event)];
    if (!settings.enabled || !enabled || source == nullptr || !*source) {
      entry = {};
      return nullptr;
    }
    if (entry.renderer != renderer || entry.source != *source) {
      entry.renderer = renderer;
      entry.source = *source;
      const auto& input = **source;
      entry.shader = {
          fx_animation_shader_create(renderer, input.code.c_str(), input.file.empty() ? label : input.file.c_str()),
          fx_animation_shader_unref
      };
    }
    return entry.shader.get();
  }

  void prepareAnimationShaders(wlr_renderer* renderer) {
    for (unsigned event = 0; event < FX_ANIMATION_SLOTS; ++event) {
      (void)animationShader(renderer, static_cast<AnimationEvent>(event));
    }
  }

  void clearAnimationShaderCache() { cache = {}; }

  void updateAnimationShader(
      wlr_scene_node* node, wlr_renderer* renderer, AnimationEvent event, const AnimatedValue& value, float direction
  ) {
    const double distance = value.target() - value.from();
    const float progress = static_cast<float>(
        distance != 0.0 ? (value.current() - value.from()) / distance : evaluateCurve(value.curve(), value.progress())
    );
    update(node, renderer, event, value, progress, direction != 0.0F ? direction : (distance < 0.0 ? -1.0F : 1.0F));
  }

  void updateAnimationShader(
      wlr_scene_node* node, wlr_renderer* renderer, AnimationEvent event, const AnimatedColor& value, float direction
  ) {
    update(node, renderer, event, value, static_cast<float>(evaluateCurve(value.curve(), value.progress())), direction);
  }
} // namespace umbriel
