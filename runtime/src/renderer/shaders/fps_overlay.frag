#version 450
// --fps-overlay: the frame counter box, drawn last on the presented image
// (swapchain or read-back frame) with glass.vert and the scissor set to the box. Outlined text, no
// background box, in the built-in 5x7 font (src/fps_font.inc, also read by
// src/fps_overlay.c: fps_overlay_ink is the CPU reference of this lookup). Blend: SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA on the color channels.
#extension GL_GOOGLE_include_directive : require

const int FONT_W = 5, FONT_H = 7, ADVANCE = 6, PAD = 2;   // fps_overlay.h
const uint FONT[44] = uint[](   // FPS_FONT_GLYPHS * 2
#include "../../fps_font.inc"
);

layout(push_constant) uniform Push {
    ivec4 box;      // x, y, w, h in target pixels (y down)
    int scale;      // target pixels per font pixel
    int n;          // characters
    uint text[10];  // glyph indices, 4 per word (low byte first): FPS_MAX_CHARS
} p;

layout(location = 0) out vec4 color;

bool ink(ivec2 f) {
    if (f.x < 0 || f.y < 0 || f.y >= FONT_H) return false;
    int c = f.x / ADVANCE, x = f.x - c * ADVANCE;
    if (c >= p.n || x >= FONT_W) return false;
    uint g = (p.text[c >> 2] >> (8 * (c & 3))) & 0xFFu;
    uint row = (FONT[g * 2u + uint(f.y >> 2)] >> (8 * (f.y & 3))) & 0x1Fu;
    return ((row >> (FONT_W - 1 - x)) & 1u) != 0u;
}

void main() {
    // No box behind the text (it hid part of the game): white text with a one-font-pixel dark
    // outline, so it stays readable on bright and dark scenes alike.
    ivec2 f = (ivec2(gl_FragCoord.xy) - p.box.xy) / p.scale - ivec2(PAD);
    if (ink(f)) {
        color = vec4(1.0, 1.0, 1.0, 0.95);
        return;
    }
    bool edge = false;
    for (int dy = -1; dy <= 1 && !edge; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (ink(f + ivec2(dx, dy))) { edge = true; break; }
    color = edge ? vec4(0.0, 0.0, 0.0, 0.75) : vec4(0.0);
}
