// Faint horizontal scanlines over the window, one logical pixel in three.
vec4 window(vec2 uv) {
    vec4 c = umbriel_sample(uv);
    float line = mod(floor(uv.y * umbriel_size.y), 3.0) == 0.0 ? 0.85 : 1.0;
    return vec4(c.rgb * line, c.a);
}
