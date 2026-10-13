#define EFFECTS % d
#define COMPOSITE % d
#define CORNERS % d

#if !defined(EFFECTS) || !defined(COMPOSITE) || !defined(CORNERS)
#error "Missing shader preamble"
#endif

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying highp vec2 v_texcoord;
uniform sampler2D tex;

uniform float radius;
uniform vec2 halfpixel;
uniform vec4 sample_bounds;

#if EFFECTS
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float noise;
// wlr_color_transfer_function of SDR values in a linear work buffer, or 0 when
// it is already encoded
#define TRANSFER_FUNCTION_SRGB 1
#define TRANSFER_FUNCTION_GAMMA22 8
uniform int linear;

float srgb_channel_to_linear(float x) { return x > 0.04045 ? pow((x + 0.055) / 1.055, 2.4) : x / 12.92; }

vec3 srgb_color_to_linear(vec3 color) {
  return vec3(srgb_channel_to_linear(color.r), srgb_channel_to_linear(color.g), srgb_channel_to_linear(color.b));
}

float linear_channel_to_srgb(float x) { return x > 0.0031308 ? 1.055 * pow(x, 1.0 / 2.4) - 0.055 : 12.92 * x; }

vec3 linear_color_to_srgb(vec3 color) {
  return vec3(linear_channel_to_srgb(color.r), linear_channel_to_srgb(color.g), linear_channel_to_srgb(color.b));
}

mat4 brightnessMatrix() {
  float b = brightness - 1.0;
  // clang-format off
  return mat4(1, 0, 0, 0,
              0, 1, 0, 0,
              0, 0, 1, 0,
              b, b, b, 1);
  // clang-format on
}

mat4 contrastMatrix() {
  float t = (1.0 - contrast) / 2.0;
  // clang-format off
  return mat4(contrast, 0, 0, 0,
              0, contrast, 0, 0,
              0, 0, contrast, 0,
              t, t, t, 1);
  // clang-format on
}

mat4 saturationMatrix() {
  vec3 luminance = vec3(0.3086, 0.6094, 0.0820) * (1.0 - saturation);
  vec3 red = vec3(luminance.x);
  red.x += saturation;
  vec3 green = vec3(luminance.y);
  green.y += saturation;
  vec3 blue = vec3(luminance.z);
  blue.z += saturation;
  // clang-format off
  return mat4(red, 0,
              green, 0,
              blue, 0,
              0, 0, 0, 1);
  // clang-format on
}

float noiseAmount(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
  p3 += dot(p3, p3.yzx + 33.33);
  float hash = fract((p3.x + p3.y) * p3.z);
  return (mod(hash, 1.0) - 0.5) * noise;
}
#endif

#if COMPOSITE
// The final upsample draws straight into the pass target, in place of storing
// the full-resolution level and drawing that as a texture. An 8-bit level
// would clamp to [0, 1] before alpha and corners scale it, so `clamp_level`
// reproduces that; a half-float level keeps the range.
uniform float alpha;
uniform int clamp_level;

#if CORNERS
uniform vec2 size;
uniform vec2 position;
uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
);
#endif
#endif

vec4 sample_texel(vec2 uv) { return texture2D(tex, clamp(uv, sample_bounds.xy, sample_bounds.zw)); }

void main() {
  vec2 uv = v_texcoord / 2.0;

  vec4 sum = sample_texel(uv + vec2(-halfpixel.x * 2.0, 0.0) * radius);

  sum += sample_texel(uv + vec2(-halfpixel.x, halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(0.0, halfpixel.y * 2.0) * radius);
  sum += sample_texel(uv + vec2(halfpixel.x, halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(halfpixel.x * 2.0, 0.0) * radius);
  sum += sample_texel(uv + vec2(halfpixel.x, -halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(0.0, -halfpixel.y * 2.0) * radius);
  sum += sample_texel(uv + vec2(-halfpixel.x, -halfpixel.y) * radius) * 2.0;

  vec4 color = sum / 12.0;

#if EFFECTS
  float color_alpha = color.a;
  // Blur controls are defined for gamma-encoded content. Convert FP16 linear
  // work-buffer samples so HDR rendering preserves the SDR blur appearance.
  if (linear == TRANSFER_FUNCTION_GAMMA22 && color_alpha != 0.0) {
    color.rgb = pow(max(color.rgb / color_alpha, vec3(0.0)), vec3(1.0 / 2.2)) * color_alpha;
  } else if (linear == TRANSFER_FUNCTION_SRGB && color_alpha != 0.0) {
    color.rgb = linear_color_to_srgb(max(color.rgb / color_alpha, vec3(0.0))) * color_alpha;
  }
  // Do *not* transpose the combined matrix when multiplying
  color = brightnessMatrix() * contrastMatrix() * saturationMatrix() * color;
  color.xyz += noiseAmount(v_texcoord);
  if (linear == TRANSFER_FUNCTION_GAMMA22 && color_alpha != 0.0) {
    color.rgb = pow(max(color.rgb / color_alpha, vec3(0.0)), vec3(2.2)) * color_alpha;
  } else if (linear == TRANSFER_FUNCTION_SRGB && color_alpha != 0.0) {
    color.rgb = srgb_color_to_linear(color.rgb / color_alpha) * color_alpha;
  }
#endif

#if COMPOSITE
  if (clamp_level != 0) {
    color = clamp(color, 0.0, 1.0);
  }
#if CORNERS
  float quad_corner_alpha = corner_alpha(
      size - 0.5, position + 0.25, false, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );
  float clip_corner_alpha = corner_alpha(
      clip_size - 1.0, clip_position + 0.5, true, clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left,
      clip_radius_bottom_right
  );
  gl_FragColor = color * alpha * quad_corner_alpha * clip_corner_alpha;
#else
  gl_FragColor = color * alpha;
#endif
#else
  gl_FragColor = color;
#endif
}
