#ifndef UMBRIELFX_AUDIO_INPUTS_H
#define UMBRIELFX_AUDIO_INPUTS_H

#include <umbrielfx/render/effect.h>

// Shared GLSL ES 1.00 audio input profile. Constant indices also work on the
// minimum GLES2 path; unused helpers/uniforms are eliminated by the linker.
#define FX_AUDIO_INPUT_SOURCE \
  "uniform vec4 umbriel_audio_levels;\n" \
  "uniform vec4 umbriel_audio_bands[4];\n" \
  "float umbriel_audio_available() { return umbriel_audio_levels.x; }\n" \
  "float umbriel_audio_rms() { return umbriel_audio_levels.y; }\n" \
  "float umbriel_audio_peak() { return umbriel_audio_levels.z; }\n" \
  "float umbriel_audio_envelope() { return umbriel_audio_levels.w; }\n" \
  "float umbriel_audio_level() { return umbriel_audio_levels.w; }\n" \
  "float umbriel_audio_band_at(int i) {\n" \
  "  if (i <= 0) return umbriel_audio_bands[0].x;\n" \
  "  if (i == 1) return umbriel_audio_bands[0].y;\n" \
  "  if (i == 2) return umbriel_audio_bands[0].z;\n" \
  "  if (i == 3) return umbriel_audio_bands[0].w;\n" \
  "  if (i == 4) return umbriel_audio_bands[1].x;\n" \
  "  if (i == 5) return umbriel_audio_bands[1].y;\n" \
  "  if (i == 6) return umbriel_audio_bands[1].z;\n" \
  "  if (i == 7) return umbriel_audio_bands[1].w;\n" \
  "  if (i == 8) return umbriel_audio_bands[2].x;\n" \
  "  if (i == 9) return umbriel_audio_bands[2].y;\n" \
  "  if (i == 10) return umbriel_audio_bands[2].z;\n" \
  "  if (i == 11) return umbriel_audio_bands[2].w;\n" \
  "  if (i == 12) return umbriel_audio_bands[3].x;\n" \
  "  if (i == 13) return umbriel_audio_bands[3].y;\n" \
  "  if (i == 14) return umbriel_audio_bands[3].z;\n" \
  "  return umbriel_audio_bands[3].w;\n" \
  "}\n" \
  "float umbriel_audio_band(float t) {\n" \
  "  float p = clamp(t, 0.0, 1.0) * 15.0;\n" \
  "  int i = int(floor(p));\n" \
  "  return mix(umbriel_audio_band_at(i), umbriel_audio_band_at(i + 1), fract(p));\n" \
  "}\n"

// Atomic append: a nearly full uniform table cannot receive only half an input snapshot.
// The five GPU vec4s use exactly two CPU table entries.
static inline bool fx_parameters_add_audio(struct fx_animation_parameters *parameters,
    const float levels[4], const float bands[16]) {
  if (parameters->uniform_count > FX_UNIFORMS_MAX - 2) {
    return false;
  }
  struct fx_uniform *level = fx_parameters_add_uniform(parameters, "umbriel_audio_levels", FX_UNIFORM_VEC4, 1);
  struct fx_uniform *spectrum = fx_parameters_add_uniform(parameters, "umbriel_audio_bands", FX_UNIFORM_VEC4, 4);
  memcpy(level->floats, levels, 4 * sizeof(float));
  memcpy(spectrum->floats, bands, 16 * sizeof(float));
  return true;
}

#endif
