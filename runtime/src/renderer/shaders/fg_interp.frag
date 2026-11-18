#version 450
// --frame-gen interpolation (renderer_frame_gen): the frame half way between
// the previous and the new one, at the display size, from both outputs as shown. A pixel the motion
// field marks still and in front of everything (distance 0: the HUD, fg_hud.comp; outside the 3D
// viewport: the bars) is the new frame's. Each other pixel takes the motion of the nearest surface
// that the splat (fg_splat.comp) landed on it, or, in a hole, of the surface around it (see below).
// Along that motion the new frame is read half a step back and the previous one half a
// step forward; a side whose own view distance there is clearly nearer than the surface's is covered
// on that side and dropped. Both sides: blended, unless they disagree strongly (an effect without
// motion vectors: rain, a flicker): then, with optical flow (--frame-gen-of), the block's flow
// (fg_flow.comp) is tried the same way and blended when both frames agree along it, else the new
// frame's. A side that lands on the HUD or the bars counts as covered; with neither side, the new
// frame's pixel.
layout(set = 0, binding = 0) uniform sampler2D cur_tex;     // the new frame as shown (linear)
layout(set = 0, binding = 1) uniform sampler2D prev_tex;    // the previous frame as shown (linear)
layout(set = 0, binding = 2) uniform sampler2D mv_tex;      // new frame's motion field (fg_mv.comp)
layout(set = 0, binding = 3) uniform sampler2D mvp_tex;     // previous frame's motion field (its distances)
layout(set = 0, binding = 4) uniform usampler2D index_tex;  // the splat's winning pixel per pixel
layout(set = 0, binding = 6) uniform sampler2D flow_tex;    // optical flow per 16 x 16 block (fg_flow.comp)
layout(push_constant) uniform Push {
    vec4 size;   // display width, height, 1 / width, 1 / height
    vec4 mode;   // x: reset (the new frame as is), y: colour disagreement limit, z: optical flow (1)
} pc;
layout(location = 0) out vec4 o;
const uint NONE = 0xFFFFFFFFu;

vec4 motion_of(uint k) {
    ivec2 q = ivec2(int(k % uint(pc.size.x)), int(k / uint(pc.size.x)));
    return texelFetch(mv_tex, q, 0);
}

// The view distance seen at pos (display pixels) in a motion field: covered when nearer than w by
// more than 4 % (a sloping ground stays itself), or by the HUD or the bars (distance 0).
bool covered(sampler2D field, vec2 pos, float w) {
    float seen = texelFetch(field, ivec2(pos), 0).z;
    return seen <= 0.0 || (w > 0.0 && seen < w * 0.96 - 0.01);
}

void main() {
    ivec2 g = ivec2(gl_FragCoord.xy);
    vec4 real = texelFetch(cur_tex, g, 0);
    if (pc.mode.x > 0.5 || texelFetch(mv_tex, g, 0).z <= 0.0) {
        o = real;
        return;
    }
    ivec2 last = ivec2(pc.size.xy) - 1;
    uint k = texelFetch(index_tex, g, 0).r;
    if (k == NONE) {
        // A hole. Mostly surrounded by one surface (a stretch: a fast or turning surface whose pixels
        // spread apart half way): that surface. Else (something uncovered or being covered): the
        // farthest surface around it, rings outward; after the first surface found the search goes on
        // as far as that surface moved half a frame (the width of what it uncovered or is covering), so
        // the surface on the hole's far side is seen too.
        const ivec2 dirs[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1));
        float wn[8];
        uint kn[8];
        int found = 0;
        float dn = 1e30;
        uint kmin = NONE;
        for (int i = 0; i < 8; i++) {
            kn[i] = texelFetch(index_tex, clamp(g + dirs[i], ivec2(0), last), 0).r;
            wn[i] = kn[i] == NONE ? -1.0 : motion_of(kn[i]).z;
            if (kn[i] == NONE) continue;
            found++;
            if (wn[i] < dn) {
                dn = wn[i];
                kmin = kn[i];
            }
        }
        int near_n = 0;
        for (int i = 0; i < 8; i++) near_n += kn[i] != NONE && wn[i] <= dn * 1.1 + 0.01 ? 1 : 0;
        if (found >= 6 && near_n >= 5) {
            k = kmin;
        } else {
            float best = -1.0, limit = 32.0;
            const int radii[13] = int[13](1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32);
            for (int j = 0; j < 13 && float(radii[j]) <= limit; j++) {
                for (int i = 0; i < 8; i++) {
                    uint n = texelFetch(index_tex, clamp(g + dirs[i] * radii[j], ivec2(0), last), 0).r;
                    if (n == NONE) continue;
                    vec4 mn = motion_of(n);
                    if (k == NONE) limit = min(32.0, max(2.0 * float(radii[j]), 0.5 * length(mn.xy) + 2.0));
                    if (mn.z > best) {
                        best = mn.z;
                        k = n;
                    }
                }
            }
        }
    }
    if (k == NONE) {
        o = real;
        return;
    }
    vec4 m = motion_of(k);
    vec2 mv = m.w < 0.0 ? vec2(0.0) : m.xy;
    vec2 c = vec2(g) + 0.5;
    vec2 pc_cur = c - 0.5 * mv, pc_prev = c + 0.5 * mv;
    bool cur_in = all(greaterThanEqual(pc_cur, vec2(0.0))) && all(lessThan(pc_cur, pc.size.xy));
    bool prev_in = m.w >= 0.0 && all(greaterThanEqual(pc_prev, vec2(0.0))) && all(lessThan(pc_prev, pc.size.xy));
    bool cur_ok = cur_in && !covered(mv_tex, pc_cur, m.z);
    bool prev_ok = prev_in && !covered(mvp_tex, pc_prev, m.w);
    vec4 cc = texture(cur_tex, pc_cur * pc.size.zw);
    vec4 cp = texture(prev_tex, pc_prev * pc.size.zw);
    if (cur_ok && prev_ok) {
        vec3 d = abs(cc.rgb - cp.rgb);
        o = max(d.r, max(d.g, d.b)) > pc.mode.y ? cc : mix(cc, cp, 0.5);
        if (max(d.r, max(d.g, d.b)) > pc.mode.y && pc.mode.z > 0.5) {
            vec2 f = texelFetch(flow_tex, g / 16, 0).xy;
            vec4 fc = texture(cur_tex, (c - 0.5 * f) * pc.size.zw), fp = texture(prev_tex, (c + 0.5 * f) * pc.size.zw);
            vec3 e = abs(fc.rgb - fp.rgb);
            if (max(e.r, max(e.g, e.b)) <= pc.mode.y) o = mix(fc, fp, 0.5);
        }
    } else if (cur_ok) {
        o = cc;
    } else if (prev_ok) {
        o = cp;
    } else {
        o = real;
    }
}
