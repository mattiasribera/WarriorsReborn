/* Recognizer for the title's HUD bars (the rage meter under the character name in the player panel,
 * top left, and the target's health bar in the target panel, top right) in recorded draws, and the rect math of the glass effect.
 *
 * How the title draws them: every 2D HUD part is a quad that a widget appends to a per-page,
 * per-layer batch in the HUD manager ([0x4cf0b8], pool of 0x84-byte batches at +0x1844); the
 * batches are sorted and drawn at the end of the frame (0xa50b0 -> 0xc04b0 -> 0xc1d50 -> 0xc19b0,
 * one DrawIndexedVerticesUP per batch, FVF 0x142 = XYZ | DIFFUSE | TEX1, triangle list of quads
 * 0 1 2 2 0 3, SRC_ALPHA / INV_SRC_ALPHA, no depth). A bar is several quads of the HUD atlas page
 * ("part_page0") inside one such batch: an invisible backing quad, two end caps, the dark track
 * (vertex color ff505050 over the atlas part "track"), the fill (the white atlas part tinted
 * with the bar color: white for the rage meter, green for the target's health) and a small colored tick at the
 * left end; other HUD parts on the same page and layer can share the draw. The quad's atlas part
 * is the stable identity (its UV rect is data of the title, independent of texture packs), so a
 * bar is recognized per quad: a quad whose UVs are the track part and whose screen shape is a thin
 * horizontal strip, plus every quad of the same draw lying on that strip (fill, tick, caps). */
#ifndef HUD_GLASS_H
#define HUD_GLASS_H
#include <stdint.h>
#include "renderer.h"

/* UV rect of the bar track part on the HUD atlas page (s0, t0, s1, t1). */
#define HUD_GLASS_TRACK_S0 0.6704f
#define HUD_GLASS_TRACK_T0 0.9087f
#define HUD_GLASS_TRACK_S1 0.6753f
#define HUD_GLASS_TRACK_T1 0.9312f

#define HUD_GLASS_MAX_BARS 8

/* Screen rect in viewport pixels (x0 < x1, y0 < y1). */
typedef struct {
    float x0, y0, x1, y1;
} HudGlassRect;

/* Bars of one recorded draw: v in clip space (RVertex), idx a triangle list into v, vp the draw's
 * viewport. Writes up to max bar rects (union of each bar's quads) and, when member is not NULL,
 * marks every vertex of a bar quad (nv entries): HUD_GLASS_TRACK for the track quad,
 * HUD_GLASS_PART for the others on its strip, 0 elsewhere. Returns the number of bars. A draw
 * that is not made of axis-aligned screen quads has none. */
enum { HUD_GLASS_PART = 1, HUD_GLASS_TRACK = 2 };
int hud_glass_find(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RViewport *vp,
                   HudGlassRect *bars, int max, uint8_t *member);

/* hud_glass_find limited to target bars: the target panel sits in the right half of the screen and
 * the player panels in the left half (the rage meter is never restyled), so a bar counts as a
 * target when its center is right of the viewport's center. */
int hud_glass_find_targets(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RViewport *vp,
                           HudGlassRect *bars, int max, uint8_t *member);

/* `--hud-hd`: restyles the target bars of one recorded draw in place,
 * after the cleaned-up look of the user's reference: the bar grows downward from its top edge to
 * HUD_HD_ASPECT x its width x scale (never thinner than before; the reference's fill is 4.3 % of
 * its width high), the track becomes dark and translucent (HUD_HD_TRACK), the fill keeps its color
 * and width with a vertical gradient (lighter top, darker bottom), and a dark outline quad
 * (HUD_HD_OUTLINE, 1 pixel at 480 lines around the bar, sampling the track part) is drawn first.
 * v must have room for *nv + 4 x HUD_GLASS_MAX_BARS vertices and idx for *ni + 6 x
 * HUD_GLASS_MAX_BARS indices; *nv and *ni are updated. Returns the number of bars restyled (0:
 * nothing changed). */
#define HUD_HD_ASPECT 0.043f
#define HUD_HD_TRACK 0xC0484C50u
#define HUD_HD_OUTLINE 0xE0101010u
int hud_hd_restyle(RVertex *v, uint32_t *nv, uint32_t *idx, uint32_t *ni, const RViewport *vp, float scale, float opacity,
                   HudGlassRect *outlines);
/* opacity (0..1; --hud-hd-opacity): the alpha of the bar's quads (track, fill, tick, caps) is
 * multiplied by opacity and the outline's by sqrt(opacity), so the edge stays visible a little
 * longer than the inside as the bar fades; 1 leaves the alphas as above. outlines (may be NULL;
 * up to HUD_GLASS_MAX_BARS): the screen rect of each restyled bar including its outline, the
 * rect --hud-hd-frost blurs. */

/* Multiplies the alpha of the marked vertices (diffuse color) by opacity (0..1), the track's by
 * half of it (the empty part of the bar reads as glass, the fill stays visible). */
void hud_glass_fade(RVertex *v, uint32_t nv, const uint8_t *member, float opacity);

/* Integer target-pixel box of a bar rect: grown by margin (viewport pixels), scaled by sx, sy
 * (target pixels per viewport pixel), rounded outward and clipped to the target tw x th. w or h
 * is 0 when nothing is left. */
