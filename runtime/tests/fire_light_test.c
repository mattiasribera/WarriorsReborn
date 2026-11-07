/* Game-independent test of --fire-light (src/fire_light.c): the option parser, flame quads as
 * sprites, the per-fire clustering, the flicker and jitter (bounded, smooth, frame-rate independent)
 * and the tracked fires as lights. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/fire_light.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    FireLightConfig c = FIRE_LIGHT_DEFAULTS;
    CHECK(!c.on && c.strength == FIRE_STRENGTH_DEFAULT, "defaults: off, strength %.2f", c.strength);
    CHECK(fire_light_arg(&c, "--lamp-lights", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(fire_light_arg(&c, "--fire-light", NULL) == 1 && c.on, "--fire-light turns it on");
    CHECK(fire_light_arg(&c, "--no-fire-light", NULL) == 1 && !c.on, "--no-fire-light turns it off");
    CHECK(fire_light_arg(&c, "--fire-light-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(fire_light_arg(&c, "--fire-light-strength", "0.1") == 2 && fire_light_arg(&c, "--fire-light-strength", "3") == 2 && c.strength == 3.0f,
          "strength range 0.1-3");
    const char *bad[] = {"0", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        FireLightConfig d = FIRE_LIGHT_DEFAULTS;
        CHECK(fire_light_arg(&d, "--fire-light-strength", bad[i]) == -1 && d.strength == FIRE_STRENGTH_DEFAULT, "strength '%s' is rejected",
              bad[i] ? bad[i] : "(missing)");
    }
}

static void test_sprite(void) {
    const float q[6][3] = {{0, 0, 1}, {0.4f, 0, 1}, {0.4f, 0, 1.6f}, {0, 0, 1}, {0.4f, 0, 1.6f}, {0, 0, 1.6f}};
    const uint32_t c[6] = {0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu};
    LampSprite s;
    CHECK(fire_sprite_quad(q, c, 6, &s) == 0, "a flame quad is a sprite");
    CHECK(fabsf(s.pos[0] - 1.2f / 6) < 1e-5f && fabsf(s.pos[2] - 7.8f / 6) < 1e-5f, "centre: vertex mean %.3f %.3f", s.pos[0], s.pos[2]);
    CHECK(fabsf(s.size - 0.6f) < 1e-5f, "size: largest extent %.3f", s.size);
    CHECK(s.weight == 1.0f, "weight 1 whatever the alpha (the flame animation fades its quads; the fire does not dim): %.3f", s.weight);
    CHECK(s.rgb[0] == FIRE_R && s.rgb[1] == FIRE_G && s.rgb[2] == FIRE_B, "colour: the fire's, not the vertex diffuse");
    const uint32_t gone[6] = {0x02FFFFFFu, 0x02FFFFFFu, 0x02FFFFFFu, 0x02FFFFFFu, 0x02FFFFFFu, 0x02FFFFFFu};
    CHECK(fire_sprite_quad(q, gone, 6, &s) == -1, "a faded-out flame is no light");
    const float pt[4][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    CHECK(fire_sprite_quad(pt, c, 4, &s) == -1, "a degenerate quad is no light");
    CHECK(fire_sprite_quad(q, c, 2, &s) == -1 && fire_sprite_quad(q, c, 7, &s) == -1, "3-6 vertices only");
}

static void test_texture(void) {
    CHECK(fire_light_texture(FIRE_TEX_FLAMES) && fire_light_texture(FIRE_TEX_FLAME_STRIP), "the flame textures are fire");
    CHECK(!fire_light_texture(0) && !fire_light_texture(0x886a4c14094e9597ull), "other textures are not");
}

static LampSprite spr(float x, float y, float z, float w) {
    LampSprite s = {{x, y, z}, 0.4f, {FIRE_R, FIRE_G, FIRE_B}, w};
    return s;
}

static void test_cluster(void) {
    /* Barrel A: five flames rising over 1 m, drifting 0.3 m; barrel B 3 m away: two flames. */
    LampSprite s[7] = {spr(0, 0, 1, 1), spr(5, 0, 1.2f, 1), spr(0.2f, 0.1f, 1.4f, 1), spr(-0.1f, 0.3f, 1.8f, 1),
                       spr(5.1f, 0, 1.6f, 1), spr(0.3f, -0.2f, 2.0f, 0.5f), spr(0, 0, 1.2f, 1)};
    s[1].pos[0] = 3;
    s[4].pos[0] = 3.1f;
    uint32_t n = fire_cluster(s, 7);
    CHECK(n == 2, "7 flames of 2 barrels 3 m apart -> %u fires", n);
    const int a = s[0].pos[0] < 1.5f ? 0 : 1, b = 1 - a;
    CHECK(fabsf(s[a].weight - 4.5f) < 1e-4f && fabsf(s[b].weight - 2) < 1e-4f, "summed weights %.2f, %.2f", s[a].weight, s[b].weight);
    CHECK(fabsf(s[b].pos[0] - 3.05f) < 1e-4f, "merged centre %.3f", s[b].pos[0]);
    /* Order independence (the same flames reversed). */
    LampSprite r[7] = {spr(0, 0, 1.2f, 1), spr(0.3f, -0.2f, 2.0f, 0.5f), spr(3.1f, 0, 1.6f, 1), spr(-0.1f, 0.3f, 1.8f, 1),
                       spr(0.2f, 0.1f, 1.4f, 1), spr(3, 0, 1.2f, 1), spr(0, 0, 1, 1)};
    n = fire_cluster(r, 7);
    const int ra = r[0].pos[0] < 1.5f ? 0 : 1;
    CHECK(n == 2 && fabsf(r[ra].pos[0] - s[a].pos[0]) < 1e-4f && fabsf(r[ra].pos[2] - s[a].pos[2]) < 1e-4f, "order-independent");
    CHECK(fire_cluster(s, 0) == 0 && fire_cluster(s, 1) == 1, "0 and 1 sprite");
}

