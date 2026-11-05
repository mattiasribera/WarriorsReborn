// Hashes and value noise of the procedural sky and skyline (--sky-hd; sky.frag, skyline.frag).
uint sky_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// One hash per lattice point (the coordinates mixed by large odd multipliers first): cheap enough
// for several noise octaves per pixel.
uint sky_hash3(ivec3 p) { return sky_hash(uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(p.z) * 0xcb1ab31fu); }
float sky_u01(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }
// Value noise in 0-1 (smooth, one hashed value per integer lattice point); seed picks the layer.
float sky_noise(vec2 p, int seed) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float a = sky_u01(sky_hash3(ivec3(c, seed))), b = sky_u01(sky_hash3(ivec3(c + ivec2(1, 0), seed)));
    float d = sky_u01(sky_hash3(ivec3(c + ivec2(0, 1), seed))), e = sky_u01(sky_hash3(ivec3(c + ivec2(1, 1), seed)));
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
