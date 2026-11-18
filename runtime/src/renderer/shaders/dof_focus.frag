#version 450
#extension GL_GOOGLE_include_directive : require
// --cutscene-dof focus (one pixel, the 1x1 state; dof.c dof_focus_range, dof_focus_pick,
// dof_grid_weight and dof_focus_step): the sharp range of the frame. With characters visible on a
// 16 x 12 grid over the central area (u 0.20-0.80, v 0.15-0.85; the character mask's depth is the
// visible one there), it spans the nearest to the farthest of them, widened by char_margin on each
// side: a back turned to the camera and the speaker behind it are both sharp. Without, as before:
// the view distances on a 9 x 7 grid over the centre (u 0.28-0.72, v 0.22-0.78), each weighted
// toward the centre, their weighted 35th percentile (the nearer third) at both ends. Each end
// (log2 distance) moves toward its target with time constant tau, or jumps to it on a snap.
// With a speaker (--cutscene-dof-speaker: pc.spk), the range is his view distance widened by
// char_margin instead (dof.c dof_speaker_range). Nothing drawn there (the sky): the range holds.
// Out: log2 near, log2 far, 3 (speaker) / 2 (characters) / 1 (centre meter) / 0 (nothing metered),
// the frame's weight.
#include "dof_common.glsl"
layout(location = 0) out vec4 o;
const int GX = 9, GY = 7, N = GX * GY;
const int CX = 16, CY = 12;

void main() {
    float cn = 0.0, cf = 0.0;
    for (int y = 0; y < CY; y++)
        for (int x = 0; x < CX; x++) {
            ivec2 q = ivec2(vec2(0.20 + 0.60 * float(x) / float(CX - 1), 0.15 + 0.70 * float(y) / float(CY - 1)) * pc.size.xy);
            if (!is_char(q)) continue;
            float ds = view_dist(depth_at(q));
            cn = cn == 0.0 ? ds : min(cn, ds);
            cf = max(cf, ds);
        }
    vec2 target;
    float metered;
    if (pc.spk.x > 0.0 && pc.spk.y > 0.0) {   // a speaker (dof.c dof_speaker_range): the range around him
        target = max(vec2(pc.spk.y / (1.0 + pc.half_.w), pc.spk.y * (1.0 + pc.half_.w)), vec2(pc.focus.x));
        metered = 3.0;
    } else if (cf > 0.0) {
        target = max(vec2(cn / (1.0 + pc.half_.w), cf * (1.0 + pc.half_.w)), vec2(pc.focus.x));
        metered = 2.0;
    } else {
        float d[N], w[N];
        float total = 0.0;
        for (int y = 0; y < GY; y++)
            for (int x = 0; x < GX; x++) {
                float u = 0.28 + 0.44 * float(x) / float(GX - 1), v = 0.22 + 0.56 * float(y) / float(GY - 1);
                float z = depth_at(ivec2(vec2(u, v) * pc.size.xy));
                int i = y * GX + x;
                d[i] = z < 1.0 ? max(view_dist(z), pc.focus.x) : 0.0;
                float du = u - 0.5, dv = v - 0.5;
                w[i] = exp(-(du * du / (2.0 * 0.12 * 0.12) + dv * dv / (2.0 * 0.16 * 0.16)));
                if (d[i] > 0.0) total += w[i];
            }
        float best = 0.0;
        for (int i = 0; i < N; i++) {
            if (d[i] <= 0.0 || (best > 0.0 && d[i] >= best)) continue;
            float below = 0.0;
            for (int k = 0; k < N; k++)
                if (d[k] > 0.0 && d[k] <= d[i]) below += w[k];
            if (below >= pc.focus.y * total) best = d[i];
        }
        target = vec2(best);
        metered = best > 0.0 ? 1.0 : 0.0;
    }
    vec4 prev = texelFetch(fstate, ivec2(0), 0);
    vec2 r = prev.xy;
    if (metered > 0.0) {
        vec2 m = log2(target);
        r = pc.focus.w > 0.5 ? m : r + (m - r) * (1.0 - exp(-min(pc.proj.w, 0.1) / pc.focus.z));
    }
    o = vec4(r, metered, pc.proj.z);
}