static void test_flicker(void) {
    double lo = 1, hi = 0, sum = 0, sum2 = 0, maxstep = 0;
    const int N = 60 * 120;   /* two minutes at 60 Hz */
    for (int i = 0; i < N; i++) {
        const double t = i / 60.0;
        const float f = fire_flicker(7, t);
        lo = f < lo ? f : lo;
        hi = f > hi ? f : hi;
        sum += f;
        sum2 += (double)f * f;
        if (i) {
            const double d = fabs(f - fire_flicker(7, t - 1 / 60.0));
            maxstep = d > maxstep ? d : maxstep;
        }
    }
    const double mean = sum / N, sd = sqrt(sum2 / N - mean * mean);
    CHECK(lo >= 1 - FIRE_FLICKER_AMP - 1e-6 && hi <= 1 + 1e-6, "within [1 - amp, 1]: %.3f..%.3f", lo, hi);
    CHECK(hi - lo > FIRE_FLICKER_AMP * 0.5, "it does flicker: range %.3f", hi - lo);
    CHECK(sd > 0.03, "and not just once: sd %.3f", sd);
    CHECK(maxstep < 0.08, "smooth: largest step between 60 Hz frames %.3f", maxstep);
    /* Frame-rate independent: a 30 Hz frame sees what a 60 Hz frame sees at the same time. */
    int same = 1;
    for (int i = 0; i < 300; i++) same &= fire_flicker(3, i / 30.0) == fire_flicker(3, (2 * i) / 60.0);
    CHECK(same, "the same value at the same time at 30 and 60 Hz");
    CHECK(fire_flicker(3, 12.345) == fire_flicker(3, 12.345), "deterministic");
    int differ = 0;
    for (int i = 0; i < 100; i++) differ += fabsf(fire_flicker(1, i * 0.1) - fire_flicker(2, i * 0.1)) > 0.01f;
    CHECK(differ > 50, "fires do not flicker in step: %d of 100 samples differ", differ);
    CHECK(fire_flicker(5, 1e6) >= 1 - FIRE_FLICKER_AMP - 1e-6 && fire_flicker(5, 1e6) <= 1, "bounded after hours");
}

