/* Game-independent audio tests: Xbox ADPCM decoding, synthetic XACT wave/sound banks (in
 * memory and streamed from a file), the mixer (resampling, gains, end of a voice) and the WAV
 * dump of the mixer thread.
 *   audio_test                     run the checks, print "ALL OK" or FAIL lines
 *   audio_test adpcm CHANNELS      decode stdin (whole blocks) to stdout as s16le (for the
 *                                  Python reference comparison in tests/test_audio.py) */
#define _GNU_SOURCE
#include "../src/audio.h"
#include "../src/xact.h"
#include "audio_synth.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int adpcm_filter(int ch) {
    uint8_t in[72];
    int16_t out[XADPCM_BLOCK_SAMPLES * 2];
    size_t block = 36u * ch;
    while (fread(in, 1, block, stdin) == block) {
        xadpcm_decode_block(in, ch, out);
        fwrite(out, sizeof(int16_t), (size_t)XADPCM_BLOCK_SAMPLES * ch, stdout);
    }
    return 0;
}

/* Constant-value source for the mixer checks. */
typedef struct { int16_t value; int left; } ConstSrc;
static int const_fill(void *ctx, int16_t *dst, int max) {
    ConstSrc *c = ctx;
    int n = c->left < max ? c->left : max;
    for (int i = 0; i < n; i++) dst[i] = c->value;
    c->left -= n;
    return n;
}

static void test_adpcm(void) {
    /* Hand-checked block: header sample 1000, step index 10 (step 19); nibble 1 adds
     * 19>>3 + 19>>2 = 6 and moves the index to 9 (step 17: +2+4 = 6), then 8 (16: +2+4),
     * 7 (14: +1+3), 6 (13: +1+3), 5 (12: +1+3), 4 (11: +1+2), 3 (10: +1+2), 2 (9: +1+2),
     * 1 (8: +1+2), 0 (7: +0+1) and stays at index 0 (+1 per sample). */
    uint8_t blk[36] = {0xE8, 0x03, 10, 0};
    memset(blk + 4, 0x11, 32);
    int16_t out[64];
    xadpcm_decode_block(blk, 1, out);
    static const int16_t want[12] = {1000, 1006, 1012, 1018, 1022, 1026, 1030, 1033, 1036, 1039, 1042, 1043};
    CHECK(!memcmp(out, want, sizeof want), "ADPCM mono: first 12 samples follow the IMA step table (got %d %d .. %d)", out[0], out[1], out[11]);
    CHECK(out[63] == 1043 + 52, "ADPCM mono: 64 samples per block, last nibble unused (out[63] = %d)", out[63]);
    uint8_t st[72];
    memset(st, 0, sizeof st);
    st[0] = 0x10; st[1] = 0x27; st[4] = 0xF0; st[5] = 0xD8;   /* L = 10000, R = -10000 */
    for (int k = 0; k < 8; k++) { memset(st + 8 + 8 * k, 0x77, 4); memset(st + 12 + 8 * k, 0xFF, 4); }
    int16_t so[128];
    xadpcm_decode_block(st, 2, so);
    CHECK(so[0] == 10000 && so[1] == -10000 && so[2] > 10000 && so[3] < -10000, "ADPCM stereo: interleaved 4-byte words per channel");
    CHECK(so[126] == 32767 && so[127] == -32768, "ADPCM stereo: decoded samples saturate (%d %d)", so[126], so[127]);
}

/* Sound bank wave duration callback over the one synthetic wave bank (bank index 0). */
static uint32_t synth_ms(void *ctx, int bank, int wave) {
    const XwbBank *wb = ctx;
    return bank == 0 && wave >= 0 && wave < wb->count ? xwb_entry_ms(&wb->entries[wave]) : XSB_NO_MS;
}

