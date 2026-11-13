/* Texture replay race (the [tex-race] log, --no-tex-race-guard). A draw records only its stage
 * textures' headers and a sparse key of their bytes (stage_texture in src/hle_d3d.c); the host
 * thread reads the texels later, when it replays the frame, while the title already records the
 * next one. When the title reuses that memory in between (streaming), the replay would decode
 * other bytes and cache them under the recorded key for good. At replay, before anything read
 * from guest memory is cached by that key, the key is computed again from the bytes now there:
 * a mismatch is logged (rate-limited) and, with the guard on (the default), nothing is cached
 * under the recorded key: the first mismatch of a texture address skips the draw for that frame
 * (the next frame records and decodes it again); when the same address mismatches again on a
 * later frame (a texture rewritten in place every frame), the draw uses the bytes there now,
 * under the key of those bytes (tex_race_repeat).
 * Game-independent (tested by tests/tex_race_test.c). */
#ifndef TEX_RACE_H
#define TEX_RACE_H
#include <stdint.h>

/* The record-time key of a texture: xtex_key over its Data address, Format and Size dwords and
 * its len bytes (all levels), never 0. */
uint64_t tex_race_key(uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes, uint32_t len);
/* Whether the texture's bytes still give the recorded key: 0 yes, 1 changed since record time, -1
 * when Format/Size do not parse (xtex_info). Changes the sparse key does not sample go unseen, as
 * they do at record time. */
int tex_race_changed(uint64_t rec_key, uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes);

/* Log rate limit: at most TEX_RACE_LOG_BURST lines per TEX_RACE_LOG_WINDOW replayed frames. */
#define TEX_RACE_LOG_BURST 8
#define TEX_RACE_LOG_WINDOW 600
typedef struct {
    uint64_t checks, races, check_bytes;   /* replay checks, changed textures among them, texture bytes checked */
    uint32_t window, logged, suppressed;   /* current log window (frame / TEX_RACE_LOG_WINDOW + 1), lines in it, lines not printed */
} TexRaceStats;
/* Counts one check of a len-byte texture at replayed frame `frame`; changed: its outcome. Returns
 * 1 when the change should be logged. */
int tex_race_note(TexRaceStats *s, int changed, uint32_t len, uint32_t frame);

/* Skip once, then draw: per texture address (a small direct-mapped table), the frame of its first
 * mismatch and of its latest one. */
#define TEX_RACE_RECENT 64
#define TEX_RACE_REPEAT_FRAMES 8
typedef struct {
    uint32_t addr[TEX_RACE_RECENT], first[TEX_RACE_RECENT], last[TEX_RACE_RECENT];
} TexRaceRecent;
/* Notes a mismatch of the texture at addr in replayed frame `frame`. Returns 1 when that address
 * already mismatched on an earlier frame and last did at most TEX_RACE_REPEAT_FRAMES frames ago
 * (draw it with the current bytes), 0 for a first mismatch (skip the draw; every draw of it in
 * that frame is skipped alike). */
int tex_race_repeat(TexRaceRecent *r, uint32_t addr, uint32_t frame);

/* --no-tex-race-guard: 0 (changes are still detected and logged, but cached as before). */
extern int tex_race_guard;

/* Test hooks (src/hle_d3d.c, used by tools/hle_d3d_test.c): records stage 0's bound texture as a
 * draw would (returns its key, 0 when not recordable), and replays that draw's stage 0 texture
 * lookup on the calling thread as replayed frame `frame` (returns 1 when the guard skipped the
 * draw; *drawn_key: the key the texture was looked up under, the recorded one or, drawn with
 * the current bytes, theirs). */
uint64_t hle_d3d_tex_race_record(void);
int hle_d3d_tex_race_replay(uint32_t frame, uint64_t *drawn_key);
/* Replay checks and detected changes so far. */
void hle_d3d_tex_race_counts(uint64_t *checks, uint64_t *races);
#endif
