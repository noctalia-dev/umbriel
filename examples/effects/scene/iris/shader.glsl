// The destination opens from the center; its coordinates remain unchanged.
vec4 transition(vec2 uv) {
    float p = umbriel_clamped_progress;
    if (p <= 0.0) return umbriel_sample_from(uv);
    if (p >= 1.0) return umbriel_sample_to(uv);
    vec2 centered = uv - vec2(0.5);
    centered.x *= umbriel_output_size.x / umbriel_output_size.y;
    float corner = length(vec2(umbriel_output_size.x / umbriel_output_size.y, 1.0)) * 0.5;
    float radius = p * corner;
    return length(centered) < radius ? umbriel_sample_to(uv) : umbriel_sample_from(uv);
}
