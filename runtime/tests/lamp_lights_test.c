/* Game-independent test of the --lamp-lights options, the halo sprite -> light list building
 * (clustering, tracking: stable ids, fades, cap) and the falloff (src/lamp_lights.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/lamp_lights.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static LampSprite sprite(float x, float y, float z, float size, float r, float g, float b, float w) {
    LampSprite s = {{x, y, z}, size, {r, g, b}, w};
    return s;
}
static const LampLight *find_id(const LampLight *l, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++)
        if (l[i].id == id) return &l[i];
    return NULL;
}

static void test_options(void) {
    LampLightsConfig c = LAMP_LIGHTS_DEFAULTS;
    CHECK(!c.on && c.strength == LAMP_STRENGTH_DEFAULT, "defaults: off, strength %.2f", c.strength);
    CHECK(lamp_lights_arg(&c, "--ssao", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(lamp_lights_arg(&c, "--lamp-lights", NULL) == 1 && c.on, "--lamp-lights turns it on");
    CHECK(lamp_lights_arg(&c, "--no-lamp-lights", NULL) == 1 && !c.on, "--no-lamp-lights turns it off");
    CHECK(lamp_lights_arg(&c, "--lamp-lights-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(lamp_lights_arg(&c, "--lamp-lights-strength", "0.1") == 2 && lamp_lights_arg(&c, "--lamp-lights-strength", "3") == 2 &&
              c.strength == 3.0f,
          "strength range 0.1-3");
    const char *bad[] = {"0", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        LampLightsConfig d = LAMP_LIGHTS_DEFAULTS;
        CHECK(lamp_lights_arg(&d, "--lamp-lights-strength", bad[i]) == -1 && d.strength == LAMP_STRENGTH_DEFAULT,
              "strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
}

static void test_sprite(void) {
    const float q[4][3] = {{0, 0, 0}, {2, 0, 0}, {2, 0, 2}, {0, 0, 2}};
    const uint32_t warm[4] = {0x80FFC080u, 0x80FFC080u, 0x80FFC080u, 0x80FFC080u};
    LampSprite s;
    CHECK(lamp_sprite_quad(q, warm, 4, &s) == 0, "a lit quad is a sprite");
    CHECK(fabsf(s.pos[0] - 1) < 1e-5f && fabsf(s.pos[1]) < 1e-5f && fabsf(s.pos[2] - 1) < 1e-5f, "centre %.2f %.2f %.2f", s.pos[0], s.pos[1],
          s.pos[2]);
    CHECK(fabsf(s.size - 2) < 1e-5f, "size: largest extent %.2f", s.size);
    CHECK(fabsf(s.rgb[0] - 1) < 1e-3f && fabsf(s.rgb[1] - 192 / 255.0f) < 1e-3f && fabsf(s.rgb[2] - 128 / 255.0f) < 1e-3f, "colour %.3f %.3f %.3f",
          s.rgb[0], s.rgb[1], s.rgb[2]);
    CHECK(fabsf(s.weight - 128 / 255.0f) < 1e-3f, "weight: alpha x brightest channel %.3f", s.weight);
    const uint32_t dim[4] = {0x05FFFFFFu, 0x05FFFFFFu, 0x05FFFFFFu, 0x05FFFFFFu};
    CHECK(lamp_sprite_quad(q, dim, 4, &s) == -1, "a faded quad is no light");
    const uint32_t black[4] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    CHECK(lamp_sprite_quad(q, black, 4, &s) == -1, "a black quad is no light");
    const uint32_t haze[4] = {0x96515356u, 0x96515356u, 0x96515356u, 0x96515356u};   /* level51 haze plume: grey, alpha 150 */
    CHECK(lamp_sprite_quad(q, haze, 4, &s) == -1, "haze is no light");
    const uint32_t smoke[4] = {0xE0808080u, 0xE0808080u, 0xE0808080u, 0xE0808080u};   /* grey 128, alpha 224: peak 0.5 */
    CHECK(lamp_sprite_quad(q, smoke, 4, &s) == -1, "grey smoke is no light");
    const uint32_t cone[4] = {0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu};   /* light cone: white, alpha 128 */
    const float small[4][3] = {{0, 0, 0}, {0.65f, 0, 0}, {0.65f, 0.65f, 0}, {0, 0.65f, 0}};
    CHECK(lamp_sprite_quad(small, cone, 4, &s) == 0, "a light cone on the ground is a light");
    const float spark[4][3] = {{0, 0, 0}, {0.1f, 0, 0}, {0.1f, 0.1f, 0}, {0, 0.1f, 0}};
    const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    CHECK(lamp_sprite_quad(spark, white, 4, &s) == -1, "a spark is no light");
    const float pt[4][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    CHECK(lamp_sprite_quad(pt, warm, 4, &s) == -1, "a degenerate quad is no light");
    CHECK(lamp_sprite_quad(q, warm, 2, &s) == -1 && lamp_sprite_quad(q, warm, 7, &s) == -1, "3-6 vertices only");
}

