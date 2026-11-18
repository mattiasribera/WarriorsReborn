// World lighting (renderer_draw_world_lit): included by geom.frag built with -DCHAR_LIT, after
// char_lit.glsl (the frame's lights, lit.*). A draw in world mode has its light mask in the push
// constant word draw.lights (bit i = light i; 0 = character mode, char_light()); its fragments add
// the masked lights to the vertex diffuse colour before the combiners (world_light_apply), so the
// texture, the combiners and the final combiner's fog mix apply to the lit colour.
// lit.params: z = the world gain, w = the world's Lambert wrap.
vec3 world_light(uint mask, out vec3 walled) {
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
#ifdef RELIEF
    n = r_n;   // --normal-maps: the relief's shading normal (geom.frag relief_setup)
#endif
    vec3 sum = vec3(0.0);
    walled = vec3(0.0);
    // Light walls (lit.wall.w): each pixel of a 2 x 2 quad marches with its own step offset and
    // sideways shift of the light (a quarter cell); geom.frag main averages the quad.
    bool walls = lit.wall.w > 0.5;
    vec3 pw = vec3(0.0), nw = vec3(0.0);
    float jit = 0.5, side = 0.0;
    if (walls) {
        pw = lw_world(v_view, 1.0);
        nw = lw_world(n, 0.0);
        uvec2 qp = uvec2(gl_FragCoord.xy) & 1u;
        jit = (float(qp.x + 2u * qp.y) + 0.5) * 0.25;
        side = (float(qp.x ^ qp.y) - 0.5) * 0.5 / lit.wall.z;
    }
    float top = 0.0;
    const uint all = mask;
    while (mask != 0u) {
        int i = findLSB(mask);
        mask &= mask - 1u;
        vec4 lr = lit.pos_r[i];
        vec3 v = lr.xyz - v_view;
        float d2 = dot(v, v), r2 = lr.w * lr.w;
        if (d2 >= r2) continue;
        float x2 = d2 / r2, a = 1.0 - x2;
        float f = a * a / (1.0 + lit.params.x * x2);   // lamp_lights.h lamp_falloff
        float ndl = dot(n, v * inversesqrt(max(d2, 1e-8)));
        float lam = clamp((ndl + lit.params.w) / (1.0 + lit.params.w), 0.0, 1.0);
        vec3 c = lit.col[i].rgb * (f * lam);
        sum += c;
        top = max(top, max(c.r, max(c.g, c.b)));
    }
    walled = sum;
    if (walls) {   // again, the strongest lights only (LW_TESTED): what the walls take away
        float least = max(LW_MIN, LW_SHARE * top);
        int tested = 0;
        mask = all;
        while (mask != 0u && tested < LW_TESTED) {
            int i = findLSB(mask);
            mask &= mask - 1u;
            vec4 lr = lit.pos_r[i];
            vec3 v = lr.xyz - v_view;
            float d2 = dot(v, v), r2 = lr.w * lr.w;
            if (d2 >= r2) continue;
            float x2 = d2 / r2, a = 1.0 - x2;
            float f = a * a / (1.0 + lit.params.x * x2);
            float ndl = dot(n, v * inversesqrt(max(d2, 1e-8)));
            float lam = clamp((ndl + lit.params.w) / (1.0 + lit.params.w), 0.0, 1.0);
            vec3 c = lit.col[i].rgb * (f * lam);
            if (max(c.r, max(c.g, c.b)) < least) continue;
            tested++;
            walled -= c * (1.0 - lw_light(i, pw, nw, jit, side));
        }
        walled = max(walled, vec3(0.0));
    }
    return sum;   // before the knee (world_light_knee)
}
// The lamp pass's soft knee, then the world gain.
vec3 world_light_knee(vec3 sum) {
    sum = sum / (1.0 + 0.5 * sum);
    return sum * lit.params.z;
}
// The lit vertex colour: the light L (world_light_knee) fills what the baked colour leaves
// (v + L (1 - v)), so dark streets gain the most and the baked lamp pools do not wash out.
vec3 world_light_apply(vec3 v0, vec3 L) {
    vec3 l = min(L, vec3(1.0));
    return v0 + l * (1.0 - v0);
}
