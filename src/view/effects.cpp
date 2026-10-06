#include "view/effects.h"

#include "config/config.h"
#include "scene/effect_registry.h"

#include <algorithm>

extern "C" {
#include <umbrielfx/render/effect.h>
}

// clang-format off
#include "wlr.h"
// clang-format on

namespace umbriel {

  void clearWindowEffectSlots(wlr_scene_node* node) {
    if (node != nullptr) {
      wlr_scene_node_set_animation(node, FX_SLOT_WINDOW, nullptr, nullptr);
      wlr_scene_node_set_animation(node, FX_SLOT_OVERLAY, nullptr, nullptr);
    }
  }

  void ViewEffects::resetSlots() {
    m_borderSlot = {};
    m_windowSlot = {};
    m_border.clear();
    m_window.clear();
    m_overlay = false;
  }

  void ViewEffects::syncNames(const Effects& effects) {
    m_border = m_borderSlot.effectiveName();
    m_window = m_windowSlot.effectiveName();
    const EffectPreset* border = m_border.empty() ? nullptr : findEffectPreset(effects, m_border);
    m_overlay = border != nullptr && !border->overlay.empty();
  }

  int ViewEffects::borderPadding() const {
    if (m_border.empty()) {
      return 0;
    }
    const EffectRegistry& registry = effectRegistry();
    return borderPresetPadding(
        registry.presetConfig(m_border), registry.preset(m_border, EffectKind::Border) != nullptr
    );
  }

  void ViewEffects::track(const void* owner) {
    if (std::ranges::find(m_owners, owner) == m_owners.end()) {
      m_owners.push_back(owner);
    }
  }

  void ViewEffects::untrack(const void* owner) {
    if (owner == nullptr || std::erase(m_owners, owner) == 0) {
      return;
    }
    effectRegistry().removeInstance(owner);
  }

  void ViewEffects::apply(const ApplyInput& input) {
    EffectRegistry& registry = effectRegistry();
    const bool gateOpen = input.border != nullptr && !m_border.empty() && borderEffectApplies(input.gate);
    fx_effect_shader* shader = gateOpen ? registry.preset(m_border, EffectKind::Border) : nullptr;
    const EffectPreset* preset = shader != nullptr ? registry.presetConfig(m_border) : nullptr;
    const bool advancing = preset != nullptr && preset->animated && preset->speed > 0.0F;
    const float seconds = advancing ? registry.compositionSeconds(input.border, input.seconds) * preset->speed : 0.0F;
    if (preset == nullptr) {
      if (input.border != nullptr) {
        wlr_scene_node_set_animation(input.border, FX_SLOT_BORDER_EFFECT, nullptr, nullptr);
      }
      untrack(input.border);
    } else {
      fx_animation_parameters parameters{};
      parameters.scale = input.scale;
      registry.fillTimeUniforms(parameters, seconds, *preset, shader, input.output, input.border);
      if (preset->light) {
        parameters.light = {
            .enabled = true,
            .spread = static_cast<float>(preset->light->spread) * input.scale,
            .intensity = preset->light->intensity,
            .threshold = preset->light->threshold,
        };
      }
      registry.setAnimationParameters(input.border, FX_SLOT_BORDER_EFFECT, shader, parameters, preset);
      track(input.border);
      registry.updateInstance(
          input.border,
          {
              .output = input.output,
              .visible = wlr_scene_node_visible_in_box(input.border, &input.outputBox),
              .readsTime = fx_effect_shader_reads(shader, "umbriel_time"),
              .advancing = advancing && input.clockAdvancing,
          },
          registry.audioSource(*preset, shader), input.border
      );
    }
    applyWindowSlots(input, preset, seconds, advancing);
  }

