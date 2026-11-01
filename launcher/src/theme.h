/* The launcher's look: a dark Dear ImGui style with one accent colour, rounded corners and
 * roomy spacing, translucent ("frosted") layers over the background art, and the Inter font (embedded in the binary, see third_party/inter) at the
 * sizes the pages use. */
#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H
#include "imgui.h"

namespace launcher {

struct Fonts {
    ImFont *body = nullptr;    /* default text and widgets */
    ImFont *hint = nullptr;    /* hint lines */
    ImFont *nav = nullptr;     /* sidebar entries */
    ImFont *heading = nullptr; /* page and card titles */
    ImFont *title = nullptr;   /* the launcher title */
    ImFont *play = nullptr;    /* the PLAY buttons */
    ImFont *label = nullptr;   /* settings row names */
    ImFont *caps = nullptr;    /* small capitals captions */
    ImFont *hero = nullptr;    /* the title on Home */
};
extern Fonts fonts;

namespace col {
extern const ImVec4 accent, accent_hover, accent_active, accent_text, on_accent, text, dim, nav, good, bad, warn;
/* Translucent layers over the background: cards, the sidebar, pill wells, hover fills, lines. */
extern const ImVec4 card, sidebar, sidebar_solid, well, hover, hover_strong, off, line, shade;
}

/* Builds the font atlas (call before the renderer backend creates its texture) and sets the style.
 * density: the interface scale the fonts are rasterized at (the layout stays in logical pixels;
 * uiscale.h). */
void theme_init(float density = 1);
/* Rebuilds the font atlas at another density (between frames; the renderer backend's font texture
 * must be recreated after it). */
void theme_fonts(float density);

} // namespace launcher
#endif
