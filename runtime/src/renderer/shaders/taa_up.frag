#version 450
// --taa-upscale resolve (RendererConfig.taa_up_w): taa_resolve.frag's blend with
// the 3D scene rendered below the output size. The frame (its 3D scene jittered) fills the top-left
// render-size rectangle of cur_tex / depth_tex / vel_tex; this pass runs over the output-size history.
// For an output pixel P its unjittered render position is P x scale, which the jittered frame shows
// at pr = P x scale + jitter. The frame alone is read at pr with Catmull-Rom (4 x 4 render pixels,
// kept inside the 2 x 2 around pr: no ringing): where there is no history and, leaned on more the
// faster the pixel moves (a Gaussian here blurred moving scenes). The 3 x 3
// render pixels around pr give the nearest sample and its distance to P in output pixels (its
// confidence, exp(-3.5 d^2), weighs its share of the blend: a sample landing on P counts most, so
// the still image converges to the output-size detail over the jitter sequence), the variance box
// the history is clipped to (YCoCg) and the nearest depth (the motion and the disocclusion test, as
// taa_resolve.frag, with the character velocity read at that tap). The history (output size) is read
// with Catmull-Rom at the reprojected position. Out: the new
// history (RGBA16F), its distances (R16F) and the output pixel's depth (R32F: the depth of the render
// pixel under pr, which the output pass writes back so the passes after the resolve see the scene's
// depth at the output size).
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
    vec4 size;         // output width, height, 1 / width, 1 / height
    vec4 vp;           // the 3D viewport's x, y (output pixels), 2 / width, 2 / height
    vec4 obj;          // x: 1 = vel_tex holds this frame's character motion; y, z: the jitter (render pixels)
    vec4 up;           // render width, height; render / output per axis
} pc;
layout(location = 0) out vec4 o;
layout(location = 1) out float od;
layout(location = 2) out float oz;
const float SKY_DIST = 60000.0;

vec3 ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 rgb_of(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

float depth_at(ivec2 q) { return texelFetch(depth_tex, q, 0).r; }

// The scene's depth at render position pr for the output pixel (oz). The passes after the resolve
// (the wet ground) rebuild positions, normals and flatness from it, so it
// must be as smooth as the surface: the nearest render pixel's depth repeats every other output row at
// 1.5x (a staircase on the ground: rows of bands in the wet floor). NDC depth is affine in screen
// position over a plane, so the 2 x 2 render pixels around pr are interpolated bilinearly when they
// lie on one plane (the diagonals agree) and on one surface (view depths within DEPTH_SPREAD of each
// other); at a silhouette, the sky or a crease the nearest pixel's depth is kept (no depth between
// two surfaces).
const float DEPTH_PLANE = 0.25, DEPTH_SPREAD = 0.08;
float depth_up(vec2 pr, ivec2 rlast, float nearest) {
    vec2 f = pr - 0.5;
    ivec2 b = ivec2(floor(f));
    vec2 t = f - vec2(b);
    float d00 = depth_at(clamp(b, ivec2(0), rlast)), d10 = depth_at(clamp(b + ivec2(1, 0), ivec2(0), rlast));
    float d01 = depth_at(clamp(b + ivec2(0, 1), ivec2(0), rlast)), d11 = depth_at(clamp(b + ivec2(1, 1), ivec2(0), rlast));
    float hi = max(max(d00, d10), max(d01, d11)), lo = min(min(d00, d10), min(d01, d11));
    float a = pc.proj.x, bb = pc.proj.y;
    if (hi >= 1.0 || !(bb > 0.0) || a - hi <= 0.0) return nearest;
    float wlo = bb / (a - lo), whi = bb / (a - hi);
    float step = max(max(abs(d10 - d00), abs(d01 - d00)), max(abs(d11 - d10), abs(d11 - d01)));
    if (whi - wlo > DEPTH_SPREAD * wlo || abs(d00 + d11 - d10 - d01) > DEPTH_PLANE * step + 1e-7) return nearest;
    return mix(mix(d00, d10, t.x), mix(d01, d11, t.x), t.y);
}

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

// The render frame at pr (render pixels) with Catmull-Rom over its 4 x 4 pixels, clamped to the
// range of the 2 x 2 nearest pr (the inner taps) so its negative lobes cannot ring.
vec4 frame_at(vec2 pr, ivec2 rlast) {
    vec2 c = floor(pr - 0.5) + 0.5;
    vec2 f = pr - c;
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    ivec2 b = ivec2(c - 0.5) - 1;
    vec4 s = vec4(0.0), lo = vec4(1e9), hi = vec4(-1e9);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            vec4 t = texelFetch(cur_tex, clamp(b + ivec2(x, y), ivec2(0), rlast), 0);
            s += t * (w[x].x * w[y].y);
            if (x == 1 || x == 2)
                if (y == 1 || y == 2) {
                    lo = min(lo, t);
                    hi = max(hi, t);
                }
        }
    return clamp(s, lo, hi);
}