typedef struct {
    int x, y, w, h;
} HudGlassBox;
HudGlassBox hud_glass_box(HudGlassRect r, float margin, float sx, float sy, int tw, int th);

/* Options of the glass effect (`--hud-glass`, off by default). blur: gaussian radius in back-buffer
 * pixels (1..32); opacity: alpha scale of the bar quads (0..1); tint and rim: ARGB8888 color and
 * strength of the frosted tint and of the 1 pixel rim; margin: panel border around the bar, in
 * back-buffer pixels. */
typedef struct {
    int on;
    float blur, opacity;
    uint32_t tint, rim;
    float margin;
    int hd;            /* --hud-hd: hud_hd_restyle on target bars */
    float hd_height;   /* --hud-hd-height: its height scale (0.5..4, default 1) */
    float hd_opacity;  /* --hud-hd-opacity: its opacity (0..1, default 1) */
    float hd_frost;    /* --hud-hd-frost: blur radius behind the bar (0..32, 0 = off) */
    float hd_track_opacity;   /* --hud-hd-track-opacity: the empty part's opacity (0..1; < 0 = hd_opacity) */
    float hd_fill_opacity;    /* --hud-hd-fill-opacity: the fill's opacity (0..1; < 0 = hd_opacity) */
    float hd_vivid;           /* --hud-hd-vivid: color compensation of the translucent fill (0..1, 0 = off) */
    int hd_frame;             /* set by the three above and --hud-hd-glass: glass style (see HudHdStyle) */
} HudGlassConfig;
#define HUD_GLASS_DEFAULTS {0, 4.0f, 0.45f, 0x30ffffffu, 0x70ffffffu, 5.0f, 0, 1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 0}

/* `--hud-hd-glass`: the preset of the glass style. */
#define HUD_HD_GLASS_TRACK_OPACITY 0.25f
#define HUD_HD_GLASS_FILL_OPACITY 0.6f
#define HUD_HD_GLASS_VIVID 1.0f
#define HUD_HD_GLASS_FROST 4.0f

/* Style of hud_hd_restyle_style. frame = 0 is the T4/T5 look (hud_hd_restyle: every strip quad x
 * opacity, filled outline quad under the bar). frame = 1 is the glass style (T7): the empty part
 * of the track (from the fill's right edge on, so it never lies under the fill) turns a light
 * frosted tint (HUD_HD_GLASS_TRACK) x track_opacity, the end caps x track_opacity; the fill keeps
 * the game's hue with a darker bottom, is made vivid (hud_hd_vivid with gain vivid x (1 /
 * fill_opacity - 1), the saturation and brightness the translucency takes away) and drawn x
 * fill_opacity; the outline is a thin frame of 4 quads (1 pixel at 480 lines, HUD_HD_GLASS_OUTLINE)
 * instead of a filled quad, so nothing darkens what shows through the bar. */
typedef struct {
    float height, opacity;
    float track_opacity, fill_opacity;   /* frame = 1 only */
    float vivid;                         /* frame = 1 only */
    int frame;
} HudHdStyle;
#define HUD_HD_GLASS_TRACK 0xC0D0D8E0u
#define HUD_HD_GLASS_OUTLINE 0x90101010u
/* The style of a configuration (track and fill opacity < 0 follow hd_opacity). */
HudHdStyle hud_hd_style(const HudGlassConfig *c);
/* hud_hd_restyle with a style; v must have room for *nv + 16 x HUD_GLASS_MAX_BARS vertices and idx
 * for *ni + 24 x HUD_GLASS_MAX_BARS indices (the frame's 4 quads per bar). */
int hud_hd_restyle_style(RVertex *v, uint32_t *nv, uint32_t *idx, uint32_t *ni, const RViewport *vp, const HudHdStyle *s,
                         HudGlassRect *outlines);
/* Vivid color: ARGB8888 c with its HSV saturation and value multiplied by 1 + gain (capped at 1),
 * hue and alpha kept; gain 0 returns c. */
uint32_t hud_hd_vivid(uint32_t c, float gain);

/* The --hud-hd-frost operation of a restyled bar: exactly its rect (outline included), square
 * corners, no tint, no rim, blur radius `frost`. */
RGlass hud_hd_frost(HudGlassRect outline, float frost);

/* Command-line option at argv[i] = opt with its value val (argv[i + 1], may be NULL):
 * `--hud-glass` turns the effect on; `--hud-glass-blur N`, `--hud-glass-opacity F` and
 * `--hud-glass-tint AARRGGBB` set a tunable and turn it on too; `--hud-hd` turns the target bar
 * restyle on, `--hud-hd-height F`, `--hud-hd-opacity F` (0..1) and `--hud-hd-frost N` (0..32)
 * set its tunables and turn it on. Returns the number of arguments
 * used (1 or 2), 0 when opt is not one of these, -1 for a missing or invalid value (c unchanged). */
int hud_glass_arg(HudGlassConfig *c, const char *opt, const char *val);

/* The glass panel of a bar: the bar rect grown by the margin, pill-shaped (corner radius = half its
 * height), with the configured blur, tint and rim. */
RGlass hud_glass_panel(const HudGlassConfig *c, HudGlassRect bar);

#endif
