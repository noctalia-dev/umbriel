#include "view/effects.h"

namespace umbriel {

  bool borderEffectApplies(const BorderEffectGate& gate) {
    return gate.focused && gate.decorated && !gate.urgent && !gate.fullscreen;
  }

  int borderPresetPadding(const EffectPreset* preset, bool compiled) {
    return compiled && preset != nullptr && preset->kind == EffectKind::Border ? preset->padding : 0;
  }

} // namespace umbriel
