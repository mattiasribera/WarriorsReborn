/* Photo mode in the running game: F9, or Back + RB held half a second on
 * a controller, freezes the game and frees the camera; a photo is accumulated from many jittered
 * replays of the frozen frame, supersampled in tiles, finished with the photo adjustments and saved
 * as a PNG in the Photos folder next to the game. Leaving restores every setting it changed.
 *
 * Pieces: src/photo.h (the pure math, capture plan, accumulation, look, PNG, settings snapshot),
 * src/hle_d3d.c (hle_d3d_photo_*: the guest frozen at Swap, the frozen frame replayed with the
 * photo camera), this file (keys, pad, panel, quality, capture, files) and gamectl's `photo`
 * command (src/gamectl.c) for headless runs.
 *
 * Controls while in photo mode (pad; the keyboard reaches them through its pad bindings):
 *   left stick move, right stick look, LT / RT down / up, LB / RB roll, d-pad up / down zoom,
 *   A take the photo, X hide / show the characters, Y show / hide the adjustments panel (open on
 *   entry; d-pad: choose and change, A on "Take photo"; Y or B hides it, then a 2 s "Y (E): show
 *   panel" hint fades and nothing else is drawn), L3 held: move faster, R3: reset the camera,
 *   B or Back (panel hidden): leave. Keyboard defaults: E is Y, F is B, Backspace is Back;
 *   F9 or Escape leave, Enter in the panel as A. */
#ifndef HLE_PHOTO_H
#define HLE_PHOTO_H
#include <stddef.h>

/* --photo-dir DIR: where photos are saved (default: <program folder>/Photos). */
extern const char *hle_photo_dir;
/* --photo-prepare / --no-photo-prepare (default on): the renderer also builds the passes photo
 * mode turns on (ambient occlusion with bounce light, contact shadows, volumetric fog, sun
 * shadows) when the player has them off, at their highest quality; gameplay leaves them off (no
 * per-frame cost; some video memory). Read by hle_d3d.c's renderer_open. */
extern int hle_photo_prepare;

/* Installs the keys, the pad combo, the panel and the replay hooks. lang: "es" for Spanish texts,
 * anything else English. */
void hle_photo_install(const char *lang);

/* The control socket's `photo` command (any thread; blocks until done or a timeout):
 *   photo enter | exit | status
 *   photo capture [PATH.png]           a photo (default: the Photos folder) with the current settings
 *   photo set KEY VALUE                resolution native|2x|4k, ss 1|2|4, samples N, exposure F, filter NAME,
 *                                      saturation F, vignette F, grain F, bars none|1.85|2.39, focus F,
 *                                      aperture F, characters on|off
 *   photo move RIGHT UP FORWARD [YAW PITCH ROLL ZOOM]   camera input held for one second of replays
 * Writes one JSON line into reply (as gamectl's replies) and returns its length. */
size_t hle_photo_command(const char *args, char *reply, size_t cap);
/* 1 while photo mode holds the game (gamectl's watchdog: no Swap is expected meanwhile). */
int hle_photo_active(void);

#endif
