// Use for window/layer opening and closing, or scratchpad show/hide.
// The compositor supplies main(), precision, uniforms, and umbriel_sample().
vec4 animation(vec2 uv) {
    float visible = umbriel_direction > 0.0
        ? umbriel_clamped_progress : 1.0 - umbriel_clamped_progress;
    float edge = mix(-0.02, 1.02, visible);
    float mask = 1.0 - smoothstep(edge - 0.02, edge + 0.02, uv.x);
    return umbriel_sample(uv) * mask;
}
