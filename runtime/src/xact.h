/* XACT 1 (original Xbox) data formats: wave banks (.xwb, "WBND" version 3) and sound banks
 * (.xsb, "SDBK" version 11), plus a wave reader that decodes PCM and Xbox ADPCM from an
 * in-memory or a streamed (file) wave bank.
 *
 * References: vgmstream src/meta/xwb.c (WAVEBANKHEADER segments, XACT 1.1 entry and format
 * bits) and src/meta/xwb_xsb.h (XACT 1.2 sound bank header, cue index and entry tables);
 * Cxbx-Reloaded src/core/hle/XACTENG/XactEng.h (streaming parameters); the sound entry fields
 * (volume, track table, events, variation tables) follow this XBE's own
 * IXACTSoundBank_GetSoundCueProperties internals (0x3d12a7). */
#ifndef XACT_H
#define XACT_H
#include <stddef.h>
#include <stdint.h>
#include "audio.h"

enum { XWB_PCM = 0, XWB_XADPCM = 1, XWB_WMA = 2 };

typedef struct {
    int codec, channels, bits;   /* bits only for PCM (8 or 16) */
    uint32_t rate;
    uint32_t offset, length;     /* bytes, relative to the wave data segment */
    uint32_t loop_start, loop_length;   /* bytes (0 length = no loop region) */
} XwbEntry;

typedef struct {
    char name[17];
    uint32_t flags;              /* bit 0: streaming bank */
    int count;
    XwbEntry *entries;
    const uint8_t *mem;          /* in-memory bank: start of the wave data segment */
    int fd;                      /* streamed bank: host file (-1 for in-memory) */
    uint64_t data_offset;        /* streamed bank: file offset of the wave data segment */
} XwbBank;

/* Parses a wave bank header and entry table from buf (len bytes from the bank's first byte).
 * Returns 0, or -1 with a message in err. needed (may be NULL) receives the number of bytes
 * the header and entry table occupy, so a caller can read more and retry. */
int xwb_parse(const uint8_t *buf, size_t len, XwbBank *wb, size_t *needed, char *err, size_t errlen);
void xwb_free(XwbBank *wb);
/* Opens a streamed wave bank whose header starts at file offset off. */
int xwb_open_stream(int fd, uint64_t off, XwbBank *wb, char *err, size_t errlen);
uint32_t xwb_entry_frames(const XwbEntry *e);

typedef struct {
    const uint8_t *d;
    size_t len;
    int cues, entries, banks;
    uint32_t bank_names;         /* offset of the wave bank name table (16 bytes per name) */
    uint32_t controls;           /* parameter control table (0 when the bank has none) */
    int ncontrols, controls_sorted;
} XsbBank;

int xsb_parse(const uint8_t *d, size_t len, XsbBank *sb, char *err, size_t errlen);
const char *xsb_bank_name(const XsbBank *sb, int i, char out[17]);

/* Parameter controls ("V" volume, "F" fade, "P" pitch in this title): named per-sound inputs
 * the title sets through Prepare/PlayEx and IXACTEngine_SetParameterControl, each mapped to the
 * sound's volume or pitch by piecewise-linear curves (XBE: 0x3d85d5 finds a cue's controls,
 * 0x3d9224 matches a name, 0x3d8970 evaluates). Table at header +0x14, count u16 at +0x20,
 * 0x18-byte entries {u16 sound entry, u8 0, u8 curve count, u32 curve offset, name[16]},
 * sorted by sound entry in this title's bank. Curve (0x18 bytes): u16 track, u16 target
 * (0 volume, 1 pitch, 2 unknown), four points {u16 x = value * 65536, s16 y}, u32 0. */
typedef struct {
    int track, target;
    uint16_t x[4];
    int16_t y[4];                /* volume: hundredths of a dB */
} XsbCurve;
typedef struct {
    char name[17];
    int ncurves;
    uint32_t curves;             /* bank offset of the first curve */
} XsbControl;
enum { XSB_TARGET_VOLUME = 0, XSB_TARGET_PITCH = 1 };
/* Controls of sound entry `entry`: returns how many (consecutive table entries starting at
 * *first), 0 when it has none. */
