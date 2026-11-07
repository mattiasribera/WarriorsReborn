// World lighting (renderer_draw_world_lit): included by geom.frag built with -DCHAR_LIT, after
// char_lit.glsl (the frame's lights, lit.*). A draw in world mode has its light mask in the push
// constant word draw.lights (bit i = light i; 0 = character mode, char_light()); its fragments add
// the masked lights to the vertex diffuse colour before the combiners (world_light_apply), so the
// texture, the combiners and the final combiner's fog mix apply to the lit colour.
// lit.params: z = the world gain, w = the world's Lambert wrap.
vec3 world_light(uint mask) {
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
    vec3 sum = vec3(0.0);
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
        sum += lit.col[i].rgb * (f * lam);
    }
    sum = sum / (1.0 + 0.5 * sum);   // the lamp pass's soft knee
    return sum * lit.params.z;
}
// The lit vertex colour: the light fills what the baked colour leaves (v + L (1 - v)), so dark
// streets gain the most and the baked lamp pools do not wash out.
vec3 world_light_apply(vec3 v0, uint mask) {
    vec3 l = min(world_light(mask), vec3(1.0));
    return v0 + l * (1.0 - v0);
}
