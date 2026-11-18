// Character lighting (renderer_char_lights, renderer_draw_lit): shared by geom.vert and geom.frag
// built with -DCHAR_LIT. Set 1, binding 0: the frame's lights (std140; vk_renderer.cpp CharLitUbo).
#define CHAR_LIT_MAX 32
layout(set = 1, binding = 0) uniform CharLights {
    vec4 cam;                    // 1 / px, 1 / py, lights in use, character gain
    vec4 params;                 // falloff k (lamp_lights.h LAMP_FALLOFF_K), Lambert wrap, -, -
    vec4 pos_r[CHAR_LIT_MAX];    // right-handed view position, radius (view units)
    vec4 col[CHAR_LIT_MAX];      // colour x intensity x strength
    vec4 opts;                   // x: 1 = the title's shadow darkens the added light too (geom.frag main); y: --parallax depth
    vec4 wto[3];                 // light walls: rows, world = wto x (view, 1)
    vec4 wall;                   // the wall map: x0, y0, 1 / cell, 1 = test the walls (RLampLights.walls)
    vec4 wallz;                  // z0, zstep, the receiver's own-wall gap
#if defined(HAIR) || defined(SKIN)
    vec4 hair;                   // --hair: world up in view space (unit), strength (RendererConfig.hair)
#endif
#ifdef SKIN
    vec4 skin;                   // --skin: strength (RendererConfig.skin); --eyes, --cloth strengths (MAT variants), -
#endif
} lit;
