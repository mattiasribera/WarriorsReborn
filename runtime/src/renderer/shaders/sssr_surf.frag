#version 450
// --reflections sssr's surface pass (vk_renderer.cpp surf_record): where a
// draw's specular map holds a material (--dry-highlights: B shine above 0, A gloss; tools/
// material_shine.py), its face normal (view space, from the view position's derivatives, turned
// towards the camera; octahedral, 0-1) and the material's shine and gloss; elsewhere nothing (the
// texel is discarded: no depth either). Read by wet.frag (bindings 8 and 9).
layout(set = 0, binding = 1) uniform sampler2D spec_map;
layout(location = 0) in vec3 v_view;
layout(location = 1) in vec4 v_t;
layout(location = 0) out vec4 o;
vec2 oct_encode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
}
void main() {
    vec2 uv = v_t.xy / (v_t.w != 0.0 ? v_t.w : 1.0);
    vec4 sm = texture(spec_map, uv);
    vec3 n = cross(dFdx(v_view), dFdy(v_view));
    float nn = dot(n, n);
    if (sm.b <= 0.0 || !(nn > 1e-30)) discard;
    n *= inversesqrt(nn);
    if (dot(n, v_view) > 0.0) n = -n;   // towards the camera
    o = vec4(oct_encode(n) * 0.5 + 0.5, sm.b, sm.a);
}
