// Reveal the intact destination along the navigation axis.
vec4 transition(vec2 output_uv) {
  float p = umbriel_clamped_progress;
  if (p <= 0.0) return umbriel_sample_from(output_uv);
  if (p >= 1.0) return umbriel_sample_to(output_uv);
  vec2 axis = abs(umbriel_axis);
  if (dot(axis, axis) < 0.5) axis = vec2(1.0, 0.0);
  float along = dot(output_uv, axis);
  if (umbriel_direction < 0.0) along = 1.0 - along;
  return along < p ? umbriel_sample_to(output_uv) : umbriel_sample_from(output_uv);
}
