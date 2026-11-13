#version 450
// The title's gamma ramp (--gamma-ramp, src/gamma_ramp.h) on the finished frame: each colour
// channel of the 8-bit pixel through its 256-entry table, as the Xbox does on scanout. Entry i of
// the table is red | green << 8 | blue << 16 (gamma_ramp_lut), four entries per uvec4. The source
// is the B8G8R8A8_UNORM frame, so a channel read back as i / 255 indexes entry i exactly, and the
// result (an integer / 255) is written back exactly: gamma_ramp_apply is the CPU reference.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform Lut { uvec4 e[64]; } lut;
layout(location = 0) out vec4 o;

uint entry(uint i) { return lut.e[i >> 2][i & 3u]; }

void main() {
    vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
    uvec3 i = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);
    uint r = entry(i.r) & 0xffu, g = entry(i.g) >> 8 & 0xffu, b = entry(i.b) >> 16 & 0xffu;
    o = vec4(vec3(r, g, b) / 255.0, c.a);
}
