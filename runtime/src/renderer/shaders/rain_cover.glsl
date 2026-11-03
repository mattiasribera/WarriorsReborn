// --rain-hd-cover (renderer_rain_cover): the rain map and the test whether a point is under cover.
// Included with COVER_SET defined (the descriptor set of the map and its parameters). The map holds,
// seen straight down, the depth of the frame's opaque world's top surface (0 top, 1 bottom; cleared
// to 1 where nothing was drawn).
layout(set = COVER_SET, binding = 0) uniform sampler2D cover_map;
layout(set = COVER_SET, binding = 1) uniform RainCover {
    vec4 row[3];   // right-handed view position (x, y, z, 1) -> map u, v (0-1), depth
    vec4 k;        // on (1: a map this frame), world units per map depth unit, covered above, soft
    vec4 inv;      // 1 / px, 1 / py, map size (texels), 0
} cover;
// 1 under the open sky, 0 under cover: the four nearest map texels (bilinear), each covering the
// point by how far its surface lies above it (smoothstep from k.z to k.z + k.w world units).
float rain_open(vec3 P) {
    if (cover.k.x < 0.5) return 1.0;
    vec4 Q = vec4(P, 1.0);
    vec3 m = vec3(dot(cover.row[0], Q), dot(cover.row[1], Q), dot(cover.row[2], Q));
    if (m.x <= 0.0 || m.y <= 0.0 || m.x >= 1.0 || m.y >= 1.0) return 1.0;
    vec4 g = textureGather(cover_map, m.xy, 0);   // texels (0, 1), (1, 1), (1, 0), (0, 0)
    vec4 c = smoothstep(vec4(cover.k.z), vec4(cover.k.z + cover.k.w), (m.z - g) * cover.k.y) * (1.0 - step(vec4(0.99998), g));
    vec2 f = fract(m.xy * cover.inv.z - 0.5);
    return 1.0 - mix(mix(c.w, c.z, f.x), mix(c.x, c.y, f.x), f.y);
}
