#define SOURCE %d
#define EFFECTS %d
#define SAMPLE_CLAMP %d
#define FORCE_HIGH_PRECISION %d

#define SOURCE_TEXTURE_RGBA 1
#define SOURCE_TEXTURE_RGBX 2
#define SOURCE_TEXTURE_EXTERNAL 3

#if !defined(SOURCE) || !defined(EFFECTS) || !defined(SAMPLE_CLAMP) || !defined(FORCE_HIGH_PRECISION)
#error "Missing shader preamble"
#endif

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

#if FORCE_HIGH_PRECISION || defined(GL_FRAGMENT_PRECISION_HIGH)
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;

#if SOURCE == SOURCE_TEXTURE_EXTERNAL
uniform samplerExternalOES tex;
#elif SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_RGBX
uniform sampler2D tex;
#endif

uniform float alpha;
uniform int source_tf;
uniform mat3 primaries_matrix;
uniform float lum_multiplier;
uniform int target_tf;

#define TRANSFER_FUNCTION_PASSTHROUGH 0
#define TRANSFER_FUNCTION_SRGB 1
#define TRANSFER_FUNCTION_ST2084_PQ 2
#define TRANSFER_FUNCTION_EXT_LINEAR 4
#define TRANSFER_FUNCTION_GAMMA22 8
#define TRANSFER_FUNCTION_BT1886 16

#if EFFECTS
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
#endif

uniform float discard_transparent;

#if SAMPLE_CLAMP
// Normalized texel range the fragment may sample: xy is the first sampled texel
// center, zw the last. A cropped surface whose crop edge falls between texels
// snaps its source box to whole texels for sharpness, which can leave the box
// reaching one texel past the crop. Clamping here duplicates the last texel
// inside the crop instead of pulling in whatever the client left outside it,
// which for a client-side-decorated window is its transparent shadow margin.
// The clamp is compiled only into the variants selected for such crops, so
// every other draw keeps the exact fetch drivers already handle well.
uniform vec4 sample_bounds;
#define SAMPLE_UV clamp(v_texcoord, sample_bounds.xy, sample_bounds.zw)
#else
#define SAMPLE_UV v_texcoord
#endif

vec4 sample_texture() {
#if SOURCE == SOURCE_TEXTURE_RGBA || SOURCE == SOURCE_TEXTURE_EXTERNAL
	return texture2D(tex, SAMPLE_UV);
#elif SOURCE == SOURCE_TEXTURE_RGBX
	return vec4(texture2D(tex, SAMPLE_UV).rgb, 1.0);
#endif
}

float srgb_channel_to_linear(float x) {
	return x > 0.04045
		? pow((x + 0.055) / 1.055, 2.4)
		: x / 12.92;
}

vec3 srgb_color_to_linear(vec3 color) {
	return vec3(
		srgb_channel_to_linear(color.r),
		srgb_channel_to_linear(color.g),
		srgb_channel_to_linear(color.b)
	);
}

vec3 pq_color_to_linear(vec3 color) {
	float inv_m1 = 1.0 / 0.1593017578125;
	float inv_m2 = 1.0 / 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	vec3 power = pow(color, vec3(inv_m2));
	vec3 num = max(power - c1, 0.0);
	vec3 denom = c2 - c3 * power;
	return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
	float l_min = 0.01;
	float l_max = 100.0;
	float lb = pow(l_min, 1.0 / 2.4);
	float lw = pow(l_max, 1.0 / 2.4);
	float a = pow(lw - lb, 2.4);
	float b = lb / (lw - lb);
	vec3 luminance = a * pow(color + vec3(b), vec3(2.4));
	return (luminance - l_min) / (l_max - l_min);
}

float linear_channel_to_srgb(float x) {
	return x > 0.0031308
		? 1.055 * pow(x, 1.0 / 2.4) - 0.055
		: 12.92 * x;
}

vec3 linear_color_to_srgb(vec3 color) {
	return vec3(
		linear_channel_to_srgb(color.r),
		linear_channel_to_srgb(color.g),
		linear_channel_to_srgb(color.b)
	);
}

vec3 linear_color_to_gamma22(vec3 color) {
	return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
}

vec4 convert_color(vec4 color) {
	if (source_tf == TRANSFER_FUNCTION_PASSTHROUGH) {
		return color;
	}

	float color_alpha = color.a;
	vec3 rgb = color_alpha == 0.0 ? vec3(0.0) : color.rgb / color_alpha;
	rgb = max(rgb, vec3(0.0));

	if (source_tf == TRANSFER_FUNCTION_SRGB) {
		rgb = srgb_color_to_linear(rgb);
	} else if (source_tf == TRANSFER_FUNCTION_ST2084_PQ) {
		rgb = pq_color_to_linear(rgb);
	} else if (source_tf == TRANSFER_FUNCTION_GAMMA22) {
		rgb = pow(rgb, vec3(2.2));
	} else if (source_tf == TRANSFER_FUNCTION_BT1886) {
		rgb = bt1886_color_to_linear(rgb);
	}

	rgb *= lum_multiplier;
	rgb = primaries_matrix * rgb;
	if (target_tf == TRANSFER_FUNCTION_SRGB) {
		rgb = linear_color_to_srgb(max(rgb, vec3(0.0)));
	} else if (target_tf == TRANSFER_FUNCTION_GAMMA22) {
		rgb = linear_color_to_gamma22(rgb);
	}

	return vec4(rgb * color_alpha, color_alpha);
}

#if EFFECTS
float corner_alpha(vec2 size, vec2 position, bool is_cutout,
		float radius_tl, float radius_tr, float radius_bl, float radius_br);
#endif

void main() {
	vec4 color = convert_color(sample_texture());
#if EFFECTS
	float quad_corner_alpha = corner_alpha(
		size - 0.5,
		position + 0.25,
		false,
		radius_top_left,
		radius_top_right,
		radius_bottom_left,
		radius_bottom_right
	);

	// Clipping
	float clip_corner_alpha = corner_alpha(
		clip_size - 1.0,
		clip_position + 0.5,
		true,
		clip_radius_top_left,
		clip_radius_top_right,
		clip_radius_bottom_left,
		clip_radius_bottom_right
	);

	gl_FragColor = color * alpha * quad_corner_alpha * clip_corner_alpha;
#else
	gl_FragColor = color * alpha;
#endif

	if (gl_FragColor.a < discard_transparent) {
		discard;
	}
}
