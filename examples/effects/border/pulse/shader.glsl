// A soft pulse traveling around the focused window's ring, tinted by the first
// palette color (accent_primary) and fading into the padding.
vec4 border(vec2 uv) {
    float d = max(umbriel_border_distance(uv), 0.0);
    float ring = umbriel_sample(uv).a;
    float angle = atan(uv.y - 0.5, uv.x - 0.5);
    float wave = 0.5 + 0.5 * sin(angle * 3.0 - umbriel_time * 2.0);
    vec4 tint = umbriel_palette_count > 0 ? umbriel_palette_at(0.0) : vec4(0.48, 0.64, 1.0, 1.0);
    float glow = exp(-d / 12.0) * (0.35 + 0.65 * wave);
    vec4 native = umbriel_sample(uv);
    return native * (1.0 - glow) + tint * glow * max(ring, 0.6 * exp(-d / 12.0));
}
