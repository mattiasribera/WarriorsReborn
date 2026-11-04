/* Game-independent test of the sound replacement packs (src/sndpack.c): CRC-32 and cue names,
 * WAV parsing and conversion (8/16/24/32-bit, float, extensible, odd chunks), the warriors.glr
 * name table (also through a synthetic XBoxWad.idx + WAD), the pack index, the binding to a
 * synthetic sound bank (audio_synth.h) and the wave lookups. Usage: sndpack_test DIR (scratch). */
#include "../src/sndpack.h"
#include "audio_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir(p, 0755)
#endif

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* WAV builder: fmt (tag 1, 3 or 0xFFFE with the sub-format), an odd-sized LIST chunk, data. */
static size_t make_wav(uint8_t *b, int tag, int sub, int ch, uint32_t rate, int bits, const void *data, uint32_t len) {
    size_t o = 12;
    memcpy(b, "RIFF", 4); memcpy(b + 8, "WAVE", 4);
    uint32_t fmt_len = tag == 0xFFFE ? 40 : 16;
    memcpy(b + o, "fmt ", 4); sy_w32(b + o + 4, fmt_len); o += 8;
    memset(b + o, 0, fmt_len);
    sy_w16(b + o, (uint16_t)tag); sy_w16(b + o + 2, (uint16_t)ch); sy_w32(b + o + 4, rate);
    sy_w32(b + o + 8, rate * ch * bits / 8); sy_w16(b + o + 12, (uint16_t)(ch * bits / 8)); sy_w16(b + o + 14, (uint16_t)bits);
    if (tag == 0xFFFE) { sy_w16(b + o + 16, 22); sy_w16(b + o + 24, (uint16_t)sub); }
    o += fmt_len;
    memcpy(b + o, "LIST", 4); sy_w32(b + o + 4, 3); memcpy(b + o + 8, "abc", 3); b[o + 11] = 0; o += 12;   /* odd: pad */
    memcpy(b + o, "data", 4); sy_w32(b + o + 4, len); o += 8;
    memcpy(b + o, data, len); o += len;
    sy_w32(b + 4, (uint32_t)o - 8);
    return o;
}

static int16_t *decode(const uint8_t *b, size_t n, SndWav *w) {
    char err[128];
    if (sndpack_wav_info(b, n, w, err, sizeof err)) { printf("     (%s)\n", err); return NULL; }
    return sndpack_wav_pcm16(b, n, w);
}

static int write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    return fclose(f) || w != n ? -1 : 0;
}

