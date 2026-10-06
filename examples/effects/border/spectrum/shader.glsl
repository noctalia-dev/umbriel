// Audio alone changes this ring: no shader clock is needed. The normalized
// angle traverses sixteen bands. Compress the display response so ordinary
// quiet playback remains visible without changing the linear audio profile.
vec4 border(vec2 uv) {
    float position = (atan(uv.y - 0.5, uv.x - 0.5) + 3.14159265) / 6.2831853;
    float band = sqrt(clamp(24.0 * umbriel_audio_band(position), 0.0, 1.0));
    float level = sqrt(clamp(16.0 * max(umbriel_audio_rms(), 0.5 * umbriel_audio_level()), 0.0, 1.0));
    float strength = clamp(0.65 * band + 0.55 * level, 0.0, 1.0);
    float distance = max(umbriel_border_distance(uv), 0.0);
    vec4 native = umbriel_sample(uv);
    vec4 tint = umbriel_palette_count > 0 ? umbriel_palette_at(fract(position + 0.25 * level)) : vec4(0.25, 0.65, 1.0, 1.0);
    float halo = 0.9 * exp(-distance / (6.0 + 18.0 * strength));
    halo *= 1.0 - smoothstep(18.0, 24.0, distance);
    float alpha = strength * max(native.a, halo);
    return native * (1.0 - strength) + tint * alpha;
}
