/* Game-independent test of a mod's own sounds (src/modsound.h): loading the asset cache's WAV
 * files, budgets, 2D / 3D play through fake mixer voices, handles bound to their mod, the distance
 * and pan law, finished voices freed, a new Lua state stopping everything. No game data.
 * Usage: modsound_test [tmpdir] */
#include "../src/modsound.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c, msg)                                            \
    do {                                                         \
        if (!(c)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            failures++;                                          \
        }                                                        \
    } while (0)

typedef struct {
    int used, playing, channels;
    uint32_t rate;
    ModSoundFill fill;
    void *ctx;
    float g[2][2];
} FakeVoice;
static FakeVoice voices[64];
static int nfreed;
static float lis_pos[3], lis_front[3] = {0, 0, 1}, lis_top[3] = {0, 1, 0};
static int lis_have = 1;

static void *f_new(int ch, uint32_t rate, ModSoundFill fill, void *ctx) {
    for (int i = 0; i < 64; i++)
        if (!voices[i].used) {
            voices[i] = (FakeVoice){1, 0, ch, rate, fill, ctx, {{0}}};
            return &voices[i];
        }
    return NULL;
}
static void f_free(void *v) { ((FakeVoice *)v)->used = 0, nfreed++; }
static void f_play(void *v, int on) { ((FakeVoice *)v)->playing = on; }
static int f_playing(const void *v) { return ((const FakeVoice *)v)->playing; }
static void f_gains(void *v, const float g[2][2]) { memcpy(((FakeVoice *)v)->g, g, sizeof(float) * 4); }
static int f_listener(float p[3], float f[3], float t[3]) {
    memcpy(p, lis_pos, 12), memcpy(f, lis_front, 12), memcpy(t, lis_top, 12);
    return lis_have;
}

