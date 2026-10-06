// Stationary spectrum visualizer. No clock, phase advance, or synthetic waves.
// Each window edge maps the same sixteen live frequency bands to fixed bars.
// Adjacent edges reverse the mapping so their corner values join continuously.
vec2 music_position(vec2 p, vec2 size) {
    vec2 q = clamp(p, vec2(0.0), size);
    vec4 edge = abs(vec4(p.x, p.y, size.x - p.x, size.y - p.y));
    float nearest = min(min(edge.x, edge.y), min(edge.z, edge.w));
    if (nearest == edge.y) return vec2(q.x / max(size.x, 1.0), size.x);
    if (nearest == edge.z) return vec2(1.0 - q.y / max(size.y, 1.0), size.y);
    if (nearest == edge.w) return vec2(1.0 - q.x / max(size.x, 1.0), size.x);
    return vec2(q.y / max(size.y, 1.0), size.y);
}
vec4 music_spectrum(vec2 position, float distance) {
    float frequency = clamp(position.x, 0.0, 1.0);
    float count = max(16.0, floor(position.y / 4.0));
    float column = min(floor(frequency * count), count - 1.0);
    float band = sqrt(clamp(32.0 * umbriel_audio_band((column + 0.5) / count), 0.0, 1.0));
    float smooth_band = sqrt(clamp(32.0 * umbriel_audio_band(frequency), 0.0, 1.0));
    if (max(band, smooth_band) < 0.001) return vec4(0.0);
    float level = sqrt(clamp(16.0 * max(umbriel_audio_rms(), 0.5 * umbriel_audio_level()), 0.0, 1.0));
    float d = abs(distance);
    float height = 44.0 * band;
    float contour = 44.0 * smooth_band;
    float bar = 1.0 - smoothstep(0.28, 0.46, abs(fract(frequency * count) - 0.5));
    float filled = 1.0 - smoothstep(max(height - 0.7, 0.0), height + 0.7, d);
    float body = filled * bar * (0.42 + 0.58 * clamp(1.0 - d / max(height, 0.001), 0.0, 1.0));
    float rim = (1.0 - smoothstep(0.45, 1.15, abs(d - contour))) * smooth_band;
    float echo = (1.0 - smoothstep(0.35, 0.95, abs(d - contour * 0.70))) * smooth_band;
    float glow = 0.07 * exp(-abs(d - contour) * 0.55) * smooth_band;
    float alpha = (0.32 + 0.68 * level) * (0.68 * body * band + 0.85 * rim + 0.25 * echo + glow);
    alpha = clamp(alpha * (1.0 - smoothstep(44.5, 47.0, d)), 0.0, 0.94);
    vec3 cyan = vec3(0.015, 0.90, 1.0), blue = vec3(0.12, 0.23, 1.0), pink = vec3(1.0, 0.055, 0.78);
    vec3 colour = frequency < 0.5 ? mix(cyan, blue, frequency * 2.0) : mix(blue, pink, frequency * 2.0 - 1.0);
    colour = mix(colour, vec3(0.76, 0.96, 1.0), clamp(0.65 * rim, 0.0, 0.65));
    return vec4(colour * alpha, alpha);
}
vec4 window(vec2 uv) {
    vec4 scene = umbriel_sample(uv);
    vec2 p = uv * umbriel_size;
    vec2 edge = min(p, umbriel_size - p);
    float d = max(min(edge.x, edge.y), 0.0);
    // The same bands reflect inside the edge, compressed to protect content.
    vec4 wave = music_spectrum(music_position(p, umbriel_size), -d * 1.55) * 0.68;
    return vec4(scene.rgb * (1.0 - wave.a) + wave.rgb * scene.a, scene.a);
}