/* glr pack: an unrelated chunk, then chunk 0x29 with {crc, cue, bank, 0} records. */
static size_t make_glr(uint8_t *b, const char **names, const uint32_t *cues, int n) {
    memset(b, 0, 16);
    sy_w32(b, 2);
    size_t o = 16;
    sy_w32(b + o, 0x10); sy_w32(b + o + 4, 4); o += 16; memcpy(b + o, "xxxx", 4); o += 4;
    sy_w32(b + o, 0x29); sy_w32(b + o + 4, 4 + 12 * n); memset(b + o + 8, 0, 8); o += 16;
    sy_w32(b + o, (uint32_t)n); o += 4;
    for (int i = 0; i < n; i++, o += 12) {
        sy_w32(b + o, sndpack_crc32(names[i])); sy_w32(b + o + 4, cues[i]); sy_w32(b + o + 8, 0);
    }
    return o;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    const char *tmp = argv[1];
    (void)synth_xwb;
    char path[1024], name[256];

    CHECK(sndpack_crc32("123456789") == 0xCBF43926u, "CRC-32 check value");
    CHECK(!sndpack_cue_name("vags\\Ambient\\Beach/2gulls.WAV", name, sizeof name) && !strcmp(name, "vags/ambient/beach/2gulls"),
          "cue name normalized: %s", name);
    CHECK(!sndpack_cue_name("ambient/beach/2gulls.wav", name, sizeof name) && !strcmp(name, "vags/ambient/beach/2gulls"),
          "vags/ prefix added: %s", name);
    CHECK(sndpack_cue_name("ambient/readme.txt", name, sizeof name) < 0 && sndpack_cue_name(".wav", name, sizeof name) < 0,
          "non-WAV and empty names rejected");

    /* WAV formats: 3 stereo frames each */
    static uint8_t b[4096];
    SndWav w;
    int16_t *p;
    const int16_t s16[] = {100, -100, 32767, -32768, 0, 1};
    size_t n = make_wav(b, 1, 0, 2, 22050, 16, s16, sizeof s16);
    p = decode(b, n, &w);
    CHECK(p && w.channels == 2 && w.rate == 22050 && w.frames == 3 && !memcmp(p, s16, sizeof s16), "16-bit stereo (odd chunk skipped)");
    free(p);
    const uint8_t u8[] = {128, 255, 0};
    n = make_wav(b, 1, 0, 1, 8000, 8, u8, sizeof u8);
    p = decode(b, n, &w);
    CHECK(p && w.frames == 3 && p[0] == 0 && p[1] == 127 * 256 && p[2] == -32768, "8-bit unsigned mono");
    free(p);
    const uint8_t s24[] = {0x00, 0x34, 0x12, 0xff, 0xff, 0xff, 0x00, 0x00, 0x80};
    n = make_wav(b, 1, 0, 1, 96000, 24, s24, sizeof s24);
    p = decode(b, n, &w);
    CHECK(p && w.frames == 3 && p[0] == 0x1234 && p[1] == -1 && p[2] == -32768, "24-bit mono");
    free(p);
    const int32_t s32[] = {0x12345678, -0x7fffffff - 1};
    n = make_wav(b, 0xFFFE, 1, 1, 48000, 32, s32, sizeof s32);
    p = decode(b, n, &w);
    CHECK(p && w.frames == 2 && p[0] == 0x1234 && p[1] == -32768, "32-bit extensible PCM");
    free(p);
    const float f32[] = {0.5f, -1.5f, 1.0f, 0.0f};
    n = make_wav(b, 3, 0, 2, 44100, 32, f32, sizeof f32);
    p = decode(b, n, &w);
    CHECK(p && w.is_float && w.frames == 2 && p[0] == 16384 && p[1] == -32768 && p[2] == 32767 && p[3] == 0, "float, clamped");
    free(p);
    char err[128];
    n = make_wav(b, 1, 0, 6, 48000, 16, s16, sizeof s16);
    CHECK(sndpack_wav_info(b, n, &w, err, sizeof err) < 0, "6 channels rejected (%s)", err);
    n = make_wav(b, 2, 0, 1, 48000, 4, s16, sizeof s16);
    CHECK(sndpack_wav_info(b, n, &w, err, sizeof err) < 0, "ADPCM rejected (%s)", err);
    CHECK(sndpack_wav_info((const uint8_t *)"RIFF\0\0\0\0WAVE", 12, &w, err, sizeof err) < 0, "no fmt/data rejected");
    n = make_wav(b, 1, 0, 1, 8000, 16, s16, sizeof s16);
    sy_w32(b + n - sizeof s16 - 4, 1000000);   /* data length past the end of the file: clamped */
    p = decode(b, n, &w);
    CHECK(p && w.frames == 6, "truncated data clamped to the file (%u frames)", w.frames);
    free(p);

    /* name table */
    static uint8_t glr[1024];
    const char *names[] = {"vags/ambient/beach/2gulls", "vags/character/ajax/hit_01", "vags/unused"};
    const uint32_t cues[] = {0, 1, 2};
    size_t gn = make_glr(glr, names, cues, 3);
    SndName *tab = NULL;
    int nt = sndpack_names_parse(glr, gn, &tab);
    CHECK(nt == 3 && tab[1].crc == sndpack_crc32(names[1]) && tab[1].cue == 1, "name table parsed (%d)", nt);
    CHECK(sndpack_names_parse(glr, 20, &tab) < 0, "truncated pack rejected");
    free(tab);
    tab = NULL;

    /* the same table inside a WAD: XBoxWad.idx {1 wad, 1 file} + Main.wad0 */
    char disc[1024];
    snprintf(disc, sizeof disc, "%s/disc", tmp);
    mkdir_(disc);
    uint8_t idx[20 + 36 + 16] = {0};
    sy_w32(idx, 1); sy_w32(idx + 4, 1); sy_w32(idx + 12, 20); sy_w32(idx + 16, 56);
    memcpy(idx + 20, "Main.wad0", 9);
    sy_w32(idx + 56, 1); sy_w32(idx + 60, (uint32_t)gn); sy_w32(idx + 68, sndpack_crc32("ee_files\\warriors.glr"));
    static uint8_t wad[1024 + 1024] = {0};
    memcpy(wad + 1024, glr, gn);   /* granule 1 */
    snprintf(path, sizeof path, "%s/XBoxWad.idx", disc);
    write_file(path, idx, sizeof idx);
    snprintf(path, sizeof path, "%s/Main.wad0", disc);
    write_file(path, wad, 1024 + gn);
    nt = sndpack_names_load(disc, &tab);
    CHECK(nt == 3 && tab[2].cue == 2, "name table from the WAD (%d)", nt);
    free(tab);
    CHECK(sndpack_names_load("/nonexistent", &tab) < 0, "no disc: no table");

    /* pack: vags/ambient/beach/2gulls.wav (cue 0), character/ajax/hit_01.wav (cue 1, no vags/),
     * other.wav (no cue), notes.txt, bad.wav (not a WAV) */
    char pack[1024];
    snprintf(pack, sizeof pack, "%s/pack", tmp);
    const char *dirs[] = {"", "/vags", "/vags/ambient", "/vags/ambient/beach", "/character", "/character/ajax"};
    for (int i = 0; i < 6; i++) { snprintf(path, sizeof path, "%s%s", pack, dirs[i]); mkdir_(path); }
    int16_t tone[480];
    for (int i = 0; i < 480; i++) tone[i] = (int16_t)(i * 50);
    n = make_wav(b, 1, 0, 1, 48000, 16, tone, sizeof tone);   /* 10 ms */
    snprintf(path, sizeof path, "%s/vags/ambient/beach/2gulls.wav", pack); write_file(path, b, n);
    snprintf(path, sizeof path, "%s/other.wav", pack); write_file(path, b, n);
    static uint8_t big[2 * 1102];
    memset(big, 128, sizeof big);
    n = make_wav(b, 1, 0, 2, 11025, 8, big, 2 * 1102);   /* 1102 frames = 99 ms */
    snprintf(path, sizeof path, "%s/character/ajax/hit_01.wav", pack); write_file(path, b, n);
    snprintf(path, sizeof path, "%s/notes.txt", pack); write_file(path, "x", 1);
    snprintf(path, sizeof path, "%s/vags/bad.wav", pack); write_file(path, "nope", 4);

    CHECK(!sndpack_active() && !sndpack_find("synth", 0, NULL, NULL) && sndpack_ms("synth", 0) == XSB_NO_MS,
          "no pack: nothing replaced");
    CHECK(sndpack_open("/nonexistent", disc) == -1, "missing pack folder");
    int no = sndpack_open(pack, disc);
    CHECK(no == 2 && sndpack_active(), "2 overrides indexed (%d)", no);

    static uint8_t xsb[1024];
    XsbBank sb;
    CHECK(!xsb_parse(xsb, synth_xsb(xsb), &sb, err, sizeof err), "synthetic sound bank");
    CHECK(!sndpack_find("synth", 0, NULL, NULL), "nothing found before the bind");
    int nk = sndpack_bind(&sb);
    CHECK(nk == 2, "cue 0 -> synth/0, cue 1 -> synth/1 (%d keys)", nk);
    const XwbBank *wb = NULL;
    const char *src = NULL;
    const XwbEntry *e = sndpack_find("SYNTH", 0, &wb, &src);
    CHECK(e && wb && e->codec == XWB_PCM && e->bits == 16 && e->channels == 1 && e->rate == 48000 && e->length == 960,
          "synth/0 -> 10 ms mono 16-bit override");
    CHECK(src && strstr(src, "vags/ambient/beach/2gulls.wav"), "override path %s", src ? src : "-");
    if (e) {
        WaveReader r;
        int16_t got[600];
        wave_reader_init(&r, wb, e, 0);
        int k = wave_reader_fill(&r, got, 600);
        CHECK(k == 480 && got[479] == tone[479], "override samples play through the wave reader (%d)", k);
    }
    e = sndpack_find("synth", 1, &wb, NULL);
    CHECK(e && e->channels == 2 && e->rate == 11025, "synth/1 -> stereo 8-bit file widened to 16-bit");
    CHECK(sndpack_ms("synth", 0) == 10 && sndpack_ms("synth", 1) == 99, "wave_ms of overrides (%u, %u)",
          sndpack_ms("synth", 0), sndpack_ms("synth", 1));
    CHECK(!sndpack_find("synth", 2, NULL, NULL) && !sndpack_find("other", 0, NULL, NULL) && sndpack_ms("other", 0) == XSB_NO_MS,
          "other waves untouched");

    /* budget: only what fits is kept */
    sndpack_budget = 1000;
    no = sndpack_open(pack, disc);
    CHECK(no == 1, "budget keeps 1 override (%d)", no);
    sndpack_budget = 256u << 20;
    sndpack_close();
    CHECK(!sndpack_active() && !sndpack_find("synth", 0, NULL, NULL), "closed: nothing replaced");

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
