/* Game-independent test of the audio extractor's helper (tools/xact_extract.c) on the synthetic
 * XACT banks of audio_synth.h: the cue -> (wave bank, wave) listing and WAV decoding (Xbox ADPCM
 * and PCM through the runtime's own wave reader). Usage: xact_extract_test DIR (a scratch folder;
 * the synthetic banks are left there as synth.xwb and synth.xsb for tests/test_audio_extract.py). */
#define XACT_EXTRACT_LIB
#include "xact_extract.c"
#include "audio_synth.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    return fclose(f) || w != n ? -1 : 0;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len > 0 ? (size_t)len : 1);
    *n = d ? fread(d, 1, (size_t)len, f) : 0;
    fclose(f);
    return d;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    char xwb[1024], xsb[1024], out[1024];
    static uint8_t wb[4096], sb[1024];
    snprintf(xwb, sizeof xwb, "%s/synth.xwb", argv[1]);
    snprintf(xsb, sizeof xsb, "%s/synth.xsb", argv[1]);
    uint32_t wn = synth_xwb(wb, 100), sn = synth_xsb(sb);
    CHECK(!write_file(xwb, wb, wn) && !write_file(xsb, sb, sn), "synthetic banks written");

    /* cue listing: cue 0 and 2 play wave 0, cue 1 plays wave 1 of bank "synth" */
    char listing[4096] = "";
    FILE *mem = tmpfile();
    int nc = mem ? xx_cues(xsb, mem) : -1;
    if (mem) { rewind(mem); size_t got = fread(listing, 1, sizeof listing - 1, mem); listing[got] = 0; fclose(mem); }
    CHECK(nc == 3, "3 cues listed (%d)", nc);
    CHECK(!strcmp(listing, "0\tsynth\t0\n1\tsynth\t1\n2\tsynth\t0\n"), "cue -> bank/wave lines");
    CHECK(xx_cues("/nonexistent/x.xsb", stdout) < 0, "missing sound bank is an error");

    /* ADPCM stereo, 2 blocks: 128 frames at 44100 Hz; the header samples come first */
    XxInfo in;
    char err[256] = "";
    snprintf(out, sizeof out, "%s/adpcm.wav", argv[1]);
    CHECK(!xx_decode(xwb, 1, out, &in, err, sizeof err), "ADPCM entry decoded %s", err);
    CHECK(in.frames == 128 && in.channels == 2 && in.rate == 44100, "ADPCM info %u frames %d ch %u Hz", in.frames,
          in.channels, in.rate);
    size_t n = 0;
    uint8_t *w = read_file(out, &n);
    CHECK(w && n == 44 + 128 * 4, "ADPCM WAV size %zu", n);
    if (w && n >= 48) {
        CHECK(!memcmp(w, "RIFF", 4) && le32(w + 4) == n - 8 && !memcmp(w + 8, "WAVEfmt ", 8), "RIFF/WAVE header");
        CHECK(le32(w + 16) == 16 && (w[20] | w[21] << 8) == 1 && (w[22] | w[23] << 8) == 2 && le32(w + 24) == 44100 &&
              le32(w + 28) == 44100 * 4 && (w[32] | w[33] << 8) == 4 && (w[34] | w[35] << 8) == 16, "fmt chunk");
        CHECK(!memcmp(w + 36, "data", 4) && le32(w + 40) == 128 * 4, "data chunk length");
        CHECK(le16s(w + 44) == 1000 && le16s(w + 46) == -1000, "first frame = block headers (%d, %d)", le16s(w + 44),
              le16s(w + 46));
        /* the same samples the runtime plays (wave_reader_fill) */
        XwbBank bank;
        size_t need = 0;
        int16_t ref[256 * 2];
        if (!xwb_parse(wb, wn, &bank, &need, err, sizeof err)) {
            bank.mem = wb + 0x80;
            WaveReader r;
            wave_reader_init(&r, &bank, &bank.entries[1], 0);
            int got = wave_reader_fill(&r, ref, 256);
            int same = got == 128;
            for (int i = 0; same && i < 256; i++) same = ref[i] == le16s(w + 44 + 2 * i);
            CHECK(same, "samples match the runtime wave reader (%d frames)", got);
            xwb_free(&bank);
        } else CHECK(0, "parse synthetic bank: %s", err);
    }
    free(w);

    /* PCM 16-bit mono ramp: 100 frames at 24000 Hz */
    snprintf(out, sizeof out, "%s/pcm.wav", argv[1]);
    CHECK(!xx_decode(xwb, 0, out, &in, err, sizeof err), "PCM entry decoded %s", err);
    CHECK(in.frames == 100 && in.channels == 1 && in.rate == 24000, "PCM info");
    w = read_file(out, &n);
    CHECK(w && n == 44 + 200 && le16s(w + 44 + 2 * 99) == 99 * 16, "PCM samples copied");
    free(w);

    CHECK(xx_decode(xwb, 2, out, &in, err, sizeof err) < 0, "wave index out of range is an error (%s)", err);
    CHECK(xx_decode("/nonexistent.xwb", 0, out, &in, err, sizeof err) < 0, "missing wave bank is an error");

    /* the decode job loop: one result line per job, in order */
    char jobs[2048];
    snprintf(jobs, sizeof jobs, "%s\t1\t%s/j1.wav\n%s\t7\t%s/j2.wav\n%s\t0\t%s/j3.wav\n", xwb, argv[1], xwb, argv[1], xwb,
             argv[1]);
    FILE *jin = tmpfile(), *jout = tmpfile();
    char res[1024] = "";
    if (jin && jout) {
        fputs(jobs, jin);
        rewind(jin);
        xx_decode_jobs(jin, jout);
        rewind(jout);
        size_t got = fread(res, 1, sizeof res - 1, jout);
        res[got] = 0;
    }
    if (jin) fclose(jin);
    if (jout) fclose(jout);
    CHECK(!strncmp(res, "ok\t128\t2\t44100\nerr\t", 19) && strstr(res, "\nok\t100\t1\t24000\n"), "job results: %s", res);

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
