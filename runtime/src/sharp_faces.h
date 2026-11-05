/* --sharp-faces: character skins (face and body share one texture) are
 * upscaled 2x at runtime from the game's own texels, so faces read sharper in close-ups and
 * cutscenes. Nothing game-derived ships: each player's copy is filtered when a skin is first drawn.
 *
 * The rule (sharp_faces_applies): stage 0 of a draw by a character's vertex program, a DXT1
 * texture whose sides are powers of two from SHARP_FACES_MIN to SHARP_FACES_MAX texels, and only
 * when the texture comes from the guest: a texture pack or built-in replacement for that hash always wins. Study:
 * 410 of 514 skins are 256x256, 56 128x128, 47 512x512 (the main cast, the most seen in close-ups).
 *
 * The filter (sharp_faces_upscale), all integer, deterministic and game-independent:
 *   1. DXT1 deblock: across each 4x4 block edge a small step whose neighbours are flat on both
 *      sides (an encoding seam, not painted detail) is smoothed; real edges are left alone.
 *   2. 2x Catmull-Rom (fixed phases 1/4 and 3/4), each output clamped to the 2x2 source texels
 *      around it, so edges get no ringing or halos.
 *   3. Mild clamped unsharp mask (3x3), again limited to the local 3x3 range.
 * Alpha uses step 2 only; when the source alpha is binary (DXT1 punch-through) it stays binary.
 * Host thread only; no shared state. Tested by tests/sharp_faces_test.c. */
#ifndef SHARP_FACES_H
#define SHARP_FACES_H
#include <stdint.h>

typedef struct {
    int on;   /* --sharp-faces / --no-sharp-faces */
} SharpFacesConfig;
#define SHARP_FACES_DEFAULTS {0}
/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1), 0 when
 * opt is not a sharp-faces option. */
int sharp_faces_arg(SharpFacesConfig *c, const char *opt, const char *val);

#define SHARP_FACES_DXT1 0x0Cu
#define SHARP_FACES_MIN 64u
#define SHARP_FACES_MAX 512u
/* Skins upscaled per presented frame at most (the rest keep the guest texture until a later frame). */
#define SHARP_FACES_PER_FRAME 1

/* 1 when a stage texture should be upscaled: on, a character draw, stage 0, from_guest (no pack or
 * built-in replacement), DXT1 (format code) with both sides powers of two in [MIN, MAX]. */
int sharp_faces_applies(const SharpFacesConfig *c, int character, int stage, int from_guest, uint32_t code, uint32_t w, uint32_t h);

/* The 2w x 2h upscale of w x h texels (0xAARRGGBB, row-major; w, h >= 4). Malloc'd, NULL on
 * allocation failure. */
uint32_t *sharp_faces_upscale(const uint32_t *bgra, uint32_t w, uint32_t h);
#endif