  void ViewEffects::applyWindowSlots(
      const ApplyInput& input, const EffectPreset* border, float borderSeconds, bool borderAdvancing
  ) {
    if (input.surface == nullptr) {
      return;
    }
    EffectRegistry& registry = effectRegistry();
    // Window slot: the default or window_effect preset, regardless of focus. Overlay: the border preset's window
    // preset, only while the border effect applies.
    const EffectPreset* windowPreset = m_window.empty() ? nullptr : registry.presetConfig(m_window);
    fx_effect_shader* windowShader = windowPreset != nullptr ? registry.preset(m_window, EffectKind::Window) : nullptr;
    const EffectPreset* overlayPreset =
        border != nullptr && !border->overlay.empty() ? registry.presetConfig(border->overlay) : nullptr;
    fx_effect_shader* overlayShader =
        overlayPreset != nullptr ? registry.preset(border->overlay, EffectKind::Window) : nullptr;
    const auto bindSlot = [&](wlr_scene_node* node, unsigned slot, const EffectPreset* preset, fx_effect_shader* shader,
                              float seconds, const void* output, bool capture) {
      if (shader == nullptr) {
        wlr_scene_node_set_animation(node, slot, nullptr, nullptr);
        return;
      }
      fx_animation_parameters parameters{};
      parameters.scale = input.scale;
      registry.fillTimeUniforms(parameters, seconds, *preset, shader, output, capture ? nullptr : node);
      if (capture) {
        wlr_scene_node_set_animation(node, slot, shader, &parameters);
      } else {
        registry.setAnimationParameters(node, slot, shader, parameters, preset);
      }
    };
    const auto bind = [&](wlr_scene_node* node, const void* output, bool capture) {
      const float seconds = capture ? input.seconds : registry.compositionSeconds(node, input.seconds);
      bindSlot(node, FX_SLOT_WINDOW, windowPreset, windowShader, seconds, output, capture);
      bindSlot(node, FX_SLOT_OVERLAY, overlayPreset, overlayShader, borderSeconds, output, capture);
    };
    bind(input.surface, input.output, false);
    // The isolated toplevel capture shows window effects only when effects are included in captures.
    if (config().effects.inCapture) {
      if (input.captureSurface != nullptr) {
        bind(input.captureSurface, input.captureOutput, true);
      }
    } else {
      clearWindowEffectSlots(input.captureSurface);
    }
    // Two instances on the surface node, keyed by the node and by its addons member so each has its own owner: the
    // window slot follows the output's clock, the overlay advances only while the border's clock does.
    const bool visible = (windowShader != nullptr || overlayShader != nullptr)
        && wlr_scene_node_visible_in_box(input.surface, &input.outputBox);
    const auto instance = [&](const void* owner, const EffectPreset* preset, const fx_effect_shader* program,
                              bool advancing) {
      if (program == nullptr) {
        untrack(owner);
        return;
      }
      track(owner);
      registry.updateInstance(
          owner,
          {
              .output = input.output,
              .visible = visible,
              .readsTime = fx_effect_shader_reads(program, "umbriel_time"),
              .advancing = advancing && input.clockAdvancing,
          },
          preset != nullptr ? registry.audioSource(*preset, program) : std::string_view{}, input.surface
      );
    };
    instance(input.surface, windowPreset, windowShader, true);
    instance(&input.surface->addons, overlayPreset, overlayShader, borderAdvancing);
    if (input.captureSurface != nullptr) {
      const auto captureInstance = [&](const void* owner, const EffectPreset* preset, const fx_effect_shader* program) {
        const auto source = preset != nullptr ? registry.audioSource(*preset, program) : std::string_view{};
        if (source.empty() || !config().effects.inCapture || !input.captureActive) {
          untrack(owner);
          return;
        }
        track(owner);
        registry.updateCaptureAudio(owner, input.captureOutput, source, true);
      };
      captureInstance(input.captureSurface, windowPreset, windowShader);
      captureInstance(&input.captureSurface->addons, overlayPreset, overlayShader);
    }
  }

  void ViewEffects::detach() {
    EffectRegistry& registry = effectRegistry();
    for (const void* owner : m_owners) {
      registry.removeInstance(owner);
    }
    m_owners.clear();
  }

  void ViewEffects::detachNodes(wlr_scene_node* surface, wlr_scene_node* border) {
    untrack(surface);
    if (surface != nullptr) {
      untrack(&surface->addons);
    }
    untrack(border);
  }

} // namespace umbriel
