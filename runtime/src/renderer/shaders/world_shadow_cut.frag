#version 450
// World shadow map, cut-out casters: the draw's stage-0 texel alpha (0-255) against an alpha test
// (D3DCMPFUNC numbering, as geom.frag's), discarded where it fails: holes cast no shadow. Depth only.
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec4 v_t0;
layout(push_constant) uniform PC {
    layout(offset = 48) uint func;
    uint ref;
} pc;
void main() {
    uint a = uint(texture(tex0, v_t0.xy / (v_t0.w != 0.0 ? v_t0.w : 1.0)).a * 255.0 + 0.5), r = pc.ref;
    bool pass;
    switch (pc.func) {
    case 1u: pass = false; break;
    case 2u: pass = a < r; break;
    case 3u: pass = a == r; break;
    case 4u: pass = a <= r; break;
    case 5u: pass = a > r; break;
    case 6u: pass = a != r; break;
    case 7u: pass = a >= r; break;
    default: pass = true; break;
    }
    if (!pass) discard;
}