static void test_banks(uint8_t *xwb, uint32_t xwb_len, uint8_t *xsb, uint32_t xsb_len) {
    XwbBank wb;
    char err[96] = "";
    int r = xwb_parse(xwb, xwb_len, &wb, NULL, err, sizeof err);
    CHECK(!r, "xwb_parse synthetic bank (%s)", err);
    if (r) return;
    wb.mem = xwb + 0x80;
    CHECK(!strcmp(wb.name, "synth") && wb.count == 2, "wave bank name %s, %d entries", wb.name, wb.count);
    CHECK(wb.entries[0].codec == XWB_PCM && wb.entries[0].channels == 1 && wb.entries[0].rate == 24000 && wb.entries[0].bits == 16,
          "entry 0: PCM mono 24000 Hz 16-bit");
    CHECK(wb.entries[1].codec == XWB_XADPCM && wb.entries[1].channels == 2 && wb.entries[1].rate == 44100 && xwb_entry_frames(&wb.entries[1]) == 128,
          "entry 1: ADPCM stereo 44100 Hz, 128 frames");
    size_t need = 0;
    CHECK(xwb_parse(xwb, 0x40, &wb, &need, err, sizeof err) && need == 0x50 + 2 * 0x18, "truncated header reports the bytes needed (%zu)", need);
    xwb_parse(xwb, xwb_len, &wb, NULL, err, sizeof err);
    wb.mem = xwb + 0x80;

    WaveReader rd;
    int16_t pcm[1200];
    wave_reader_init(&rd, &wb, &wb.entries[0], 1);
    int got = 0, n;
    while ((n = wave_reader_fill(&rd, pcm + got, 7)) > 0 && got < 1100) got += n;
    CHECK(got == 2 * 500 && pcm[499] == 499 * 16 && pcm[500] == 0, "PCM entry with one repeat: %d frames, wraps to the start", got);
    wave_reader_init(&rd, &wb, &wb.entries[1], 0);
    got = 0;
    while ((n = wave_reader_fill(&rd, pcm + got * 2, 50)) > 0) got += n;
    CHECK(got == 128 && pcm[0] == 1000 && pcm[1] == -1000, "ADPCM entry served in partial blocks: %d frames", got);

    /* Streamed: the same bank behind 100 bytes of padding in a temporary file. */
    char path[] = "/tmp/audio_test_XXXXXX";
    int fd = mkstemp(path);
    XwbBank sw;
    if (fd >= 0) {
        uint8_t pad[100] = {0};
        CHECK(write(fd, pad, 100) == 100 && write(fd, xwb, xwb_len) == (ssize_t)xwb_len, "temporary streamed bank written");
        r = xwb_open_stream(fd, 100, &sw, err, sizeof err);
        CHECK(!r && sw.count == 2 && sw.data_offset == 100 + 0x80, "xwb_open_stream: %d entries, data at %llu (%s)", sw.count, (unsigned long long)sw.data_offset, err);
        if (!r) {
            int16_t a[256], b[256];
            WaveReader r1, r2;
            wave_reader_init(&r1, &wb, &wb.entries[1], 0);
            wave_reader_init(&r2, &sw, &sw.entries[1], 0);
            int n1 = wave_reader_fill(&r1, a, 128), n2 = wave_reader_fill(&r2, b, 128);
            CHECK(n1 == 128 && n2 == 128 && !memcmp(a, b, 256 * sizeof(int16_t)), "streamed entry decodes like the in-memory one");
            xwb_free(&sw);
        }
        close(fd);
        unlink(path);
    }

    XsbBank sb;
    r = xsb_parse(xsb, xsb_len, &sb, err, sizeof err);
    CHECK(!r && sb.cues == 3 && sb.banks == 1, "xsb_parse: %d cues, %d wave bank names (%s)", sb.cues, sb.banks, err);
    if (!r) {
        char name[17];
        XsbSound s;
        uint32_t rng = 1;
        CHECK(!strcmp(xsb_bank_name(&sb, 0, name), "synth"), "wave bank name 0 = %s", name);
        CHECK(!xsb_cue_sound(&sb, 0, &rng, &s) && s.nwaves == 1 && s.w[0].wave == 0 && s.w[0].bank == 0 && s.w[0].loops == 0 && s.volume == -400,
              "cue 0: track event plays wave 0 once at -4.00 dB (volume %d)", s.volume);
        CHECK(!xsb_cue_sound(&sb, 1, &rng, &s) && s.nwaves == 1 && s.w[0].wave == 1 && s.w[0].loops == 0, "cue 1: simple entry plays wave 1");
        CHECK(!xsb_cue_sound(&sb, 2, &rng, &s) && s.nwaves == 1 && s.w[0].loops == -1, "cue 2: loop count 0xffff loops forever");
        CHECK(xsb_cue_sound(&sb, 3, &rng, &s) == -1, "cue 3 (out of range) is rejected");
        /* Durations in ms as the XBE computes them (0x3d4427): ADPCM length / 36 * 64, 16-bit PCM
         * length / 2, then / channels * 1000 / rate, integer. */
        CHECK(xwb_entry_ms(&wb.entries[0]) == 20 && xwb_entry_ms(&wb.entries[1]) == 2,
              "wave durations: 500 PCM frames at 24 kHz = %u ms, 2 stereo ADPCM blocks at 44.1 kHz = %u ms",
              xwb_entry_ms(&wb.entries[0]), xwb_entry_ms(&wb.entries[1]));
        uint32_t pr[25];
        CHECK(!xsb_cue_props(&sb, 1, synth_ms, &wb, pr) && pr[0x34 / 4] == 2 && pr[0x30 / 4] == 1 && pr[0x2c / 4] == 1 &&
              pr[0x24 / 4] == 0 && pr[0x28 / 4] == 0,
              "cue 1 properties (simple entry): duration %u ms, wave %u, entry %u", pr[0x34 / 4], pr[0x30 / 4], pr[0x2c / 4]);
        CHECK(xsb_cue_props(&sb, 3, synth_ms, &wb, pr) == -1 && pr[0x34 / 4] == 0, "cue 3 properties: rejected, zero-filled");
    }
    xwb_free(&wb);
}

