#version 450
// --taa resolve (renderer_taa; src/taa.h): the frame (resolved, its 3D scene jittered) blended into
// the reprojected history. Per pixel: the nearest depth of the 3 x 3 neighbourhood gives the view
// distance (NDC depth = a - b / w; at the far end a direction), the reprojection map takes the
// pixel's clip position to the previous frame's (taa.c taa_reproject_point), the history is read
// there with a Catmull-Rom filter and clipped toward the neighbourhood's mean inside its variance
// box (YCoCg, at most its min/max), then blended with the frame: `blend` of it, more for faster
// motion. A skinned character's pixels (the velocity pass: the same surface's distance at the
// nearest tap) use its own previous position and distance instead of the camera's. Disocclusion: the history's view distances (the nearest of each pixel's 3 x 3, out 1) at
// the previous position (the nearest of the 2 x 2 under it) against this point's previous distance
// (the reprojection's w): a surface that was not the one seen there (a character moved off it or
// onto it) takes the frame instead, fading in from a 3 % to an 8 % difference plus the depth spread
// of the 3 x 3 (relative; twice it at the top). Off screen, behind
// the camera or with reset: the frame as is. The alpha (bloom light mask) is clamped to the
// neighbourhood's range and blended alike. Out: the new history (RGBA16F) and its distances (R16F;
// the far end: SKY_DIST).
layout(set = 0, binding = 0) uniform sampler2D cur_tex;
#ifdef MSAA
layout(set = 0, binding = 1) uniform sampler2DMS depth_tex;
#else
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
#endif
layout(set = 0, binding = 2) uniform sampler2D hist_tex;
layout(set = 0, binding = 3) uniform sampler2D dist_tex;
layout(set = 0, binding = 4) uniform sampler2D vel_tex;
layout(push_constant) uniform Push {
    vec4 rx, ry, rw;   // the reprojection map's rows (previous clip x, y, w)
    vec4 proj;         // depth a, depth b, blend (the frame's share at rest), reset (1)
    vec4 size;         // target width, height, 1 / width, 1 / height
    vec4 vp;           // the 3D viewport's x, y (target pixels), 2 / width, 2 / height
    vec4 obj;          // x: 1 = vel_tex holds this frame's character motion; yz: this frame's jitter (target pixels)
} pc;
layout(location = 0) out vec4 o;
layout(location = 1) out float od;
const float SKY_DIST = 60000.0;

vec3 ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 rgb_of(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

float depth_at(ivec2 q) { return texelFetch(depth_tex, q, 0).r; }

// Catmull-Rom over the history: the 4 x 4 taps folded into 5 bilinear reads (corners dropped).
vec4 history(vec2 uv) {
    vec2 pos = uv * pc.size.xy;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c - 1.0) * pc.size.zw, t3 = (c + 2.0) * pc.size.zw, t12 = (c + w2 / w12) * pc.size.zw;
    float a = w12.x * w0.y, b = w0.x * w12.y, m = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
    vec4 s = texture(hist_tex, vec2(t12.x, t0.y)) * a + texture(hist_tex, vec2(t0.x, t12.y)) * b + texture(hist_tex, t12) * m +
             texture(hist_tex, vec2(t3.x, t12.y)) * d + texture(hist_tex, vec2(t12.x, t3.y)) * e;
    return clamp(s / (a + b + m + d + e), vec4(0.0), vec4(1.0));
}

