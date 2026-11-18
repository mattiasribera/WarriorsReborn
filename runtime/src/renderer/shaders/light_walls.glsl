// The level's wall map (renderer_lamp_wall_map, light-walls.md): per
// texel the lowest and highest point of the walls over it (R16G16_UINT; 0 none, h: world z =
// z0 + (h - 1) zstep). The includer declares the map's usampler2D as LW_MAP first. wall: x0, y0,
// 1 / cell, 1 = loaded; wallz: z0, zstep, the receiver's own-wall gap (world units, 1.5 cells).

// A world point q inside a wall: its cell holds walls whose height span contains q.z. top: the
// walls' top there (world z; 0 outside the map or with no wall).
bool wall_at(vec3 q, vec4 wall, vec4 wallz, ivec2 size, out float top) {
    top = 0.0;
    ivec2 c = ivec2(floor((q.xy - wall.xy) * wall.z));
    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, size))) return false;
    uvec2 bt = texelFetch(LW_MAP, c, 0).rg;
    if (bt.x == 0u) return false;
    top = wallz.x + float(bt.y - 1u) * wallz.y;
    return q.z >= wallz.x + float(bt.x - 1u) * wallz.y && q.z <= top;
}

// The port's lights through walls (RLampLights.walls): at most LW_STEPS_MAX map taps per light, about
// one per cell crossed (a longer way samples coarser). Near the light (LW_NEAR world units,
// horizontally) only walls reaching LW_ABOVE above it block: the lamp's own post or bracket does not.
#define LW_STEPS_MAX 12
#define LW_NEAR 1.0
#define LW_ABOVE 0.25
// The cost cap per pixel: only the lights adding at least LW_SHARE of the strongest one's light there
// (and more than LW_MIN) are tested, at most LW_TESTED of them (in the list's order: the nearest
// and brightest first), so at most LW_TESTED x LW_STEPS_MAX taps; weaker ones pass walls.
#define LW_TESTED 2
#define LW_SHARE 0.25
#define LW_MIN (1.0 / 128.0)
// 1 when the light at l reaches the receiver at p (world, z up) with surface normal n (world; 0 =
// unknown), 0 when a wall of the map stands between them. The way starts 1.5 cells (wallz.z) off the
// receiver's surface along its horizontal normal and is tested only from 1.5 cells away from the
// receiver (its own wall, which fills its cell, does not block its light), up to the light's own
// cell. jit: the steps' offset within their interval (0-1); side: how far the light is moved
// sideways (world units; pixels of a quad take different values, so averaged the wall's shadow edge
// is soft).
float wall_light(vec3 p, vec3 n, vec3 l, float jit, float side, vec4 wall, vec4 wallz) {
    vec3 o = p + vec3(n.xy, 0.0) * wallz.z;
    vec2 d = l.xy - o.xy;
    float horiz = length(d);
    if (horiz < 1e-4) return 1.0;
    l.xy += vec2(-d.y, d.x) * (side / horiz);
    ivec2 size = textureSize(LW_MAP, 0);
    int steps = clamp(int(ceil(horiz * wall.z)), 1, LW_STEPS_MAX);
    for (int k = 0; k < steps; k++) {
        float s = (float(k) + jit) / float(steps);
        float dl = (1.0 - s) * horiz;
        if (dl * wall.z < 1.0) break;   // the light's own cell
        vec3 q = mix(o, l, s);
        if (length(q.xy - p.xy) < wallz.z) continue;
        float top;
        if (wall_at(q, wall, wallz, size, top) && (dl >= LW_NEAR || top >= l.z + LW_ABOVE)) return 0.0;
    }
    return 1.0;
}
