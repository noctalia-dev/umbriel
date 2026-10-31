#include "view/effects.h"

namespace umbriel {

  bool borderEffectApplies(const BorderEffectGate& gate) {
    // The widening to unfocused windows is opt-in (gate.allowUnfocused from
    // [effects].border_unfocused): every drawn window costs a ring capture and
    // one program pass per frame, so the default keeps the focused-only draw.
    return (gate.focused || gate.allowUnfocused) && gate.decorated && !gate.urgent && !gate.fullscreen;
  }

  int borderPresetPadding(const EffectPreset* preset, bool compiled) {
    return compiled && preset != nullptr && preset->kind == EffectKind::Border ? preset->padding : 0;
  }

} // namespace umbriel
