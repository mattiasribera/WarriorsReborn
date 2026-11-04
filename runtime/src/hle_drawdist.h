/* Draw distance: the title's far plane (= fog end) and its small-prop cull, patched at start-up.
 *
 * The title has no mesh LOD. What reaches the screen is limited by two things:
 *
 * - The far plane. The scene render function 0x32a470 moves it every frame toward the sector
 *   streamer's target, capped at 1.5 x the level's Lua value (CameraSetClipping, mostly 90) and
 *   at 300. When the title's frame-rate meter [0x4b062c] reads <= 29.5 fps it instead pulls the
 *   far plane in by (29.5 - fps) x dt x 10.5 (a single 100 ms hitch: -20 units, regrowth 10.5
 *   units/s). On this port host hitches feed that meter, so the fog "breathes".
 * - Small props: a prop of radius r at squared distance d2 is not drawn when r / d2 < 0.0004
 *   ([0x4212d4]) and fades in up to 0.0005 ([0x4212d8]) with alpha (x - 0.0004) x [0x42142c].
 *
 * Patches (each byte-checked, a mismatch writes nothing of that patch and is logged):
 * - steady fog (default on, --fog-breathing keeps the title's): the jbe at 0x32a5e7 that takes
 *   the low-fps branch becomes two nops, so the far plane always follows the streaming target.
 * - --draw-distance N (1-4, default 1 = no write): the mulss operand at 0x32a514 (1.5) and the
 *   movss operand at 0x32a651 (300) point at runtime floats 1.5 x N and 300 x N. Both or none.
 * - --prop-detail N (1-8, default 1 = no write): the cull and fade thresholds become
 *   0.0004 / N^2 and 0.0005 / N^2 and the fade scale x N^2 (a full fade over the narrower band),
 *   so small props are drawn N times as far. The three constants are used only by the prop
 *   draw 0x9d740. All or none.
 * Cutscene and scripted cameras (camera type 10) keep the Lua far plane as before. */
#ifndef HLE_DRAWDIST_H
#define HLE_DRAWDIST_H
#include "hle_ratefix.h"

#define DRAWDIST_MIN 1.0f
#define DRAWDIST_MAX 4.0f
#define PROPDETAIL_MIN 1.0f
#define PROPDETAIL_MAX 8.0f

typedef struct {
    int steady_fog;        /* 1: the low-fps far-plane shrink is skipped (default) */
    float draw_distance;   /* far-plane cap multiplier, 1 = as shipped */
    float prop_detail;     /* small-prop distance multiplier, 1 = as shipped */
} DrawDistOptions;

#define DRAWDIST_DEFAULTS {1, 1.0f, 1.0f}

/* 1 when s is a number in lo..hi (whole string, finite; sets *out), 0 otherwise. */
int drawdist_parse_factor(const char *s, float lo, float hi, float *out);

/* Consumes argv[*i] (and its value) when it is a draw-distance option, advancing *i. Returns 1
 * when consumed, 0 when not ours, -1 for a missing or out-of-range value. */
int drawdist_parse_arg(int argc, char **argv, int *i, DrawDistOptions *o);

/* Applies the options to memory where guest address va is at host address va + bias (bias 0:
 * the identity-mapped guest) and writes one "[drawdist] ..." line (no newline) into report.
 * Returns the number of patches applied (steady fog, draw distance, prop detail: 0-3). */
int drawdist_install_at(const DrawDistOptions *o, intptr_t bias, char *report, size_t cap);

/* The floats the far-plane operands point at once --draw-distance is applied (for the test). */
extern float drawdist_cap_scale, drawdist_cap_max;

/* drawdist_install_at on the loaded title, printing the line. */
int hle_drawdist_install(const DrawDistOptions *o);
#endif