// h clipped toward the box's centre onto the box [lo, hi] (a point inside is kept).
vec3 clip_box(vec3 lo, vec3 hi, vec3 h) {
    vec3 c = 0.5 * (hi + lo), ext = 0.5 * (hi - lo) + 1e-4;
    vec3 v = h - c, r = abs(v / ext);
    float k = max(r.x, max(r.y, r.z));
    return k > 1.0 ? c + v / k : h;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), last = ivec2(pc.size.xy) - 1;
    vec4 c = texelFetch(cur_tex, p, 0);
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    float alo = 1.0, ahi = 0.0, dn = 1.0, df = 0.0;
    ivec2 pn = p;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), last);
            vec4 s = texelFetch(cur_tex, q, 0);
            vec3 t = ycocg(s.rgb);
            m1 += t;
            m2 += t * t;
            lo = min(lo, t);
            hi = max(hi, t);
            alo = min(alo, s.a);
            ahi = max(ahi, s.a);
            float d = depth_at(q);
            if (d < dn) {
                dn = d;
                pn = q;
            }
            df = max(df, d);
        }
    vec2 ndc = (gl_FragCoord.xy - pc.vp.xy) * pc.vp.zw - 1.0;
    ndc.y = -ndc.y;
    vec4 v = vec4(ndc, 1.0, 0.0);
    float a = pc.proj.x, b = pc.proj.y, wn = SKY_DIST;
    bool near = dn < 1.0 && a - dn > 0.0 && b > 0.0;
    if (near) {
        wn = min(b / (a - dn), SKY_DIST);
        v = vec4(ndc * wn, wn, 1.0);
    }
    od = wn;
    vec3 q = vec3(dot(pc.rx, v), dot(pc.ry, v), dot(pc.rw, v));
    vec2 tap = vec2(0.0);   // a character's motion is read at the nearest tap: its offset from this pixel
    if (pc.obj.x > 0.5 && near) {   // a skinned character's surface (its distance matches the frame's): its own motion
        vec4 ve = texelFetch(vel_tex, pn, 0);
        if (ve.z > 0.0 && ve.w > 0.0 && abs(ve.w - wn) < 0.03 * wn + 0.01) {
            q = vec3(ve.xy * ve.z, ve.z);
            // Its previous position is the unjittered surface point's, which this frame's jitter put
            // here: the jitter goes back on (as the camera path, which starts from the pixel itself),
            // or the history is read the jitter away and every still character shakes with it.
            tap = vec2(pn - p) - pc.obj.yz;
        }
    }
    vec2 prev = vec2(q.x / q.z, -q.y / q.z);
    vec2 px = (prev + 1.0) / pc.vp.zw + pc.vp.xy - tap;
    vec2 uv = px * pc.size.zw;
    if (pc.proj.w > 0.5 || !(q.z > 0.0) || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        o = c;
        return;
    }
    ivec2 h0 = clamp(ivec2(floor(px - 0.5)), ivec2(0), last), h1 = min(h0 + 1, last);
    float dp = min(min(texelFetch(dist_tex, h0, 0).r, texelFetch(dist_tex, ivec2(h1.x, h0.y), 0).r),
                   min(texelFetch(dist_tex, ivec2(h0.x, h1.y), 0).r, texelFetch(dist_tex, h1, 0).r));
    float wq = near ? min(q.z, SKY_DIST) : SKY_DIST;
    // The tolerance grows with the depth spread of this 3 x 3 (edges, slopes seen at a grazing angle):
    // a character a few steps in front of a wall is told apart from it, a sloping street is not.
    float wf = df < 1.0 && a - df > 0.0 && b > 0.0 ? min(b / (a - df), SKY_DIST) : SKY_DIST;
    float spread = min((wf - wn) / max(wn, 1e-4), 1.0);
    float occl = smoothstep(0.03 + spread, 0.08 + 2.0 * spread, abs(dp - wq) / max(min(dp, wq), 1e-4));
    vec4 h = history(uv);
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, vec3(0.0)));
    vec3 hy = clip_box(max(mu - sigma, lo), min(mu + sigma, hi), ycocg(h.rgb));
    vec3 cy = ycocg(c.rgb);
    float speed = length(px - gl_FragCoord.xy);
    float k = max(clamp(pc.proj.z + 0.02 * speed, pc.proj.z, 0.4), occl);
    o = vec4(clamp(rgb_of(mix(hy, cy, k)), 0.0, 1.0), mix(clamp(h.a, alo, ahi), c.a, k));
}
