#include "view/effects.h"

namespace umbriel {

  namespace {
    // The most specific selector replaces the default by name; "off" and "" both disable it.
    std::string pick(const std::string& fallback, const std::optional<std::string>& override) {
      if (!override) {
        return fallback;
      }
      return *override == kEffectOff ? std::string() : *override;
    }
  } // namespace

  ViewEffectNames resolveViewEffectNames(const Effects& effects, const ResolvedWindowRule& rule) {
    return {.border = pick(effects.border, rule.borderEffect), .window = pick(effects.window, rule.windowEffect)};
  }

  std::string resolveScreenEffectName(const Effects& effects, const OutputRule* rule) {
    return rule != nullptr ? pick(effects.screen, rule->screenEffect) : effects.screen;
  }

  bool borderEffectApplies(const BorderEffectGate& gate) {
    return gate.focused && gate.decorated && !gate.urgent && !gate.fullscreen;
  }

  int borderPresetPadding(const EffectPreset* preset, bool compiled) {
    return compiled && preset != nullptr && preset->kind == EffectKind::Border ? preset->padding : 0;
  }

} // namespace umbriel