static void test_cluster(void) {
    /* Two quads of one lamp (halo and cone, overlapping), one lamp far away. */
    LampSprite s[3] = {sprite(0, 0, 3, 1, 1, 0.8f, 0.5f, 0.6f), sprite(10, 0, 3, 1, 0, 0, 1, 0.5f), sprite(0.4f, 0, 3, 0.5f, 1, 0.8f, 0.5f, 0.2f)};
    uint32_t n = lamp_cluster(s, 3);
    CHECK(n == 2, "dedup: 3 quads of 2 lamps -> %u", n);
    int a = fabsf(s[0].pos[0]) < 1 ? 0 : 1, b = 1 - a;
    CHECK(fabsf(s[a].pos[0] - 0.1f) < 1e-4f, "merged centre weighted by weight %.3f", s[a].pos[0]);
    CHECK(fabsf(s[a].weight - 0.8f) < 1e-5f && fabsf(s[a].size - 1) < 1e-5f, "merged weight %.2f (sum), size %.2f (largest)", s[a].weight, s[a].size);
    CHECK(fabsf(s[a].rgb[0] - 1) < 1e-5f && fabsf(s[a].rgb[1] - 0.8f) < 1e-5f, "merged colour kept");
    CHECK(fabsf(s[b].pos[0] - 10) < 1e-5f && s[b].weight == 0.5f, "the far lamp stays alone");
    /* Order independence. */
    LampSprite r[3] = {sprite(0.4f, 0, 3, 0.5f, 1, 0.8f, 0.5f, 0.2f), sprite(10, 0, 3, 1, 0, 0, 1, 0.5f), sprite(0, 0, 3, 1, 1, 0.8f, 0.5f, 0.6f)};
    CHECK(lamp_cluster(r, 3) == 2, "order-independent count");
    CHECK(lamp_cluster(s, 0) == 0, "empty");
}

static void test_radius_falloff(void) {
    CHECK(lamp_radius(1) == LAMP_RADIUS_SCALE, "radius from size %.2f", lamp_radius(1));
    CHECK(lamp_radius(0.001f) == LAMP_RADIUS_MIN && lamp_radius(1000) == LAMP_RADIUS_MAX, "radius clamped");
    CHECK(fabsf(lamp_falloff(0, 4) - 1) < 1e-6f, "falloff 1 at the light");
    CHECK(lamp_falloff(4, 4) == 0 && lamp_falloff(5, 4) == 0, "0 at and beyond the radius");
    float prev = 2;
    int mono = 1;
    for (int i = 0; i <= 100; i++) {
        float f = lamp_falloff(4 * i / 100.0f, 4);
        mono &= f <= prev && f >= 0;
        prev = f;
    }
    CHECK(mono, "monotonic, never negative");
    CHECK(lamp_falloff(3.96f, 4) < 1e-3f, "smooth to zero at the edge (%.5f)", lamp_falloff(3.96f, 4));
    const float x = 0.5f, e = (1 - x * x) * (1 - x * x) / (1 + LAMP_FALLOFF_K * x * x);
    CHECK(fabsf(lamp_falloff(2, 4) - e) < 1e-6f, "formula at half the radius %.4f vs %.4f", lamp_falloff(2, 4), e);
    CHECK(lamp_falloff(1, 0) == 0, "no radius: no light");
}

