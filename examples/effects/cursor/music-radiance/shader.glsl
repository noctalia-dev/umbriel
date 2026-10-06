// Expanding rainbow waves follow playback energy; the native pointer stays sharp.
vec4 cursor(vec2 uv) {
    vec4 scene = umbriel_sample(uv);
    float level = sqrt(clamp(16.0 * max(umbriel_audio_rms(), 0.5 * umbriel_audio_level()), 0.0, 1.0));
    if (level < 0.001) return scene;
    vec2 p = (uv - umbriel_pointer) * umbriel_size;
    float radius = length(p);
    float angle = atan(p.y, p.x);
    // Sweep the spectrum out and back so both sides of the angle wrap agree.
    float frequency = 0.5 + 0.5 * cos(angle);
    float band = sqrt(clamp(24.0 * umbriel_audio_band(frequency), 0.0, 1.0));
    float energy = clamp(0.8 * level + 0.4 * band, 0.0, 1.0);
    float phase = radius / (26.0 + 16.0 * energy) - umbriel_time * 0.85;
    float wave = exp(-pow((fract(phase) - 0.5) * 7.0, 2.0));
    float rays = 0.65 + 0.35 * pow(0.5 + 0.5 * cos(angle * 7.0 - radius * 0.035 + umbriel_time), 2.0);
    float falloff = exp(-radius / (45.0 + 65.0 * energy));
    float edge = 1.0 - smoothstep(135.0, 158.0, radius);
    vec3 colour = 0.55 + 0.45 * cos(6.2831853 * (angle / 6.2831853 + radius * 0.004
        - umbriel_time * 0.07 + band * 0.18 + vec3(0.0, 0.33, 0.67)));
    float glow = clamp(energy * (0.22 + 0.95 * wave) * rays * falloff * edge, 0.0, 0.8);
    return vec4(mix(scene.rgb, colour * scene.a, glow), scene.a);
}