void main() {
    ivec2 last = ivec2(pc.size.xy) - 1, rlast = ivec2(pc.up.xy) - 1;
    vec2 pr = gl_FragCoord.xy * pc.up.zw + pc.obj.yz;   // this output pixel's centre in the jittered frame (render pixels)
    ivec2 qc = ivec2(floor(pr));
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    vec4 nearest = vec4(0.0);
    float alo = 1.0, ahi = 0.0, dn = 1.0, df = 0.0, dmin = 1e9;
    ivec2 pn = clamp(qc, ivec2(0), rlast);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 g = qc + ivec2(x, y), q = clamp(g, ivec2(0), rlast);
            vec4 s = texelFetch(cur_tex, q, 0);
            vec2 dd = (vec2(g) + 0.5 - pr) / pc.up.zw;   // in output pixels
            float e2 = dot(dd, dd);
            if (e2 < dmin) {
                dmin = e2;
                nearest = s;
            }
            vec3 t = ycocg(s.rgb);
            m1 += t;
            m2 += t * t;
            lo = min(lo, t);
            hi = max(hi, t);
            alo = min(alo, s.a);
            ahi = max(ahi, s.a);
            float z = depth_at(q);
            if (z < dn) {
                dn = z;
                pn = q;
            }
            df = max(df, z);
        }
    vec4 c = frame_at(pr, rlast);
    oz = depth_up(pr, rlast, depth_at(clamp(qc, ivec2(0), rlast)));
    float conf = exp(-3.5 * dmin);   // 1 for a sample on the output pixel's centre, 0.42 half an output pixel away
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
    vec2 tap = vec2(0.0);   // a character's motion is read at the nearest tap: its offset from this pixel (output pixels)
    if (pc.obj.x > 0.5 && near) {
        vec4 ve = texelFetch(vel_tex, pn, 0);
        if (ve.z > 0.0 && ve.w > 0.0 && abs(ve.w - wn) < 0.03 * wn + 0.01) {
            q = vec3(ve.xy * ve.z, ve.z);
            tap = (vec2(pn) + 0.5 - pr) / pc.up.zw;
        }
    }
    vec2 prev = vec2(q.x / q.z, -q.y / q.z);
    vec2 px = (prev + 1.0) / pc.vp.zw + pc.vp.xy - tap;
    vec2 uv = px * pc.size.zw;
    if (pc.proj.w > 0.5 || !(q.z > 0.0) || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        o = clamp(c, 0.0, 1.0);
        return;
    }
    ivec2 h0 = clamp(ivec2(floor(px - 0.5)), ivec2(0), last), h1 = min(h0 + 1, last);
    float dp = min(min(texelFetch(dist_tex, h0, 0).r, texelFetch(dist_tex, ivec2(h1.x, h0.y), 0).r),
                   min(texelFetch(dist_tex, ivec2(h0.x, h1.y), 0).r, texelFetch(dist_tex, h1, 0).r));
    float wq = near ? min(q.z, SKY_DIST) : SKY_DIST;
    float wf = df < 1.0 && a - df > 0.0 && b > 0.0 ? min(b / (a - df), SKY_DIST) : SKY_DIST;
    float spread = min((wf - wn) / max(wn, 1e-4), 1.0);
    float occl = smoothstep(0.03 + spread, 0.08 + 2.0 * spread, abs(dp - wq) / max(min(dp, wq), 1e-4));
    vec4 h = history(uv);
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, vec3(0.0)));
    vec3 hy = clip_box(max(mu - sigma, lo), min(mu + sigma, hi), ycocg(h.rgb));
    // Still pixels take the nearest sample (its confidence sets its share in k below); moving ones lean
    // on the frame read at pr (the history is softer and less trusted there).
    float speed = length(px - gl_FragCoord.xy);
    float moving = clamp(0.1 * speed, 0.0, 1.0);
    vec4 cur = mix(c, nearest, 1.0 - max(occl, moving));
    float k = max(clamp(2.0 * pc.proj.z * conf + 0.02 * speed, 0.02, 0.5), occl);
    vec3 cy = ycocg(cur.rgb);
    o = vec4(clamp(rgb_of(mix(hy, cy, k)), 0.0, 1.0), mix(clamp(h.a, alo, ahi), cur.a, k));
}