/* Appends a parameter control table (header +0x14, count at +0x20) and its curves to a
 * synthetic sound bank: entry 0 "V" and "P", entry 1 "V" and "F" (this title's three curves),
 * entry 2 none. unsorted lists entry 1 first. Returns the new bank length. */
static const uint16_t curve_v[8] = {655, (uint16_t)-4000, 3932, (uint16_t)-2443, 9830, (uint16_t)-1647, 65535, 0};
static const uint16_t curve_f[8] = {0, (uint16_t)-6400, 10695, (uint16_t)-2256, 32281, (uint16_t)-473, 65535, 0};
static const uint16_t curve_p[8] = {0, (uint16_t)-8192, 21002, (uint16_t)-5440, 44949, (uint16_t)-2621, 65535, 0};
static uint32_t synth_controls(uint8_t *xsb, uint32_t len, int unsorted) {
    static const struct { int sound; const char *name; int target; const uint16_t *pts; } c[4] = {
        {0, "V", 0, curve_v}, {0, "P", 1, curve_p}, {1, "V", 0, curve_v}, {1, "F", 0, curve_f}};
    uint32_t t = (len + 3) & ~3u, cv = t + 4 * 0x18;
    sy_w32(xsb + 0x14, t);
    sy_w16(xsb + 0x20, 4);
    for (int i = 0; i < 4; i++) {
        int k = unsorted ? (i + 2) % 4 : i;
        uint8_t *e = xsb + t + i * 0x18, *p = xsb + cv + i * 0x18;
        memset(e, 0, 0x18);
        sy_w16(e, (uint16_t)c[k].sound);
        e[3] = 1;
        sy_w32(e + 4, cv + i * 0x18);
        memcpy(e + 8, c[k].name, strlen(c[k].name));
        memset(p, 0, 0x18);
        sy_w16(p + 2, (uint16_t)c[k].target);
        for (int j = 0; j < 8; j++) sy_w16(p + 4 + 2 * j, c[k].pts[j]);
    }
    return cv + 4 * 0x18;
}

static XsbCurve curve_of(const uint16_t pts[8]) {
    XsbCurve c = {0, 0, {pts[0], pts[2], pts[4], pts[6]}, {(int16_t)pts[1], (int16_t)pts[3], (int16_t)pts[5], (int16_t)pts[7]}};
    return c;
}

