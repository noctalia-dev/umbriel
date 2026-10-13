#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex;
uniform float gain;
// wlr_color_transfer_function of SDR values in a linear target (and a linear
// source), or 0 when the values are already encoded
#define TRANSFER_FUNCTION_SRGB 1
#define TRANSFER_FUNCTION_GAMMA22 8
uniform int linear;
uniform bool emission;
uniform int source_linear;
uniform float threshold;
uniform vec4 source_region;

// Emission thresholds the border result into level 0; blend maps the blurred
// level to a screen-blend contribution.
void main() {
  if (emission) {
    vec2 uv = (v_texcoord - source_region.xy) / source_region.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
      gl_FragColor = vec4(0.0);
      return;
    }
    vec4 value = texture2D(tex, uv);
    if (source_linear != 0 && value.a > 0.0) {
      vec3 rgb = max(value.rgb / value.a, 0.0);
      if (source_linear == TRANSFER_FUNCTION_GAMMA22) {
        rgb = pow(rgb, vec3(1.0 / 2.2));
      } else {
        rgb = mix(rgb * 12.92, 1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), rgb));
      }
      value.rgb = rgb * value.a;
    }
    float peak = max(value.r, max(value.g, value.b));
    gl_FragColor = value * smoothstep(threshold, max(threshold + 0.001, 1.0), peak);
    return;
  }
  vec3 light = 1.0 - exp(-max(texture2D(tex, v_texcoord).rgb, 0.0) * gain);
  if (linear == TRANSFER_FUNCTION_GAMMA22) {
    light = pow(light, vec3(2.2));
  } else if (linear == TRANSFER_FUNCTION_SRGB) {
    light = mix(light / 12.92, pow((light + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), light));
  }
  gl_FragColor = vec4(light, max(light.r, max(light.g, light.b)));
}