int xsb_sound_controls(const XsbBank *sb, int entry, int *first);
int xsb_control(const XsbBank *sb, int i, XsbControl *out);   /* 0, or -1 when out of range */
int xsb_curve(const XsbBank *sb, const XsbControl *c, int k, XsbCurve *out);
/* The curve's output for a control value, as 0x3d8970: x = (int)(value * 65536) truncated;
 * x <= x0 gives y0, x >= x3 gives y3, otherwise linear between the enclosing points with
 * integer division (truncated towards zero). */
int32_t xsb_curve_eval(const XsbCurve *c, float value);

#define XSB_MAX_WAVES 8
typedef struct {
    int bank, wave;              /* bank: index into the sound bank's wave bank name table */
    int loops;                   /* 0 = play once, -1 = loop forever, n = n extra repeats */
} XsbWave;
typedef struct {
    int nwaves;
    XsbWave w[XSB_MAX_WAVES];
    int32_t volume;              /* hundredths of a decibel (<= 0) */
    int category;                /* 0xff = none */
    int entry;                   /* sound entry index the cue resolved to */
    int is3d;                    /* entry flag 0x01: positioned on its sound source (DirectSound 3D) */
    float min_dist, max_dist;    /* from the entry's 0x28-byte 3D record (+0x08, +0x0c) */
    int rec3d_index;             /* the entry's 3D record index (valid when is3d) */
    uint8_t rec3d[0x28];         /* the raw 3D record (zeros for 2D sounds), for the I3DL2 log */
} XsbSound;
/* Resolves a cue index to the waves its sound plays (one per play-wave event; variation and
 * random tables pick with *rng, a caller-owned LCG state). Returns 0, or -1 for a bad cue. */
int xsb_cue_sound(const XsbBank *sb, uint32_t cue, uint32_t *rng, XsbSound *out);

/* Playing time of one pass over an entry in milliseconds, as the XBE's XACT computes it
 * (0x3d4427): ADPCM bytes / 36 * 64, 16-bit PCM bytes / 2, then / channels * 1000 / rate. */
uint32_t xwb_entry_ms(const XwbEntry *e);
/* Cue properties: the 0x64-byte block IXACTSoundBank_GetSoundCueProperties writes, field for
 * field as the XBE's own code (0x3d12a7): +0x00 3D flag, +0x04 entry byte 0xe, +0x08 volume
 * (-16 x attenuation steps), +0x0c..+0x20 entry fields, +0x24 largest loop count of the play
 * events, +0x28 track count, +0x2c sound entry index, +0x30 wave index (0xffff when none or
 * mixed), +0x34 duration in ms (longest wave), +0x38/+0x3c entry fields, +0x40..+0x60 the 3D
 * parameter record. ms(ctx, bank, wave) gives a wave's duration (XSB_NO_MS when unknown).
 * Returns 0, or -1 for a bad cue (out zero-filled). */
#define XSB_NO_MS 0xFFFFFFFFu
typedef uint32_t (*XsbWaveMs)(void *ctx, int bank, int wave);
int xsb_cue_props(const XsbBank *sb, uint32_t cue, XsbWaveMs ms, void *ctx, uint32_t out[25]);
/* Sequential decoder over one wave bank entry (an AudioFill source). */
typedef struct {
    const XwbBank *wb;
    const XwbEntry *e;
    uint32_t pos;                /* byte position inside the entry */
    int loops;                   /* remaining repeats, -1 = forever */
    uint8_t io[72 * 64];         /* streamed read cache (whole ADPCM blocks) */
    uint32_t io_pos, io_len;     /* entry byte range held in io */
    int16_t blk[XADPCM_BLOCK_SAMPLES * 2];   /* decoded ADPCM block */
    int blk_n, blk_i;
} WaveReader;
void wave_reader_init(WaveReader *r, const XwbBank *wb, const XwbEntry *e, int loops);
int wave_reader_fill(void *ctx, int16_t *dst, int max);
#endif
