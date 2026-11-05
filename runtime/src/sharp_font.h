/* Sharp fonts (on by default, `--no-sharp-fonts` turns them off): the
 * title's two font atlases, upscaled 4x from the user's own copy of the game when they are first
 * drawn, so the HUD, menu, objective and subtitle letters stay crisp at 1080p and above.
 *
 * The title draws every character as one quad with the cell's normalized UV rect, tinted by the
 * vertex color, so the atlas can be replaced by a larger image without touching the layout. Its
 * letters are white ink with an anti-aliased alpha edge; bilinear magnification of the 512x512
 * (main font and HUD sheet, "part_page0") and 1024x512 (big font) DXT5 images turns those edges
 * soft and blocky. sharp_font_upscale rebuilds the edge at 4x: a Catmull-Rom interpolation of the
 * alpha is cut at half the local ink level with a ramp about one output texel wide, so the contour
 * follows the original anti-aliasing (an implicit distance field) instead of the texel grid.
 *
 * Only letter ink is rebuilt. A texel counts as ink when the texels around it are near white and
 * their alpha changes steeply (an anti-aliased glyph edge). Coloured or grey art (the button
 * icons, portraits, HUD markers) and soft white glows fail that test and are upscaled bilinearly,
 * exactly as the GPU would sample the original, so they look as before. The weight between the
 * two is interpolated, so there are no seams. Nothing game-derived is stored: the input is the
 * guest's own decoded texture and the output lives only in memory.
 *
 * Game-independent and thread-safe (tested by tools/sharp_font_test.c). Images are B8G8R8A8
 * texels (uint32_t 0xAARRGGBB). */
#ifndef SHARP_FONT_H
#define SHARP_FONT_H
#include <stdint.h>

#define SHARP_FONT_SCALE 4
#define SHARP_FONT_MAIN_HASH 0x886a4c14094e9597ull   /* part_page0: main font, button icons, HUD sheet */
#define SHARP_FONT_BIG_HASH 0x912d0ecc2ada0599ull    /* big slab-serif font */

/* 1 when the texture-pack content hash is one of the font atlases above. */
int sharp_font_atlas(uint64_t hash);

/* The w x h image upscaled by scale (1..8): a new (w * scale) x (h * scale) image (free() it), or
 * NULL when an argument is invalid or memory runs out. Letter ink gets the sharp edge, every other
 * texel the bilinear value the GPU would sample at the same place (clamped at the image border). */
uint32_t *sharp_font_upscale(const uint32_t *bgra, uint32_t w, uint32_t h, int scale);

/* Weight of the sharp edge at input texel (x, y), 0..1 (0: bilinear, 1: rebuilt edge); exposed for
 * the tests. */
float sharp_font_ink(const uint32_t *bgra, uint32_t w, uint32_t h, int x, int y);

/* Command-line option: `--sharp-fonts` sets *on, `--no-sharp-fonts` clears it. Returns 1 when opt
 * is one of them, 0 otherwise. */
int sharp_font_arg(int *on, const char *opt);
#endif