static void test_track(void) {
    LampTracker k;
    lamp_tracker_reset(&k);
    const float eye[3] = {0, 0, 0};
    LampLight out[LAMP_MAX_LIGHTS];
    LampSprite s[2] = {sprite(5, 0, 3, 1, 1, 0.8f, 0.5f, 0.6f), sprite(-5, 0, 3, 1, 0, 0, 1, 0.5f)};
    uint32_t n = lamp_track_update(&k, s, 2, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 0, "new lights start at level 0 (fade in): %u", n);
    n = lamp_track_update(&k, s, 2, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 2, "then they show: %u", n);
    const uint32_t id0 = out[0].id, id1 = out[1].id;
    CHECK(id0 != id1, "distinct ids");
    const float i1 = out[0].intensity;
    n = lamp_track_update(&k, s, 2, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 2 && out[0].intensity > i1, "fading in (%.3f -> %.3f)", i1, out[0].intensity);
    /* Stability: the halo jitters a little (particle motion) and the order of sprites changes. */
    for (int f = 0; f < 20; f++) {
        LampSprite j[2] = {sprite(-5 + 0.05f * (f & 1), 0, 3, 1, 0, 0, 1, 0.5f), sprite(5 - 0.05f * (f & 1), 0, 3.02f, 1.1f, 1, 0.8f, 0.5f, 0.6f)};
        n = lamp_track_update(&k, j, 2, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    }
    const LampLight *a = find_id(out, n, id0), *b = find_id(out, n, id1);
    CHECK(n == 2 && a && b, "same ids after jitter and reordering");
    CHECK(a && b && fabsf(fabsf(a->pos[0]) - 5) < 0.1f && fabsf(fabsf(b->pos[0]) - 5) < 0.1f && a->pos[0] * b->pos[0] < 0, "positions kept");
    CHECK(a && fabsf(a->intensity - (a->pos[0] > 0 ? 0.6f : 0.5f)) < 0.02f, "full intensity reached %.3f", a ? a->intensity : -1);
    CHECK(b && fabsf(b->radius - lamp_radius(b->pos[0] > 0 ? 1.1f : 1)) < 0.05f, "radius from size %.2f", b ? b->radius : -1);
    /* One lamp's halo vanishes for a frame (culled particle): fades, keeps its id, comes back. */
    const uint32_t lost = a && a->pos[0] > 0 ? id1 : id0;   /* s[0] is the lamp at +5: the one at -5 goes */
    n = lamp_track_update(&k, s, 1, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    const LampLight *fl = find_id(out, n, lost);
    CHECK(n == 2 && fl && fl->intensity < 0.5f && fl->intensity > 0.3f, "a missing light fades instead of popping (n %u, %.3f)", n,
          fl ? fl->intensity : -1);
    n = lamp_track_update(&k, s, 2, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 2 && find_id(out, n, id0) && find_id(out, n, id1), "and keeps its id when it comes back");
    /* Gone for good: dropped after fading out. */
    for (int f = 0; f < 12; f++) n = lamp_track_update(&k, s, 1, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 1 && k.n == 1, "dropped after fading out (n %u, tracks %u)", n, k.n);
    /* A new sprite elsewhere gets a new id. */
    LampSprite c = sprite(0, 20, 3, 1, 1, 1, 1, 0.5f);
    lamp_track_update(&k, &c, 1, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    n = lamp_track_update(&k, &c, 1, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    const LampLight *nl = NULL;
    for (uint32_t i = 0; i < n; i++)
        if (fabsf(out[i].pos[1] - 20) < 1e-3f) nl = &out[i];
    CHECK(nl && nl->id != id0 && nl->id != id1, "a new light gets a new id (%u)", nl ? nl->id : 0);
}

/* in_view for the hold test: everything with x > 0 is on screen. */
static int right_half(const float pos[3], void *ctx) {
    (void)ctx;
    return pos[0] > 0;
}
static void test_hold(void) {
    LampTracker k;
    lamp_tracker_reset(&k);
    const float eye[3] = {0, 0, 0};
    LampLight out[LAMP_MAX_LIGHTS];
    LampSprite s[2] = {sprite(5, 0, 3, 1, 1, 1, 1, 1), sprite(-5, 0, 3, 1, 1, 1, 1, 1)};
    uint32_t n = 0;
    for (int f = 0; f < 10; f++) n = lamp_track_update(&k, s, 2, eye, out, LAMP_MAX_LIGHTS, right_half, NULL);
    CHECK(n == 2, "two lights (%u)", n);
    /* Both halos are gone: the one on screen fades, the one off screen (the title draws no halo
     * there) still lights what is on screen near it, unchanged. */
    for (int f = 0; f < 12; f++) n = lamp_track_update(&k, s, 0, eye, out, LAMP_MAX_LIGHTS, right_half, NULL);
    CHECK(n == 1 && out[0].pos[0] < 0 && fabsf(out[0].intensity - 1) < 1e-3f, "off screen: held (n %u, x %.1f, %.2f)", n, n ? out[0].pos[0] : 0,
          n ? out[0].intensity : 0);
    for (int f = 0; f < LAMP_HOLD; f++) n = lamp_track_update(&k, s, 0, eye, out, LAMP_MAX_LIGHTS, right_half, NULL);
    CHECK(n == 0, "but not forever (LAMP_HOLD frames, then it fades: %u)", n);
}

static void test_cap(void) {
    LampTracker k;
    lamp_tracker_reset(&k);
    const float eye[3] = {0, 0, 0};
    LampLight out[LAMP_MAX_LIGHTS];
    enum { N = 60 };
    LampSprite s[N];
    for (int i = 0; i < N; i++) s[i] = sprite(3.0f * (i + 1), 0, 0, 1, 1, 1, 1, 0.5f);   /* farther and farther */
    uint32_t n = 0;
    for (int f = 0; f < 10; f++) n = lamp_track_update(&k, s, N, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == LAMP_MAX_LIGHTS, "cap: %u of %d lights", n, N);
    float far = 0;
    for (uint32_t i = 0; i < n; i++) far = out[i].pos[0] > far ? out[i].pos[0] : far;
    CHECK(far <= 3.0f * LAMP_MAX_LIGHTS + 0.01f, "the nearest are kept (farthest %.1f)", far);
    CHECK(lamp_track_update(&k, s, N, eye, out, 4, NULL, NULL) == 4, "a smaller cap");
    /* Hysteresis: two equal lights at nearly the same priority, cap 1: the shown one stays. */
    lamp_tracker_reset(&k);
    LampSprite two[2] = {sprite(10, 0, 0, 1, 1, 1, 1, 0.5f), sprite(-10, 0, 0, 1, 1, 1, 1, 0.5f)};
    for (int f = 0; f < 10; f++) n = lamp_track_update(&k, two, 2, eye, out, 1, NULL, NULL);
    const uint32_t shown = out[0].id;
    int flips = 0;
    for (int f = 0; f < 20; f++) {
        float e2[3] = {(f & 1) ? 0.3f : -0.3f, 0, 0};   /* the camera sways between them */
        n = lamp_track_update(&k, two, 2, e2, out, 1, NULL, NULL);
        flips += n == 1 && out[0].id != shown;
    }
    CHECK(flips == 0, "the shown light is kept against a near tie (%d flips)", flips);
    /* A lamp whose halo and glow merged (weight 1.3): its light is no brighter than one opaque halo. */
    lamp_tracker_reset(&k);
    LampSprite hot = sprite(4, 0, 0, 1, 1, 1, 1, 1.3f);
    for (int f = 0; f < 10; f++) n = lamp_track_update(&k, &hot, 1, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(n == 1 && out[0].intensity <= 1.0f + 1e-6f, "intensity capped at 1 (%.2f)", n ? out[0].intensity : -1);
    /* Too many sprites for the tracker: no overflow. */
    static LampSprite many[LAMP_TRACK_MAX + 40];
    for (int i = 0; i < LAMP_TRACK_MAX + 40; i++) many[i] = sprite(3.0f * i, 50, 0, 1, 1, 1, 1, 0.5f);
    lamp_track_update(&k, many, LAMP_TRACK_MAX + 40, eye, out, LAMP_MAX_LIGHTS, NULL, NULL);
    CHECK(k.n <= LAMP_TRACK_MAX, "tracks bounded (%u)", k.n);
}

int main(void) {
    test_options();
    test_sprite();
    test_cluster();
    test_radius_falloff();
    test_track();
    test_hold();
    test_cap();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
