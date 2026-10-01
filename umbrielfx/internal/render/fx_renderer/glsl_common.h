#ifndef FX_GLSL_COMMON_H
#define FX_GLSL_COMMON_H

#define FX_GLSL_TRIG \
"#define sin(x) sin(mod((x), 6.283185307179586))\n" \
"#define cos(x) cos(mod((x), 6.283185307179586))\n"

#define FX_GLSL_PALETTE \
"vec4 umbriel_palette_at(float t) {\n" \
"  if (umbriel_palette_count <= 0) return vec4(0.0);\n" \
"  float span = float(umbriel_palette_count);\n" \
"  float scaled = fract(t) * span;\n" \
"  float index = floor(scaled);\n" \
"  float next = mod(index + 1.0, span);\n" \
"  vec4 from = umbriel_palette[0];\n" \
"  vec4 to = umbriel_palette[0];\n" \
"  for (int i = 0; i < 4; i++) {\n" \
"    if (i >= umbriel_palette_count) break;\n" \
"    if (float(i) == index) from = umbriel_palette[i];\n" \
"    if (float(i) == next) to = umbriel_palette[i];\n" \
"  }\n" \
"  return mix(from, to, scaled - index);\n" \
"}\n"

#define FX_GLSL_ANIMATION \
"uniform float umbriel_progress;\n" \
"uniform float umbriel_linear_progress;\n" \
"uniform float umbriel_direction;\n" \
"uniform vec4 umbriel_random_seed;\n" \
"#define umbriel_clamped_progress clamp(umbriel_progress, 0.0, 1.0)\n"

#endif
