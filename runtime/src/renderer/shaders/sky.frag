#version 450
#extension GL_GOOGLE_include_directive : require
// Procedural sky (--sky-hd, renderer_sky; renderer.h has the layers). World z is up; every size is
// an angle, so the look does not depend on the resolution. Alpha: the bloom light mask.
#include "sky_common.glsl"
layout(push_constant) uniform PC {
    vec4 right;     // xyz; w: seconds
    vec4 up;        // xyz; w: cloud cover 0-1
    vec4 fwd;       // xyz; w: star chance per cell (RSky.stars)
    vec4 moon;      // direction xyz; w: angular radius (radians, 0 = none)
    uvec4 ramp_lo;  // XRGB ramp at 0, 12.9, 25.7, 38.6 degrees
    uvec4 ramp_hi;  // 51.4, 64.3, 77.1, 90 degrees
    uvec4 misc;     // silhouette XRGB, moon light (float bits)
} pc;
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

const float PI = 3.14159265;
vec3 xrgb(uint c) { return vec3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) / 255.0; }
vec3 ramp(int i) { return xrgb(i < 4 ? pc.ramp_lo[i] : pc.ramp_hi[i - 4]); }

// A distant city band along the horizon: building tops per azimuth cell (n cells over 360 degrees,
// hashed: no repeat), heights lo..hi radians times a slow district factor; returns the coverage of
// a pixel at elevation el (AA over px radians).
float city(float a, float el, float px, float n, float lo, float hi, int seed) {
    float c = floor(a * n);
    uint h = sky_hash3(ivec3(int(c), seed, 7));
    float r = sky_u01(h);
    float top = lo + (hi - lo) * r * r * (0.5 + sky_noise(vec2(a * 37.0, float(seed)), seed + 3));
    if (sky_u01(sky_hash(h ^ 0x51u)) > 0.94) top += hi;   // a tower now and then
    return clamp((top - el) / px + 0.5, 0.0, 1.0);
}

