// PQ exponentiation needs full fragment precision for smooth HDR output.
precision highp float;

varying vec2 v_texcoord;

uniform sampler2D tex;
uniform bool copy_encoded_sdr;
uniform mat3 color_matrix;
uniform int inverse_eotf;
uniform sampler2D lut;
uniform float lut_dim;
uniform int has_lut;

#define TF_SRGB 1
#define TF_PQ 2
#define TF_EXT_LINEAR 4
#define TF_GAMMA22 8
#define TF_BT1886 16

float linear_channel_to_srgb(float x) { return max(min(x * 12.92, 0.04045), 1.055 * pow(x, 1.0 / 2.4) - 0.055); }

vec3 linear_color_to_srgb(vec3 color) {
  return vec3(linear_channel_to_srgb(color.r), linear_channel_to_srgb(color.g), linear_channel_to_srgb(color.b));
}

vec3 linear_color_to_pq(vec3 color) {
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  const float m = 78.84375;
  const float n = 0.1593017578125;
  vec3 pow_n = pow(clamp(color, vec3(0.0), vec3(1.0)), vec3(n));
  return pow((vec3(c1) + c2 * pow_n) / (vec3(1.0) + c3 * pow_n), vec3(m));
}

vec3 linear_color_to_bt1886(vec3 color) {
  const float lmin = 0.01;
  const float lmax = 100.0;
  float lb = pow(lmin, 1.0 / 2.4);
  float lw = pow(lmax, 1.0 / 2.4);
  float a = pow(lw - lb, 2.4);
  float b = lb / (lw - lb);
  vec3 l = color * (lmax - lmin) + vec3(lmin);
  return pow(l / a, vec3(1.0 / 2.4)) - vec3(b);
}

float unpack_lut(vec2 encoded) {
  vec2 bytes = floor(encoded * 255.0 + 0.5);
  return (bytes.x * 256.0 + bytes.y) / 65535.0;
}

float sample_lut(float value, float row) {
  float position = clamp(value, 0.0, 1.0) * (lut_dim - 1.0);
  float lower = floor(position);
  float upper = min(lower + 1.0, lut_dim - 1.0);
  float y = (row + 0.5) / 3.0;
  float a = unpack_lut(texture2D(lut, vec2((lower + 0.5) / lut_dim, y)).ra);
  float b = unpack_lut(texture2D(lut, vec2((upper + 0.5) / lut_dim, y)).ra);
  return mix(a, b, position - lower);
}

void main() {
  vec4 color = texture2D(tex, v_texcoord);

  // Unconverting would zero RGB where alpha is zero, but a direct frame keeps it.
  if (copy_encoded_sdr) {
    gl_FragColor = color;
    return;
  }

  float alpha = color.a;
  vec3 rgb = alpha == 0.0 ? vec3(0.0) : color.rgb / alpha;

  rgb = color_matrix * rgb;
  if (inverse_eotf != TF_EXT_LINEAR || has_lut != 0) {
    rgb = max(rgb, vec3(0.0));
  }
  if (inverse_eotf == TF_SRGB) {
    rgb = linear_color_to_srgb(rgb);
  } else if (inverse_eotf == TF_PQ) {
    rgb = linear_color_to_pq(rgb);
  } else if (inverse_eotf == TF_GAMMA22) {
    rgb = pow(rgb, vec3(1.0 / 2.2));
  } else if (inverse_eotf == TF_BT1886) {
    rgb = linear_color_to_bt1886(rgb);
  }
  if (has_lut != 0) {
    rgb = vec3(sample_lut(rgb.r, 0.0), sample_lut(rgb.g, 1.0), sample_lut(rgb.b, 2.0));
  }

  gl_FragColor = vec4(rgb * alpha, alpha);
}
