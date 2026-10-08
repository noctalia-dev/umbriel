vec4 border(vec2 uv) {
    float brightness = 0.2 + 0.8 * umbriel_audio_level();
    return vec4(vec3(0.2, 0.5, 1.0) * brightness, 1.0);
}
