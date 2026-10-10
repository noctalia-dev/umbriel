uniform vec2 umbriel_workspace_axis;

vec4 animation(vec2 uv) {
  float coordinate = dot(uv, abs(umbriel_workspace_axis));
  if (umbriel_workspace_axis.x + umbriel_workspace_axis.y < 0.0)
    coordinate = 1.0 - coordinate;
  float visible = step(coordinate, umbriel_clamped_progress);
  return mix(umbriel_sample(uv), umbriel_sample_incoming(uv), visible);
}
