/* Sound replacement packs (--sound-pack DIR): DIR/<cue name>.wav plays instead of the wave of
 * that cue, e.g. DIR/vags/character/ajax/fight/hit_01.wav (the "vags/" folder may be left out).
 * tools/audio_extract writes the original sounds with exactly these paths.
 *
 * Keying: the title names its cues by CRC-32 of the exact "vags/..." string (lower case, "/").
 * Its name table, chunk 0x29 "Static Sounds" of warriors.glr ({u32 crc32, u32 cue, u16 bank,
 * u16} records), is read at start-up from the user's disc folder (XBoxWad.idx -> the WAD that
 * holds warriors.glr), so each pack file resolves to a cue. When the title creates its sound
 * bank, each overridden cue's waves (xsb_cue_sound) give the (wave bank name, wave index) keys
 * that the cue builder and the duration query look up. A random cue keys only the wave of one
 * pick (this title's cues have one wave each).
 *
 * Files: RIFF WAVE, PCM 8/16/24/32-bit or 32-bit float (also WAVE_FORMAT_EXTENSIBLE), mono or
 * stereo, any rate (the mixer resamples). Every override is decoded to 16-bit PCM in memory at
 * start-up (sndpack_budget bytes in all; files past it are skipped with a message), so the audio
 * path never touches the disk. No FLAC/OGG.
 *
 * With no pack open every lookup returns "none" before any other work. Not thread-safe: open,
 * bind and close on the title's thread; the returned bank/entry stay valid until sndpack_close. */
#ifndef SNDPACK_H
#define SNDPACK_H
#include "xact.h"
#include <stddef.h>
#include <stdint.h>

uint32_t sndpack_crc32(const char *s);   /* CRC-32 (IEEE, the zlib one) of a C string */

/* Cue name of a pack file's path relative to the pack ("vags/Ambient\\x.WAV" -> "vags/ambient/x";
 * "ambient/x.wav" -> "vags/ambient/x"). Returns 0, or -1 when it is not a .wav or too long. */
int sndpack_cue_name(const char *rel, char *out, size_t n);

typedef struct {
    int channels, bits, is_float;
    uint32_t rate, frames;
    size_t data;                 /* offset of the first sample */
} SndWav;
/* Parses a WAV header; returns 0, or -1 with a reason in err. */
int sndpack_wav_info(const uint8_t *d, size_t n, SndWav *w, char *err, size_t errlen);
/* Interleaved 16-bit samples of a parsed WAV (malloc'd, frames * channels). */
int16_t *sndpack_wav_pcm16(const uint8_t *d, size_t n, const SndWav *w);

typedef struct { uint32_t crc, cue; } SndName;
/* The name table of warriors.glr bytes (malloc'd in *out); returns the count or -1. */
int sndpack_names_parse(const uint8_t *glr, size_t n, SndName **out);
/* The same from a disc folder (XBoxWad.idx and its WAD files). */
int sndpack_names_load(const char *disc_dir, SndName **out);

extern size_t sndpack_budget;   /* decoded bytes kept in all (default 256 MiB) */

/* Indexes and decodes the .wav files under dir (recursively) whose cue name is in names.
 * Returns the number of overrides, or -1 when dir cannot be read. Replaces any open pack. */
int sndpack_open_table(const char *dir, const SndName *names, int n);
/* The same with the names of the disc folder; -2 when its name table cannot be read. */
int sndpack_open(const char *dir, const char *disc_dir);
int sndpack_active(void);       /* overrides loaded */

/* Maps the overridden cues of a sound bank to (wave bank, wave) keys; returns the keys added. */
int sndpack_bind(const XsbBank *sb);
/* The override of a wave (bank name compared without case): a 16-bit PCM in-memory bank and its
 * one entry for wave_reader_init, and the file's pack path. NULL when the wave is not replaced. */
const XwbEntry *sndpack_find(const char *bank, int wave, const XwbBank **wb, const char **path);
/* The override's length in ms, or XSB_NO_MS when the wave is not replaced. */
uint32_t sndpack_ms(const char *bank, int wave);
void sndpack_close(void);
#endif
