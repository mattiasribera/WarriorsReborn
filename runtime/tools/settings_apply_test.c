/* settings_apply: class (a) changes are applied once at the next
 * frame boundary through their registered setter; b / c / d changes and values a setter refuses
 * are not live (recorded for the restart). */
#include "../src/settings_apply.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int vol_calls, sharp_calls;
static char vol_last[32], sharp_last[32];
static void set_vol(const char *v) { vol_calls++, snprintf(vol_last, sizeof vol_last, "%s", v); }
static void set_sharp(const char *v) { sharp_calls++, snprintf(sharp_last, sizeof sharp_last, "%s", v); }
static int occl_can(const char *v) { return strcmp(v, "true") != 0; }   /* on only when it started on: here it did not */
static int occl_calls;
static void set_occl(const char *v) { (void)v, occl_calls++; }

static const SaLive table[] = {
    {"music_volume", NULL, set_vol},
    {"taa_sharpness", NULL, set_sharp},
    {"audio_occlusion", occl_can, set_occl},
    {"bloom", NULL, set_vol},   /* a setter for a class b key never makes it live */
};

int main(void) {
    SmValues start;
    sm_values_init(&start);
    sm_set(&start, "taa", "true");
    sm_set(&start, "audio_occlusion", "false");
    sa_register(table, (int)(sizeof table / sizeof table[0]));

    CHECK(sa_request(&start, "music_volume", "-6") == 1 && sa_queued() == 1, "class a with a setter: queued");
    CHECK(vol_calls == 0, "nothing applied before the frame boundary");
    CHECK(sa_request(&start, "music_volume", "-8") == 1 && sa_queued() == 1, "a newer value replaces the queued one");
    CHECK(sa_request(&start, "taa_sharpness", "0.8") == 1 && sa_queued() == 2, "a second key queued");
    CHECK(sa_frame() == 2 && vol_calls == 1 && !strcmp(vol_last, "-8") && sharp_calls == 1 && !strcmp(sharp_last, "0.8"),
          "the frame boundary applies each key once with its newest value");
    CHECK(sa_frame() == 0 && vol_calls == 1 && sa_queued() == 0, "applied once: the next frame applies nothing");

    CHECK(sa_request(&start, "bloom", "true") == 0 && sa_queued() == 0, "class b (bloom): not live, for the restart");
    CHECK(sa_request(&start, "msaa", "4") == 0, "class c (msaa): not live");
    CHECK(sa_request(&start, "mods", "a,b") == 0, "class d (mods): not live");
    CHECK(sa_request(&start, "rumble", "false") == 0, "class a without a setter: not live");
    CHECK(sa_request(&start, "audio_occlusion", "true") == 0 && !sa_can_live(&start, "audio_occlusion", "true"),
          "a setter that refuses the value: not live");
    CHECK(sa_request(&start, "nope", "1") == 0, "an unknown key: not live");
    CHECK(sa_can_live(&start, "music_volume", "3") && sa_queued() == 0, "sa_can_live queues nothing");
    sa_frame();
    CHECK(occl_calls == 0 && vol_calls == 1, "refused and non-live changes never reach a setter");

    /* the temporal upscaling mode while it was on at start is live (sm_change_class) */
    sm_set(&start, "taa_upscale", "quality");
    static const SaLive up[] = {{"taa_upscale", NULL, set_vol}};
    sa_register(up, 1);
    vol_calls = 0;
    CHECK(sa_request(&start, "taa_upscale", "performance") == 1 && sa_frame() == 1 && vol_calls == 1, "upscaling mode while on: live");
    CHECK(sa_request(&start, "taa_upscale", "off") == 0, "upscaling off: not live");

    sa_reset();
    CHECK(sa_request(&start, "music_volume", "1") == 0, "after reset: no setters");
    sm_values_free(&start);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
