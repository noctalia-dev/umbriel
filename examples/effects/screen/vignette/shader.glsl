// Darken the output towards its corners.
vec4 screen(vec2 uv) {
    vec4 c = umbriel_sample(uv);
    vec2 p = uv - 0.5;
    float falloff = smoothstep(0.35, 0.9, length(p) * 1.2);
    return vec4(c.rgb * (1.0 - 0.6 * falloff), c.a);
}
