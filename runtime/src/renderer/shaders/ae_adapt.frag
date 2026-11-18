#version 450
// --auto-exposure adaptation (src/auto_exposure.c auto_exposure_step: keep in step): one pixel, the
// new state from the previous one and the meter grid. Out: adapted level, gain in stops, 1 when it
// snapped, meter (log2). Only the host's snap (first frame, gap, camera cut) jumps; the level stays
// where the gain is not at a limit (no wind-up).
layout(set = 0, binding = 0) uniform sampler2D meter;
layout(set = 0, binding = 1) uniform sampler2D prev;
layout(push_constant) uniform PC {
    vec4 a;   // dt, snap, key, adapt
    vec4 b;   // ev_min, ev_max, tau_up, tau_down
    vec4 c;   // rate_up, 0, 0, 0
    vec4 d;   // grid width, height, 0, 0
} pc;
layout(location = 0) out vec4 o;

void main() {
    vec4 st = texelFetch(prev, ivec2(0), 0);
    float swl = 0.0, sw = 0.0;
    ivec2 n = ivec2(pc.d.xy);
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++) {
            vec2 v = texelFetch(meter, ivec2(x, y), 0).xy;
            swl += v.x;
            sw += v.y;
        }
    float adapted = st.x, dt = pc.a.x;
    float m = sw > 1e-3 ? clamp(swl / sw, -13.0, 0.0) : adapted;
    if (pc.a.y > 0.0) {
        adapted = m;
    } else if (dt > 0.0) {
        float d = m - adapted;
        float s = d * (1.0 - exp(-dt / (d > 0.0 ? pc.b.z : pc.b.w)));
        adapted += min(s, pc.c.x * dt);
    }
    if (pc.a.w > 0.0) adapted = clamp(adapted, pc.a.z - pc.b.y / pc.a.w, pc.a.z - pc.b.x / pc.a.w);
    float cut = pc.a.y > 0.0 ? 1.0 : 0.0;
    o = vec4(adapted, clamp(pc.a.w * (pc.a.z - adapted), pc.b.x, pc.b.y), cut, m);
}
