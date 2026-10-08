uniform vec4 umbriel_workspace_rect;
uniform vec2 umbriel_workspace_axis;

vec4 animation(vec2 uv) {
    vec2 output_uv = umbriel_workspace_rect.xy + uv * umbriel_workspace_rect.zw;
    float coordinate = dot(output_uv, abs(umbriel_workspace_axis));
    if (umbriel_workspace_axis.x + umbriel_workspace_axis.y < 0.0)
        coordinate = 1.0 - coordinate;
    float visible = step(coordinate, umbriel_clamped_progress);
    if (umbriel_direction < 0.0)
        visible = 1.0 - visible;
    return umbriel_sample(uv) * visible;
}