void main() {
    vec3 d = normalize(pc.fwd.xyz + v_ndc.x * pc.right.xyz + v_ndc.y * pc.up.xyz);
    const float t = pc.right.w, cover = pc.up.w, stars = pc.fwd.w;
    const float el = asin(clamp(d.z, -1.0, 1.0));
    const float px = max(length(fwidth(d)), 1e-5);   // radians per pixel
    vec3 h0 = ramp(0);

    // Gradient: the level's ramp by elevation.
    float k = clamp(el / (0.5 * PI), 0.0, 1.0) * 7.0;
    int i0 = int(min(floor(k), 6.0));
    vec3 sky = mix(ramp(i0), ramp(i0 + 1), k - float(i0));

    // Horizon glow varying slowly with azimuth (light pollution; the direction itself is the
    // noise coordinate, so there is no seam and no repeat).
    vec2 hz = normalize(d.xy + vec2(1e-6, 0.0));
    // The city band's azimuth, 0-1.
    const float a = atan(hz.y, hz.x) / (2.0 * PI) + 0.5;
    float glow = 1.0;
    if (el < 0.6) {   // exp(-el / 0.12) is under 1 % above
        glow = 0.7 + 0.45 * sky_noise(hz * 2.3 + 11.0, 1) + 0.25 * sky_noise(hz * 7.1 + 5.0, 2);
        sky *= mix(1.0, glow, exp(-max(el, 0.0) / 0.12));
    }

    float mask = 0.0;
    if (el > -0.01) {
        // Clouds on a plane above the camera, drifting with the clock; detail fades into the
        // horizon (the cover's mean colour stays).
        float cloud = 0.0, n = 0.0;
        if (d.z > 0.0) {
            vec2 p = d.xy / (d.z + 0.12) * 1.7 + vec2(0.011, 0.004) * t;
            n = 0.5 * sky_noise(p, 10) + 0.3 * sky_noise(p * 2.03 + 17.0, 11) + 0.2 * sky_noise(p * 4.11 + 31.0, 12);
            float edge = 1.0 - cover;
            cloud = smoothstep(edge - 0.12, edge + 0.22, n);
            cloud = mix(cover * 0.85, cloud, smoothstep(0.03, 0.2, d.z));
        } else {
            cloud = cover * 0.85;
        }

        // Stars: hashed cells on the sphere (sky.h SKY_STAR_CELLS, SKY_STAR_MIN_ELEVATION), one
        // point each at most with chance `stars` (sky.h SKY_STAR_CHANCE: a hazy city sky, a few
        // dozen on a screen); a gaussian about one pixel wide at any resolution, nearly white.
        float sl = 0.0;
        vec3 sc = vec3(0.0);
        if (stars > 0.0 && el > 0.02) {
            const float K = 160.0;
            ivec3 c = ivec3(floor(d * K));
            uint h = sky_hash3(c);
            if (sky_u01(h) < stars) {
                vec3 sp = normalize(vec3(c) + 0.2 + 0.6 * vec3(sky_u01(sky_hash(h ^ 1u)), sky_u01(sky_hash(h ^ 2u)), sky_u01(sky_hash(h ^ 3u))));
                float u = sky_u01(sky_hash(h ^ 4u));
                float ang = 2.0 * asin(min(length(d - sp) * 0.5, 1.0));
                float sigma = 0.6 * px;
                float tw = 1.0 + 0.12 * sin(t * (1.5 + 3.0 * u) + 6.2831 * u);
                sl = (0.3 + 0.5 * u * u * u) * tw * exp(-ang * ang / (2.0 * sigma * sigma));
                sl *= smoothstep(0.02, 0.35, el) * (1.0 - cloud);
                sc = mix(vec3(0.94, 0.96, 1.0), vec3(1.0, 0.97, 0.92), sky_u01(sky_hash(h ^ 5u)));
            }
        }

        // Moon: disc (limb darkened, faint maria), halo; behind the clouds.
        float disc = 0.0, halo = 0.0;
        vec3 mc = vec3(0.0);
        const float r = pc.moon.w, ml = uintBitsToFloat(pc.misc.y);
        if (r > 0.0 && ml > 0.0) {
            vec3 m = pc.moon.xyz;
            float ang = 2.0 * asin(min(length(d - m) * 0.5, 1.0));
            disc = clamp((r - ang) / px + 0.5, 0.0, 1.0);
            halo = (0.3 * exp(-max(ang - r, 0.0) / (r * 1.5)) + 0.1 * exp(-ang / (r * 10.0))) * ml;
            if (disc > 0.0) {
                vec3 e1 = normalize(cross(m, abs(m.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0))), e2 = cross(e1, m);
                vec2 q = vec2(dot(d, e1), dot(d, e2)) / r;
                float mu = sqrt(max(1.0 - dot(q, q), 0.0));
                float maria = 0.6 * sky_noise(q * 2.5 + 5.0, 20) + 0.4 * sky_noise(q * 6.0 + 9.0, 21);
                mc = vec3(0.93, 0.95, 1.0) * (0.6 + 0.4 * mu) * (0.78 + 0.3 * maria);
            }
        }

        sky += vec3(0.5, 0.58, 0.72) * halo + sc * sl;
        sky = mix(sky, mc, disc * ml);
        // Clouds lit from below by the city's glow (and near the moon by its halo).
        vec3 cc = max(mix(ramp(1), h0, 0.55) * (0.8 + 0.6 * n), sky * 1.05) + vec3(0.4, 0.45, 0.55) * halo * 0.6;
        sky = mix(sky, cc, cloud);
        mask = clamp(disc * ml * (1.0 - cloud), 0.0, 1.0);   // stars: no glow
    }

    // The distant city band: two rows of buildings, the far one hazed into the horizon glow, the
    // near one with a faint warm light per building (its windows would be a pixel or two: a grid of
    // them only sparkles). Below the horizon: the near row's colour. No building reaches 0.06
    // radians (the highest tower: 2 x 0.022 x 1.5 tops).
    if (el > 0.06 + px) {
        out_color = vec4(sky, mask);
        return;
    }
    vec3 sil = xrgb(pc.misc.x);
    float far = city(a, el, px, 900.0, 0.002, 0.012, 1);
    float near = city(a, el, px, 420.0, 0.003, 0.022, 2);
    vec3 farc = mix(sil, h0 * glow, 0.5), nearc = mix(sil, h0, 0.18);
    sky = mix(sky, farc, far);
    float lit = 0.01 + 0.03 * sky_u01(sky_hash3(ivec3(int(floor(a * 420.0)), 2, 9)));
    nearc += vec3(1.0, 0.75, 0.42) * lit * smoothstep(-0.004, 0.0, el);
    sky = mix(sky, nearc, near);
    out_color = vec4(sky, mask * (1.0 - near));
}
