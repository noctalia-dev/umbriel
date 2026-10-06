// Translucent rising smoke: playback shapes its billows, density and colour.
// Text coordinates and client alpha are preserved, and silence is native pixels.
float smoke_hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float smoke_noise(vec2 p) {
    vec2 cell = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(smoke_hash(cell), smoke_hash(cell + vec2(1.0, 0.0)), f.x),
        mix(smoke_hash(cell + vec2(0.0, 1.0)), smoke_hash(cell + vec2(1.0)), f.x), f.y);
}
float smoke_fbm(vec2 p) {
    float value = 0.0, weight = 0.5;
    for (int i = 0; i < 4; i++) {
        value += weight * smoke_noise(p);
        p = mat2(0.8, -0.6, 0.6, 0.8) * p * 2.03 + vec2(4.7, 9.2);
        weight *= 0.5;
    }
    return value;
}
vec4 window(vec2 uv) {
    vec4 scene = umbriel_sample(uv);
    float level = sqrt(clamp(16.0 * max(umbriel_audio_rms(), 0.5 * umbriel_audio_level()), 0.0, 1.0));
    if (level < 0.001 || scene.a < 0.001) return scene;
    float bass = sqrt(clamp(24.0 * umbriel_audio_band(0.12), 0.0, 1.0));
    float treble = sqrt(clamp(32.0 * umbriel_audio_band(0.72), 0.0, 1.0));
    vec2 p = (uv - 0.5) * vec2(umbriel_size.x / max(umbriel_size.y, 1.0), 1.0) * 4.5;
    p.y += umbriel_time * 0.24;
    vec2 curl = vec2(smoke_fbm(p + vec2(0.0, umbriel_time * 0.08)), smoke_fbm(p + vec2(5.2, 1.3)));
    float billow = smoke_fbm(p + (2.0 + 1.3 * bass) * curl);
    float wisps = smoothstep(0.22, 0.72, billow) * (0.65 + 0.35 * sin(billow * 19.0 + curl.y * 4.0));
    vec3 colour = 0.5 + 0.5 * cos(6.2831853 * (0.57 + billow * 0.35 + treble * 0.15
        + vec3(0.0, 0.33, 0.67)));
    colour = mix(vec3(0.24, 0.18, 0.38), colour, 0.75);
    float density = clamp((0.18 + 0.48 * wisps) * level + 0.08 * bass * wisps, 0.0, 0.52);
    return vec4(mix(scene.rgb, colour * scene.a, density), scene.a);
}
