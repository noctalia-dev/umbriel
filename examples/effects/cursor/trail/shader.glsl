// History UVs share the drawn rectangle; distances remain in logical pixels.
vec4 cursor(vec2 uv) {
    vec4 background = umbriel_sample(uv);
    float coverage = 0.0;
    vec3 colour = vec3(0.22, 0.16, 0.38);
    for (int i = 0; i < 7; ++i) {
        if (i + 1 < umbriel_pointer_count) {
            vec4 a = umbriel_pointer_history[i];
            vec4 b = umbriel_pointer_history[i + 1];
            vec2 segment = (b.xy - a.xy) * umbriel_size;
            vec2 offset = (uv - a.xy) * umbriel_size;
            float t = clamp(dot(offset, segment) / max(dot(segment, segment), 0.001), 0.0, 1.0);
            float life = clamp(1.0 - mix(a.z, b.z, t) / 0.3, 0.0, 1.0);
            float distance = length(offset - segment * t);
            float width = mix(0.5, 3.5, life);
            float glow = exp(-distance * distance / (width * width)) * life * life;
            vec3 tint = mix(vec3(0.22, 0.16, 0.38), vec3(0.65, 0.61, 0.91), smoothstep(0.0, 0.65, life));
            tint = mix(tint, vec3(0.98, 0.93, 0.61), smoothstep(0.75, 1.0, life));
            if (glow > coverage) {
                coverage = glow;
                colour = tint;
            }
        }
    }
    return mix(background, vec4(colour, 1.0), coverage * 0.85);
}