static void test_jitter(void) {
    float maxa = 0, maxstep = 0, prev[3];
    for (int i = 0; i < 3600; i++) {
        float o[3];
        fire_jitter(9, i / 60.0, o);
        for (int a = 0; a < 3; a++) {
            maxa = fabsf(o[a]) > maxa ? fabsf(o[a]) : maxa;
            if (i) maxstep = fabsf(o[a] - prev[a]) > maxstep ? fabsf(o[a] - prev[a]) : maxstep;
            prev[a] = o[a];
        }
    }
    CHECK(maxa <= FIRE_JITTER + 1e-6f && maxa > FIRE_JITTER * 0.3f, "within +-%.2f and moving: %.3f", FIRE_JITTER, maxa);
    CHECK(maxstep < 0.03f, "smooth: largest step %.4f", maxstep);
}

static void test_shade(void) {
    LampLight l[2] = {{{1, 2, 3}, 9, {1, 1, 1}, 1.0f, 4}, {{10, 0, 0}, 9, {1, 1, 1}, 0.5f, 5}};
    fire_light_shade(l, 2, 3.5, 2.0f);
    const float f0 = fire_flicker(4, 3.5), f1 = fire_flicker(5, 3.5);
    CHECK(l[0].rgb[0] == FIRE_R && l[0].rgb[1] == FIRE_G && l[0].rgb[2] == FIRE_B, "fire colour");
    CHECK(fabsf(l[0].intensity - FIRE_INTENSITY * f0 * 2.0f) < 1e-5f, "intensity %.3f = base x flicker x strength", l[0].intensity);
    CHECK(fabsf(l[1].intensity - FIRE_INTENSITY * 0.5f * f1 * 2.0f) < 1e-5f, "keeps the track's fade %.3f", l[1].intensity);
    CHECK(l[0].radius > FIRE_RADIUS * 0.9f && l[0].radius <= FIRE_RADIUS, "radius %.2f", l[0].radius);
    CHECK(fabsf(l[0].pos[0] - 1) <= FIRE_JITTER && fabsf(l[0].pos[2] - 3) <= FIRE_JITTER, "jittered position %.3f %.3f", l[0].pos[0], l[0].pos[2]);
    CHECK(l[0].id == 4 && l[1].id == 5, "ids kept");
}

/* End to end through the shared tracker: two barrels, the nearest FIRE_MAX_LIGHTS kept. */
static void test_track(void) {
    LampTracker k;
    lamp_tracker_reset(&k);
    LampSprite s[12];
    for (int i = 0; i < 12; i++) s[i] = spr(4.0f * i, 0, 1, 1);
    const float eye[3] = {0, -2, 1.7f};
    LampLight out[LAMP_MAX_LIGHTS];
    uint32_t n = 0;
    for (int f = 0; f < 20; f++) n = lamp_track_update(&k, s, 12, eye, out, FIRE_MAX_LIGHTS, NULL, NULL);
    CHECK(n == FIRE_MAX_LIGHTS, "capped at %u: %u", FIRE_MAX_LIGHTS, n);
    float far = 0;
    for (uint32_t i = 0; i < n; i++) far = out[i].pos[0] > far ? out[i].pos[0] : far;
    CHECK(far < 4.0f * FIRE_MAX_LIGHTS, "the nearest fires win: farthest kept at x %.1f", far);
}

int main(void) {
    test_options();
    test_sprite();
    test_texture();
    test_cluster();
    test_flicker();
    test_jitter();
    test_shade();
    test_track();
    if (fails) {
        printf("fire_light_test: %d FAILED\n", fails);
        return 1;
    }
    printf("fire_light_test: ALL OK\n");
    return 0;
}
