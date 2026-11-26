/* Healed marks for mods: the Lua function ModScar(char_id, level).
 *
 * The title's character draw (0x8fc70) lays a blood texture over a fighter whose health is under
 * 90 %: one of three 64x64 DXT5 atlases "charblood_d1..3_tex" (ResourceMgr [0x4cf0b8] + 0x58..0x60),
 * bound with SetTexture(1, tex) (0x8fe3d, the renderer's cache 0xb8f50 on [0x4cfe8c]) and pixel
 * shader slot 4 instead of 2; the skin turns into the atlas texel by its alpha. The atlas is a sheet
 * of small pieces (bruise blobs, a black eye, slashes, drips); every character model carries its own
 * texture coordinates into it, so each piece lands on the same body part of every model. The no-blood
 * word [[0x5b9f78] + 0x42a] turns it off. The human is [draw + 0x2a8], its CfgChar id at +0xa8.
 *
 * ModScar(char_id, level) gives every character of that id healed marks of `level` (0-1, 0 = none):
 * the same atlas pieces recoloured as old injuries (dark plum bruises, dark red closed cuts, no fresh red;
 * every colour darker than the darkest skin, so a mark never shows as a pale speck), fainter for a lower
 * level, and from level 0.6 a small tan plaster on the cheekbone (atlas texels x 41-47, y 9-14). Fresh blood
 * (health under 90 %) is drawn over the marks; healing takes only the fresh blood away. ModScar()
 * with no arguments removes every mark (a mod calls it at the start of each level).
 *
 * The draw's choice at 0x8fe10-0x8fe46 (no-blood word, health under 90 %, bind the atlas) is replaced
 * by a jump to a stub that asks scars_pick for the texture to bind (0 = none: pixel shader slot 2),
 * written the first time a mark is set; the gore mod's health hook at 0x8fd9c still feeds it. */
#ifndef HLE_SCARS_H
#define HLE_SCARS_H
#include <stdint.h>

#define SCARS_SITE 0x0008fe10u        /* mov eax, [0x5b9f78]: start of the blood-stage choice */
#define SCARS_BLOOD_BACK 0x0008fe42u  /* push 4 (blood pixel shader) */
#define SCARS_NONE_BACK 0x0008fe46u   /* push 2 (plain pixel shader) */
#define SCARS_SETTEX_FN 0x000b8f50u   /* renderer cache SetTexture(stage, tex), thiscall */
#define SCARS_RENDERER 0x004cfe8cu    /* its object */
#define SCARS_RESMGR 0x004cf0b8u      /* ResourceMgr; +0x58 + 4 * i = charblood_d(i+1) */
#define SCARS_GAME 0x005b9f78u        /* [ ] + 0x42a: the no-blood word */
#define SCARS_BLOOD_PCT 0x00420c1cu   /* float 90: blood shows under this health percent */

#define SCARS_CHARS 32                /* character ids with marks at once */
#define SCARS_STEPS 8                 /* levels are drawn in 8 steps */
#define SCARS_TEX 64                  /* atlas size */

/* Marks of a character id: level clamped to 0-1 (0 removes it). Returns 0, or -1 when the table
 * is full or the id is not 1-65535. scars_clear removes all. */
int scars_set(int char_id, double level);
void scars_clear(void);
/* The step (1..SCARS_STEPS) of a character id's marks, 0 without. */
int scars_step(int char_id);

/* One healed texel (B8G8R8A8) from the atlas texel of the game: its alpha scaled by the step,
 * its colour an old injury's (thin strong pieces dark red like a closed cut, soft ones a dark plum
 * bruise), always darker than skin. */
uint32_t scars_heal_texel(uint32_t blood, int step);
/* A over B (both B8G8R8A8, straight alpha). */
uint32_t scars_over(uint32_t a, uint32_t b);
/* The healed atlas of a step: from the game's d1 atlas (step <= 4) or d2 (above), with a plaster
 * on the cheekbone from step 5. src* = the game's atlases decoded, SCARS_TEX^2 texels each. */
void scars_build(const uint32_t *d1, const uint32_t *d2, int step, uint32_t *out);

/* Installs the hook if needed (1 = in place). */
int scars_patch(void);
/* ModScar(char_id, level) / ModScar() for the title's Lua (cdecl C function, registered by hle_mods). */
int __attribute__((cdecl)) hle_scars_lua(uint32_t L);
extern int hle_scars_trace;

#endif