static void test_controls(const uint8_t *xsb0, uint32_t len0) {
    /* Curve evaluation as 0x3d8970 (expected values worked by hand from the formula). */
    XsbCurve v = curve_of(curve_v), f = curve_of(curve_f);
    CHECK(xsb_curve_eval(&v, 0.18f) == -1589, "V curve at 0.18 -> %d (-15.9 dB)", xsb_curve_eval(&v, 0.18f));
    CHECK(xsb_curve_eval(&f, 0.0f) == -6400, "F curve at 0 -> %d (-64 dB)", xsb_curve_eval(&f, 0.0f));
    CHECK(xsb_curve_eval(&f, 1.0f) == 0 && xsb_curve_eval(&v, 1.0f) == 0, "F and V curves at 1 -> 0 dB");
    CHECK(xsb_curve_eval(&f, 0.1f) == -3861, "F curve at 0.1 -> %d (first segment)", xsb_curve_eval(&f, 0.1f));
    CHECK(xsb_curve_eval(&f, 0.5f) == -467, "F curve at 0.5 -> %d (last segment)", xsb_curve_eval(&f, 0.5f));
    CHECK(xsb_curve_eval(&v, 0.0f) == -4000 && xsb_curve_eval(&v, -1.0f) == -4000 && xsb_curve_eval(&v, 655 / 65536.0f) == -4000,
          "V curve clamps below its first point (0, -1, x0) -> -4000");
    CHECK(xsb_curve_eval(&v, 2.0f) == 0 && xsb_curve_eval(&v, 30000.0f) == 0, "V curve clamps above its last point (2, 30000) -> 0");
    CHECK(xsb_curve_eval(&v, NAN) == -4000 && xsb_curve_eval(&v, 1e12f) == -4000,
          "NaN and values past the int range read as the x86 integer indefinite value -> first point");
    CHECK(xsb_curve_eval(&v, 3932 / 65536.0f) == -2443, "V curve exactly at its second point -> -2443");
    XsbCurve d = {0, 0, {0, 3, 65534, 65535}, {0, -10, -10, -10}};
    CHECK(xsb_curve_eval(&d, 1 / 65536.0f) == -3, "interpolation truncates towards zero (-10 * 1 / 3 -> %d)", xsb_curve_eval(&d, 1 / 65536.0f));

    /* Control table lookup, sorted (binary search) and unsorted (linear). */
    for (int unsorted = 0; unsorted < 2; unsorted++) {
        static uint8_t xsb[1024];
        memcpy(xsb, xsb0, len0);
        uint32_t len = synth_controls(xsb, len0, unsorted);
        XsbBank sb;
        char err[96] = "";
        int r = xsb_parse(xsb, len, &sb, err, sizeof err);
        CHECK(!r && sb.ncontrols == 4 && sb.controls_sorted == !unsorted, "%s control table: %d entries (%s)",
              unsorted ? "unsorted" : "sorted", sb.ncontrols, err);
        int first = -1, n0 = xsb_sound_controls(&sb, 0, &first), first1 = -1, n1 = xsb_sound_controls(&sb, 1, &first1), x;
        CHECK(n0 == 2 && first == (unsorted ? 2 : 0) && n1 == 2 && first1 == (unsorted ? 0 : 2) && xsb_sound_controls(&sb, 2, &x) == 0 &&
              xsb_sound_controls(&sb, 99, &x) == 0, "sound entries 0 and 1 have 2 controls each (first %d, %d), entry 2 none", first, first1);
        XsbControl c;
        XsbCurve cv;
        int names_ok = 1;
        for (int i = 0; i < n1; i++) {
            names_ok &= !xsb_control(&sb, first1 + i, &c) && c.ncurves == 1 && !xsb_curve(&sb, &c, 0, &cv);
            names_ok &= !strcmp(c.name, i ? "F" : "V") && cv.target == XSB_TARGET_VOLUME;
            names_ok &= i ? !memcmp(cv.x, f.x, sizeof f.x) && !memcmp(cv.y, f.y, sizeof f.y) : xsb_curve_eval(&cv, 0.18f) == -1589;
        }
        CHECK(names_ok, "entry 1 controls: V and F, one volume curve each, points as written");
        CHECK(!xsb_control(&sb, first + 1, &c) && !strcmp(c.name, "P") && !xsb_curve(&sb, &c, 0, &cv) && cv.target == XSB_TARGET_PITCH,
              "entry 0 control P targets pitch");
        CHECK(xsb_control(&sb, 4, &c) == -1 && xsb_curve(&sb, &c, 1, &cv) == -1, "out-of-range control and curve rejected");
        if (!unsorted) {
            sy_w16(xsb + 0x20, 60000);   /* table past the end: the bank stays usable, without controls */
            r = xsb_parse(xsb, len, &sb, err, sizeof err);
            CHECK(!r && sb.ncontrols == 0 && xsb_sound_controls(&sb, 0, &x) == 0, "control table out of range -> no controls");
        }
    }
    XsbBank sb;
    char err[96];
    CHECK(!xsb_parse(xsb0, len0, &sb, err, sizeof err) && sb.ncontrols == 0, "bank without a control table -> no controls");
}

