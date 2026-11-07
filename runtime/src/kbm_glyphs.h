/* Keyboard and mouse button icons for the game's font/icon atlas (texture-pack key
 * 886a4c14094e9597).
 *
 * The game draws its button prompts as characters of the main font atlas: the rect of character
 * code ch is record ch + 0x5e of a table of float UV rects {u0, v0, u1, v1} in paks\global.pak.
 * This module reads that table from the user's own disc at start-up and repaints the button
 * cells of an atlas image (the guest's own texture or a pack's replacement, at any scale) with
 * key caps and mouse drawings made by this code for the keys bound to each pad control. Nothing
 * game-derived is stored: only character codes and record indices.
 *
 * Game-independent and not thread-safe (tested by tools/kbm_glyphs_test.c). Images are
 * B8G8R8A8 texels (uint32_t 0xAARRGGBB). Key codes are the input_map.h ones (lower-case ASCII,
 * INPUT_KEY_*, INPUT_MOUSE_*). */
#ifndef KBM_GLYPHS_H
#define KBM_GLYPHS_H
#include <stddef.h>
#include <stdint.h>

#define KBM_ATLAS_HASH 0x886a4c14094e9597ull
#define KBM_RECT_BASE 0x5e          /* record index of character code 0 */
#define KBM_RECTS_MAX 0x1000
#define KBM_PAK_NAME_HASH 0x4cfc9c5cu   /* XBoxWad.idx name hash of paks\global.pak */
#define KBM_RED 0xffc8102eu         /* Warriors red, the bound mouse button */

typedef struct { float u0, v0, u1, v1; } KbmRect;

/* The glyph rect table in global.pak bytes: a 16-byte header {count, 0x5e, 0, 0} followed by
 * count records inside [0, 1] (the known offset 0x861f4 first, else a scan). Returns the record
 * count (at most max are copied) or -1 when there is none. */
int kbm_rects_parse(const uint8_t *pak, size_t n, KbmRect *out, int max);
/* The same from a game folder: XBoxWad.idx -> the WAD holding global.pak -> the table. */
int kbm_rects_load(const char *disc_dir, KbmRect *out, int max);

/* Button cells: a character code's record (or a large face sprite's record) and the pad control
 * (IN_*) whose binding it shows. */
typedef struct { int record, control; const char *what; } KbmCell;
int kbm_cell_count(void);
const KbmCell *kbm_cell(int i);

/* Key cap label of a key: "E", "SHIFT", "SPACE"..., or the short form ("SHF", "SPC") when
 * short_form is set; arrows are the control characters 1-4 (drawn as arrows). NULL for a mouse
 * button or an unknown key. */
const char *kbm_key_label(int key, int short_form);

/* Keys whose names are long draw a symbol on the key cap instead of a label (both Shift keys the
 * same): Shift a hollow up arrow, Enter a down-then-left arrow, Tab an arrow to a bar, Backspace a
 * left-pointing key with a cross, Space a bracket. KBM_SYM_NONE for the others (labels). */
enum { KBM_SYM_NONE, KBM_SYM_SHIFT, KBM_SYM_ENTER, KBM_SYM_TAB, KBM_SYM_BACKSPACE, KBM_SYM_SPACE };
int kbm_key_symbol(int key);
extern int kbm_key_symbols;   /* 1 (default): draw the symbols; 0: labels (tests compare both) */

/* Repaints every button cell whose control has a key (keys[control] >= 0; IN_COUNT entries) with
 * that key's icon, centred in the cell and placed in the vertical band the base's own face
 * buttons occupy, so the text layout does not change. Cells of unbound controls keep the base's
 * art. Returns the number of cells painted. */
int kbm_paint_atlas(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const int *keys);

/* One icon into a w x h image (cleared to transparent first), in the rows band_top..band_bottom
 * (fractions of h). */
void kbm_draw_icon(uint32_t *bgra, int w, int h, int stride, int key, float band_top, float band_bottom);
/* The image kbm_paint_atlas works on for a draw of the atlas: the pack's replacement when one was
 * read (pack_image), else the --sharp-fonts upscale (sharp_state: 0 not started, 1 working,
 * 2 ready, 3 uploaded, 4 failed) while it is pending or ready, else the game's own atlas. An
 * atlas a pack replaces (pack_replaces) is never upscaled, so an unreadable replacement falls back
 * to the game's atlas at once instead of waiting for an upscale that never starts. KBM_BASE_WAIT:
 * keep the Xbox icons this frame and ask again. */
enum { KBM_BASE_PACK, KBM_BASE_SHARP, KBM_BASE_WAIT, KBM_BASE_GAME };
int kbm_atlas_base(int pack_image, int pack_replaces, int sharp_fonts, int sharp_state);
#endif