static void wav(const char *path, int ch, int rate, int frames) {
    FILE *f = fopen(path, "wb");
    const uint32_t bytes = (uint32_t)(frames * ch * 2);
    uint8_t h[44] = {0};
    const uint32_t v[] = {36 + bytes, 16, (uint32_t)rate, (uint32_t)(rate * ch * 2), bytes};
    memcpy(h, "RIFF", 4), memcpy(h + 8, "WAVEfmt ", 8), memcpy(h + 36, "data", 4);
    for (int k = 0; k < 4; k++) h[4 + k] = (uint8_t)(v[0] >> (8 * k)), h[16 + k] = (uint8_t)(v[1] >> (8 * k)), h[24 + k] = (uint8_t)(v[2] >> (8 * k)),
                                h[28 + k] = (uint8_t)(v[3] >> (8 * k)), h[40 + k] = (uint8_t)(v[4] >> (8 * k));
    h[20] = 1, h[22] = (uint8_t)ch, h[32] = (uint8_t)(ch * 2), h[34] = 16;
    fwrite(h, 1, 44, f);
    for (int i = 0; i < frames * ch; i++) {
        int16_t s = (int16_t)(i + 1);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "/tmp";
    char dir[600], path[800], err[200];
    snprintf(dir, sizeof dir, "%s/modsound_cache", tmp);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/knock.wav", dir), wav(path, 1, 22050, 100);
    snprintf(path, sizeof path, "%s/song.wav", dir), wav(path, 2, 44100, 50);
    snprintf(path, sizeof path, "%s/broken.wav", dir);
    FILE *f = fopen(path, "wb");
    fputs("RIFF not really", f), fclose(f);
    snprintf(path, sizeof path, "%s/crate.wmesh", dir);
    f = fopen(path, "wb"), fclose(f);

    /* no audio wired: loading works, playing refuses */
    CHECK(modsound_add_dir("crate", dir) == 2 && modsound_count("crate") == 2 && modsound_count(NULL) == 2, "two sounds loaded, the broken file and other files skipped");
    CHECK(!modsound_play("crate", "knock", NULL, 1, 0, err, sizeof err) && strstr(err, "no audio"), "no audio: nil");
    const ModSoundAudio a = {f_new, f_free, f_play, f_playing, f_gains, f_listener, NULL, NULL};
    modsound_setup(&a);

    /* 2D */
    int h = modsound_play("crate", "knock", NULL, 0.5f, 0, err, sizeof err);
    FakeVoice *v = &voices[0];
    CHECK(h > 0 && v->used && v->playing && v->channels == 1 && v->rate == 22050, "2D: a playing mono voice at the file's rate");
    CHECK(fabsf(v->g[0][0] - 0.5f * 0.7071f) < 1e-4f && fabsf(v->g[0][1] - v->g[0][0]) < 1e-6f, "2D: centred at the volume");
    int16_t buf[300];
    CHECK(v->fill(v->ctx, buf, 60) == 60 && buf[0] == 1 && buf[59] == 60 && v->fill(v->ctx, buf, 60) == 40 && v->fill(v->ctx, buf, 60) == 0,
          "fill: the samples, then the end");
    /* sandbox: another mod's name or handle */
    CHECK(!modsound_play("other", "knock", NULL, 1, 0, err, sizeof err) && strstr(err, "no sound knock"), "another mod cannot play crate's sound");
    CHECK(modsound_stop("other", h) == -1 && modsound_move("other", h, (float[3]){0, 0, 0}) == -1 && modsound_stop("crate", 999) == -1, "bad handles refused");
    CHECK(!modsound_play("crate", "nothere", NULL, 1, 0, err, sizeof err) && strstr(err, "a [sound] of the mod's assets.ini"), "unknown name");
    /* finished voices are freed by the tick */
    v->playing = 0;
    nfreed = 0;
    modsound_tick();
    CHECK(nfreed == 1 && modsound_playing("crate") == 0, "a finished sound is freed");
    /* loop */
    h = modsound_play("crate", "knock", NULL, 1, 1, err, sizeof err);
    v = &voices[0];
    CHECK(h > 0 && v->fill(v->ctx, buf, 250) == 250 && buf[100] == 1, "loop: wraps to the start");
    modsound_stop("crate", h);
    /* 3D: a source 4 m to the listener's right (audio axes: front +z, top +y, right +x) */
    lis_pos[0] = lis_pos[1] = lis_pos[2] = 0;
    h = modsound_play("crate", "knock", (float[3]){4, 0, 0}, 1, 0, err, sizeof err);   /* world (x, y, z) = audio (x, z, y) */
    v = &voices[0];
    CHECK(h > 0 && v->g[0][1] > 0.45f && v->g[0][0] < 0.01f, "3D: to the right, half level at twice the full-level distance");
    CHECK(!modsound_move("crate", h, (float[3]){-1, 0, 0}) && v->g[0][0] > 0.99f && v->g[0][1] < 0.01f, "move: now left, full level within 2 m");
    lis_pos[0] = 100;
    modsound_tick();
    CHECK(v->g[0][0] == 0 && v->g[0][1] == 0, "tick: the listener moved away (> 40 m): silent");
    lis_have = 0;
    modsound_tick();
    CHECK(v->g[0][0] > 0.7f && fabsf(v->g[0][0] - v->g[0][1]) < 1e-6f, "no listener: centred");
    lis_have = 1;
    /* stereo 3D: each channel to its own side */
    float g[2][2];
    modsound_gains((float[3]){0, 0, 0}, (float[3]){0, 0, 1}, (float[3]){0, 1, 0}, (float[3]){0, 0, 1}, 1, 2, g);
    CHECK(fabsf(g[0][0] - 0.7071f) < 1e-3f && g[0][1] == 0 && g[1][0] == 0 && fabsf(g[1][1] - 0.7071f) < 1e-3f, "stereo in front: L to L, R to R, centred");
    modsound_gains((float[3]){0, 0, 0}, (float[3]){0, 0, 1}, (float[3]){0, 1, 0}, (float[3]){0, 0, 30}, 1, 1, g);
    CHECK(g[0][0] > 0 && g[0][0] < 0.04f, "30 m: faded towards 40 m");
    /* budget: 32 playing per mod */
    modsound_new_state();
    int ok = 1;
    for (int i = 0; i < MODSOUND_PLAYING_MAX; i++) ok &= modsound_play("crate", "song", NULL, 1, 1, err, sizeof err) > 0;
    CHECK(ok && !modsound_play("crate", "song", NULL, 1, 1, err, sizeof err) && strstr(err, "budget"), "33rd sound refused");
    nfreed = 0;
    modsound_new_state();
    CHECK(nfreed == MODSOUND_PLAYING_MAX && modsound_playing("crate") == 0, "a new Lua state stops every sound");
    /* the per-mod sample budget */
    char big[600];
    snprintf(big, sizeof big, "%s/modsound_big", tmp);
    mkdir(big, 0755);
    snprintf(path, sizeof path, "%s/a.wav", big), wav(path, 2, 48000, (int)(MODSOUND_BYTES_MAX / 4) - 10);
    snprintf(path, sizeof path, "%s/b.wav", big), wav(path, 1, 48000, 100);
    CHECK(modsound_add_dir("big", big) == 1, "a mod's sounds past 16 MiB are skipped");
    modsound_reset();
    CHECK(modsound_count(NULL) == 0, "reset");
    if (failures) {
        printf("modsound_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("modsound_test: all passed\n");
    return 0;
}