static void test_mixer(void) {
    ConstSrc c = {1000, 1000};
    AudioVoice *v = voice_new(1, 24000, const_fill, &c);
    voice_play(v, 1);
    int16_t out[480 * 2];
    audio_lock();
    audio_mix(out, 480);
    audio_unlock();
    CHECK(out[0] == 1000 && out[1] == 1000 && out[958] == 1000 && out[959] == 1000, "mono 24 kHz voice upsampled to both channels");
    CHECK(c.left == 1000 - 241, "480 output frames consumed 240 source frames plus 1 of lookahead (%d left)", c.left);
    voice_set_volume(v, audio_db100_to_gain(-602));
    audio_lock();
    audio_mix(out, 2);
    audio_unlock();
    CHECK(abs(out[0] - 500) <= 2, "-6.02 dB halves the level (%d)", out[0]);
    float g[2][2] = {{0, 1}, {0, 0}};
    voice_set_volume(v, 1);
    voice_set_gains(v, g);
    audio_lock();
    audio_mix(out, 2);
    audio_unlock();
    CHECK(out[0] == 0 && out[1] == 1000, "gains route the source to the right channel only");
    audio_lock();
    for (int i = 0; i < 10; i++) audio_mix(out, 480);
    audio_unlock();
    CHECK(!voice_playing(v) && out[0] == 0 && out[1] == 0, "the voice stops after its source ends");
    voice_free(v);
    ConstSrc l1 = {30000, 4800}, l2 = {30000, 4800};
    AudioVoice *a1 = voice_new(1, 48000, const_fill, &l1), *a2 = voice_new(1, 48000, const_fill, &l2);
    voice_play(a1, 1);
    voice_play(a2, 1);
    int16_t lim[2400 * 2];
    audio_lock();
    audio_mix(lim, 2400);
    audio_unlock();
    int lo = 32767, hi = 0;
    for (int i = 0; i < 4800; i++) { lo = lim[i] < lo ? lim[i] : lo; hi = lim[i] > hi ? lim[i] : hi; }
    CHECK(hi <= 32000 && lo > 30000, "limiter: two voices at 30000 stay under the -0.2 dBFS ceiling (range %d..%d)", lo, hi);
    voice_free(a1);
    voice_free(a2);
    audio_lock();
    for (int i = 0; i < 400; i++) audio_mix(out, 480);   /* let the limiter recover */
    audio_unlock();
    CHECK(fabsf(audio_db100_to_gain(0) - 1) < 1e-6f && audio_db100_to_gain(-10000) == 0, "0 dB = unity, -100 dB = silence");
}

static void test_dump(void) {
    char path[] = "/tmp/audio_dump_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { CHECK(0, "temporary dump file"); return; }
    close(fd);
    ConstSrc c = {3000, 1 << 30};
    AudioVoice *v = voice_new(2, 48000, const_fill, &c);
    voice_play(v, 1);
    CHECK(!strcmp(audio_open("null", path), "null"), "null backend opened");
    nanosleep(&(struct timespec){0, 120 * 1000 * 1000}, NULL);
    uint64_t mixed = audio_frames_mixed();
    audio_close();
    voice_free(v);
    FILE *f = fopen(path, "rb");
    uint8_t h[44];
    int16_t s[2] = {0, 0};
    long size = 0;
    if (f) {
        size_t hn = fread(h, 1, 44, f);
        size_t sn = fread(s, 2, 2, f);
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
        uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
        CHECK(hn == 44 && sn == 2 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && (long)data == size - 44,
              "WAV header patched with the data size (%u bytes)", data);
        CHECK(mixed >= 4800 && data / 4 >= mixed, "the mixer thread ran on the host clock (%llu frames in about 120 ms)", (unsigned long long)mixed);
        CHECK(s[0] == 3000 && s[1] == 3000, "dump holds the mix (%d %d)", s[0], s[1]);
    } else CHECK(0, "dump file readable");
    unlink(path);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "adpcm")) return adpcm_filter(atoi(argv[2]) == 2 ? 2 : 1);
    static uint8_t xwb[4096], xsb[1024];
    uint32_t xwb_len = synth_xwb(xwb, 500), xsb_len = synth_xsb(xsb);
    test_adpcm();
    test_banks(xwb, xwb_len, xsb, xsb_len);
    test_controls(xsb, xsb_len);
    test_mixer();
    test_dump();
    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
