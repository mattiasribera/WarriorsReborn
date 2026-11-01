/* High-level emulation (HLE) of the audio API boundary: the title-facing XACT engine and
 * DirectSound entry points are replaced with host functions (DirectSound buffers and XACT cues
 * played by the host mixer in audio.c), so the statically linked XACTENG/DSOUND code never
 * touches MCPX APU/AC97 MMIO. */
#ifndef HLE_AUDIO_H
#define HLE_AUDIO_H
#include <stdint.h>

typedef uint32_t (*HleLookup)(const char *name);   /* symbol -> address, 0 if unknown */

/* Patches every known entry point with a jmp to a host stub. An entry is patched only when its
 * first bytes match the code this table was derived from (otherwise it is reported and skipped).
 * lookup may be NULL; when given, a symbol that resolves to a different address is skipped too.
 * Returns the number of patched entries. */
int hle_audio_install(HleLookup lookup);
void hle_audio_summary(void);          /* one line: total HLE calls and per-function counts */
extern int hle_audio_log_limit;        /* log lines per function (default 32, 0 = silent) */
/* --sfx-3d-volume: extra level in dB on every 3D (positioned) XACT cue, default 0. 2D cues,
 * DirectSound buffers (Bink, streams) are unchanged. parse accepts a decimal number in
 * HLE_AUDIO_SFX3D_MIN_DB..HLE_AUDIO_SFX3D_MAX_DB and returns 0, else -1 (*db untouched). */
#define HLE_AUDIO_SFX3D_MIN_DB (-40.0f)
#define HLE_AUDIO_SFX3D_MAX_DB 12.0f
int hle_audio_parse_db(const char *s, float *db);
void hle_audio_set_sfx_3d_volume(float db);
float hle_audio_sfx_3d_gain(void);   /* linear */
/* --no-xact-controls: 0 plays every cue at its sound entry volume, ignoring the parameter
 * controls ("V", "F") the title sets in Prepare/PlayEx and SetParameterControl (default 1). */
void hle_audio_set_xact_controls(int on);
int hle_audio_xact_controls(void);
/* Host file descriptor behind a guest file handle (for streamed wave banks), -1 if none. */
extern int (*hle_audio_handle_fd)(uint32_t handle);

/* Table access for tests. */
int hle_audio_entry_count(void);
uint32_t hle_audio_entry_addr(int i);
int hle_audio_entry_argbytes(int i);
const char *hle_audio_entry_name(int i);
const uint8_t *hle_audio_entry_sig(int i);   /* HLE_SIG_LEN bytes */
#define HLE_SIG_LEN 5
#endif
