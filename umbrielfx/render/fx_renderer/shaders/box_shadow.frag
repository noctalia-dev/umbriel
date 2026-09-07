// Writeup: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/

precision highp float;

varying vec4 v_color;
varying vec2 v_texcoord;

uniform vec2 position;
uniform vec2 size;
uniform float blur_sigma;
uniform float corner_radius;
uniform vec2 clip_position;
uniform vec2 clip_size;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

float gaussian(float x, float sigma) {
    const float pi = 3.141592653589793;
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

// approximates the error function, needed for the gaussian integral
vec2 erf(vec2 x) {
    vec2 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

// return the blurred mask along the x dimension
float roundedBoxShadowX(float x, float y, float sigma, float corner, vec2 halfSize) {
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf((x + vec2(-curved, curved)) * (sqrt(0.5) / sigma));
    return integral.y - integral.x;
}

// return the mask for the shadow of a box from lower to upper
float roundedBoxShadow(vec2 lower, vec2 upper, vec2 point, float sigma, float corner_radius) {
    // Center everything to make the math easier
    vec2 center = (lower + upper) * 0.5;
    vec2 halfSize = (upper - lower) * 0.5;
    point -= center;

    // The signal is only non-zero in a limited range, so don't waste samples
    float low = point.y - halfSize.y;
    float high = point.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);

    // Accumulate samples (we can get away with surprisingly few samples)
    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; i++) {
        value += roundedBoxShadowX(point.x, point.y - y, sigma, corner_radius, halfSize) * gaussian(y, sigma) * step;
        y += step;
    }

    return value;
}

float corner_alpha(vec2 size, vec2 position, bool is_cutout,
        float radius_tl, float radius_tr, float radius_bl, float radius_br);

void main() {
    float shadow_alpha;
    if (blur_sigma <= 0.0) {
        shadow_alpha = v_color.a;
    } else {
        shadow_alpha = v_color.a * roundedBoxShadow(
                position + blur_sigma,
                position + size - blur_sigma,
                gl_FragCoord.xy, blur_sigma * 0.5,
                corner_radius);
    }

    // Clipping
    float clip_corner_alpha = corner_alpha(
        clip_size - 1.5,
        clip_position + 0.75,
        true,
        clip_radius_top_left,
        clip_radius_top_right,
        clip_radius_bottom_left,
        clip_radius_bottom_right
    );

    gl_FragColor = vec4(v_color.rgb, shadow_alpha) * clip_corner_alpha;
}
