#extension GL_OES_standard_derivatives : enable

precision highp float;

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float inner_radius_top_left;
uniform float inner_radius_top_right;
uniform float inner_radius_bottom_left;
uniform float inner_radius_bottom_right;
uniform float seam_radius_top_left;
uniform float seam_radius_top_right;
uniform float seam_radius_bottom_left;
uniform float seam_radius_bottom_right;
uniform float outer_radius_top_left;
uniform float outer_radius_top_right;
uniform float outer_radius_bottom_left;
uniform float outer_radius_bottom_right;

uniform float inner_width;
uniform float outer_width;
uniform vec4 inner_color;

float rounded_rect_distance(vec2 size, vec2 position,
		float radius_tl, float radius_tr, float radius_bl, float radius_br);

float antialias_width(float distance) {
	vec2 gradient = vec2(dFdx(distance), dFdy(distance));
	return max(length(gradient), 1.0) * 0.5;
}

float contour_distance(float outset,
		float radius_tl, float radius_tr, float radius_bl, float radius_br) {
	vec2 expansion = vec2(outset);
	return rounded_rect_distance(
		clip_size + expansion * 2.0,
		clip_position - expansion,
		radius_tl,
		radius_tr,
		radius_bl,
		radius_br
	);
}

void main() {
	float total_width = inner_width + outer_width;
	float inner_distance = contour_distance(0.0,
		inner_radius_top_left, inner_radius_top_right,
		inner_radius_bottom_left, inner_radius_bottom_right);
	float seam_distance = contour_distance(inner_width,
		seam_radius_top_left, seam_radius_top_right,
		seam_radius_bottom_left, seam_radius_bottom_right);
	float outer_distance = contour_distance(total_width,
		outer_radius_top_left, outer_radius_top_right,
		outer_radius_bottom_left, outer_radius_bottom_right);

	float inner_antialias = antialias_width(inner_distance);
	float seam_antialias = antialias_width(seam_distance);
	float outer_antialias = antialias_width(outer_distance);
	float inner_coverage = smoothstep(
		-inner_antialias, inner_antialias, inner_distance);
	float outer_coverage = 1.0 - smoothstep(
		-outer_antialias, outer_antialias, outer_distance);

	float outer_mix = 0.0;
	if (outer_width > 0.0) {
		outer_mix = inner_width > 0.0
			? smoothstep(-seam_antialias, seam_antialias, seam_distance)
			: 1.0;
	}
	vec4 color = mix(inner_color, v_color, outer_mix);
	gl_FragColor = color * min(inner_coverage, outer_coverage);
}
