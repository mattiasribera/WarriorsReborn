// --auto-exposure (src/auto_exposure.h): shared by filmic.frag (its AUTO_EXPOSURE variant) and
// exposure.frag. ae_state is the 1x1 adaptation state (ae_adapt.frag: x adapted level, y gain in
// stops, z cut timer, w meter).
layout(set = 0, binding = 3) uniform sampler2D ae_state;

float ae_gain() { return exp2(texelFetch(ae_state, ivec2(0), 0).y); }

// src/auto_exposure.c auto_exposure_rolloff: identity below the knee k and when tmax <= 1, else
// k + (1 - k) u / (1 + a u) with tmax mapped to 1.
vec3 ae_rolloff(vec3 t, float tmax, float k) {
    if (tmax <= 1.0) return t;
    float T = (tmax - k) / (1.0 - k), a = (T - 1.0) / T;
    vec3 u = max(t - k, 0.0) / (1.0 - k);
    return mix(t, k + (1.0 - k) * u / (1.0 + a * u), step(k, t));
}

// Dave Hoskins' hash12 (MIT).
float ae_hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

// Triangular (TPDF) dither of +-1 LSB of the 8-bit target, new every frame (seed).
float ae_dither(vec2 p, float seed) {
    return (ae_hash(p + seed * vec2(37.0, 17.0)) + ae_hash(p.yx + 91.7 + seed * vec2(13.0, 29.0)) - 1.0) / 255.0;
}
