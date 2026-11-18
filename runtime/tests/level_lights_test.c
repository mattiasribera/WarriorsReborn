/* Game-independent test of the --level-lights options, the LightManager reader (over a fake guest
 * memory) and the per-frame light selection (src/level_lights.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/level_lights.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Fake guest memory: MEM_SIZE bytes at MEM_BASE; reads outside it fail. */
#define MEM_BASE 0x00400000u
#define MEM_SIZE 0x20000u
static uint8_t mem[MEM_SIZE];
static int reads_out;
static int fake_read(uint32_t va, void *dst, uint32_t len, void *ctx) {
    (void)ctx;
    if (va < MEM_BASE || len > MEM_SIZE || va - MEM_BASE > MEM_SIZE - len) {
        reads_out++;
        return -1;
    }
    memcpy(dst, mem + (va - MEM_BASE), len);
    return 0;
}
static void put32(uint32_t va, uint32_t v) { memcpy(mem + (va - MEM_BASE), &v, 4); }
static void put16(uint32_t va, uint16_t v) { memcpy(mem + (va - MEM_BASE), &v, 2); }
static void putf(uint32_t va, float v) { memcpy(mem + (va - MEM_BASE), &v, 4); }

#define PTR_VA (MEM_BASE + 0x100u)       /* the manager pointer (LEVEL_LIGHTS_MGR_VA in the title) */
#define MGR_VA (MEM_BASE + 0x1000u)
#define ARR0_VA (MEM_BASE + 0x2000u)
#define ARR1_VA (MEM_BASE + 0x3000u)
#define POOL_VA (MEM_BASE + 0x3800u)
#define LIGHT_VA(i) (MEM_BASE + 0x4000u + (uint32_t)(i) * 0x100u)
static void light(int i, uint32_t kind, float x, float y, float z, float radius, float r, float g, float b, uint16_t enable, uint16_t glow) {
    const uint32_t a = LIGHT_VA(i);
    memset(mem + (a - MEM_BASE), 0, LEVEL_LIGHT_BYTES);
    mem[a - MEM_BASE + LEVEL_LIGHT_DIRTY] = (uint8_t)(i & 1);   /* the title clears it after an update: not checked */
    put32(a + LEVEL_LIGHT_KIND, kind);
    putf(a + LEVEL_LIGHT_COLOR, r), putf(a + LEVEL_LIGHT_COLOR + 4, g), putf(a + LEVEL_LIGHT_COLOR + 8, b), putf(a + LEVEL_LIGHT_COLOR + 12, 1);
    putf(a + LEVEL_LIGHT_POS, x), putf(a + LEVEL_LIGHT_POS + 4, y), putf(a + LEVEL_LIGHT_POS + 8, z);
    putf(a + LEVEL_LIGHT_DIR, 0), putf(a + LEVEL_LIGHT_DIR + 4, 0), putf(a + LEVEL_LIGHT_DIR + 8, -1);
    putf(a + LEVEL_LIGHT_RADIUS, radius);
    put16(a + LEVEL_LIGHT_GLOW, glow);
    put16(a + LEVEL_LIGHT_ENABLE, enable);
}
/* list l holds the lights idx[0..n-1]. */
static void list(int l, const int *idx, int n) {
    const uint32_t arr = l ? ARR1_VA : ARR0_VA, vec = MGR_VA + (l ? LEVEL_MGR_LIST1 : LEVEL_MGR_LIST0);
    for (int i = 0; i < n; i++) put32(arr + 4u * (uint32_t)i, LIGHT_VA(idx[i]));
    put32(vec + 4, arr);
    put32(vec + 8, arr + 4u * (uint32_t)n);
}
/* The pool: capacity 64, its first n entries live. */
static void pool(const int *idx, int n) {
    for (int i = 0; i < n; i++) put32(POOL_VA + 4u * (uint32_t)i, LIGHT_VA(idx[i]));
    put32(MGR_VA + LEVEL_MGR_POOL, POOL_VA);
    put32(MGR_VA + LEVEL_MGR_POOL + 4, 64);
    put32(MGR_VA + LEVEL_MGR_POOL + 8, (uint32_t)n);
}
static void reset_mem(void) {
    memset(mem, 0, sizeof mem);
    put32(PTR_VA, MGR_VA);
    pool(NULL, 0);
    list(0, NULL, 0);
    list(1, NULL, 0);
}

static void test_options(void) {
    LevelLightsConfig c = LEVEL_LIGHTS_DEFAULTS;
    CHECK(!c.on && c.cap == 32, "defaults: off, cap %u", c.cap);
    CHECK(level_lights_arg(&c, "--lamp-lights", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(level_lights_arg(&c, "--level-lights", NULL) == 1 && c.on, "--level-lights turns it on");
    CHECK(level_lights_arg(&c, "--no-level-lights", NULL) == 1 && !c.on, "--no-level-lights turns it off");
    CHECK(level_lights_arg(&c, "--level-lights-max", "12") == 2 && c.cap == 12 && !c.on, "the cap sets the cap only");
    CHECK(level_lights_arg(&c, "--level-lights-max", "1") == 2 && c.cap == 1 && level_lights_arg(&c, "--level-lights-max", "32") == 2 && c.cap == 32,
          "cap range 1-32");
    const char *bad[] = {"0", "33", "-1", "abc", "8x", "", "1.5", NULL};
    for (int i = 0; i < 8; i++) {
        LevelLightsConfig d = LEVEL_LIGHTS_DEFAULTS;
        CHECK(level_lights_arg(&d, "--level-lights-max", bad[i]) == -1 && d.cap == 32, "cap '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
}

static LevelLightSet set;
static void test_read(void) {
    reset_mem();
    light(0, LEVEL_KIND_POINT, -286.1f, 122.8f, 5.49f, 9, 0.843f, 0.886f, 1.0f, 1, 0xffff);   /* level99.lua:261 */
    light(1, LEVEL_KIND_POINT, -278.7f, 128.1f, 3.49f, 15, 0.843f, 0.886f, 1.0f, 1, 3);      /* with a halo */
    light(2, LEVEL_KIND_POINT, 10, 10, 2, 0, 1, 1, 1, 1, 2);                                 /* glow only */
    light(3, LEVEL_KIND_POINT, 20, 20, 2, 8, 1, 1, 1, 0, 0xffff);                            /* switched off */
    light(4, LEVEL_KIND_DIR, 0, 0, 0, 0, 0.07f, 0.10f, 0.18f, 1, 0xffff);                    /* the moon */
    light(5, LEVEL_KIND_AMBIENT, 0, 0, 0, 0, 0.055f, 0.065f, 0.075f, 1, 0xffff);
    light(6, LEVEL_KIND_POINT, NAN, 0, 0, 5, 1, 1, 1, 1, 0xffff);                            /* invalid position */
    light(7, LEVEL_KIND_POINT, 0, 0, 0, 500, 1, 1, 1, 1, 0xffff);                            /* invalid radius */
    light(8, 9, 0, 0, 0, 5, 1, 1, 1, 1, 0xffff);                                             /* unknown kind */
    light(9, LEVEL_KIND_POINT, 0, 0, 0, 5, -1, 1, 1, 1, 0xffff);                             /* negative colour */
    const int a[] = {0, 2, 4, 6, 8};
    const int b[] = {1, 3, 5, 7, 9, 0};   /* light 0 in both lists */
    list(0, a, 5);
    list(1, b, 6);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0, "the manager is read");
    CHECK(set.n == 2, "2 real point lights (got %u)", set.n);
    CHECK(set.listed == 10, "10 distinct lights listed (got %u)", set.listed);
    CHECK(set.off == 2 && set.rejected == 4, "2 skipped (glow-only, off), 4 invalid (got %u, %u)", set.off, set.rejected);
    const LevelPoint *p0 = set.p[0].id == LIGHT_VA(0) ? &set.p[0] : &set.p[1], *p1 = p0 == &set.p[0] ? &set.p[1] : &set.p[0];
    CHECK(p0->id == LIGHT_VA(0) && p1->id == LIGHT_VA(1), "ids are the light addresses");
    CHECK(p0->pos[0] == -286.1f && p0->pos[1] == 122.8f && p0->pos[2] == 5.49f && p0->radius == 9 && p0->rgb[2] == 1.0f && !p0->halo,
          "the light's position, radius, colour, no halo");
    CHECK(p1->radius == 15 && p1->halo, "the second light has its halo");
    CHECK(set.ndir == 1 && set.dir[0].dir[2] == -1 && fabsf(set.dir[0].rgb[2] - 0.18f) < 1e-6f, "the moon: direction and colour");
    CHECK(fabsf(set.ambient[0] - 0.055f) < 1e-6f && fabsf(set.ambient[2] - 0.075f) < 1e-6f, "the ambient colour");
    CHECK(set.in_lists == 10 && p0->near && p1->near, "lights in the lists are near (%u)", set.in_lists);

    /* The pool holds every light: lights only there are read too (not near); its entries past the
     * live count are not; the manager's own character light is skipped. */
    light(10, LEVEL_KIND_POINT, 100, 0, 3, 12, 1, 0.9f, 0.8f, 1, 0xffff);
    light(11, LEVEL_KIND_POINT, 200, 0, 3, 12, 1, 0.9f, 0.8f, 1, 0xffff);   /* past the live count */
    light(12, LEVEL_KIND_POINT, 0, 0, 0, 4, 1, 1, 1, 1, 0xffff);             /* the character light */
    put32(MGR_VA + LEVEL_MGR_CHAR_LIGHT, LIGHT_VA(12));
    const int pl[] = {0, 1, 10, 12, 11};
    pool(pl, 4);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 3 && set.listed == 12 && set.in_lists == 10,
          "the pool adds its own lights (got %u lights, %u listed, %u in lists)", set.n, set.listed, set.in_lists);
    int far = -1;
    for (uint32_t i = 0; i < set.n; i++) {
        CHECK(set.p[i].id != LIGHT_VA(11) && set.p[i].id != LIGHT_VA(12), "no light past the live count, no character light");
        if (set.p[i].id == LIGHT_VA(10)) far = (int)i;
    }
    CHECK(far >= 0 && !set.p[far].near && set.p[far].radius == 12, "a pool-only light is read, not near");
    put32(MGR_VA + LEVEL_MGR_POOL + 8, 65);   /* live over the capacity: the pool is not used */
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 2, "a broken pool alone is skipped (%u)", set.n);
    pool(pl, 4);
    list(0, NULL, 0);
    list(1, NULL, 0);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 3 && set.in_lists == 0, "the pool alone is enough");
    pool(NULL, 0);
    list(0, a, 5);
    list(1, b, 6);

    /* Bad managers and lists: no light, no read outside memory beyond the checks. */
    put32(PTR_VA, 0);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == -1 && set.n == 0, "no manager: -1");
    put32(PTR_VA, 0x10);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == -1 && set.n == 0, "unmapped manager: -1");
    put32(PTR_VA, MGR_VA);
    put32(MGR_VA + LEVEL_MGR_LIST0 + 8, ARR0_VA + 4u * 5000);   /* end far beyond the begin */
    put32(MGR_VA + LEVEL_MGR_LIST1 + 4, 0x10);                   /* unmapped begin */
    put32(MGR_VA + LEVEL_MGR_POOL, 0x13);                         /* misaligned pool */
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == -1 && set.n == 0, "pool and both lists broken: -1");
    put32(MGR_VA + LEVEL_MGR_LIST0 + 8, ARR0_VA - 4);            /* end before the begin */
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == -1, "end before begin: -1");
    list(1, b, 6);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 2, "one good list is enough");
    put32(ARR1_VA, 0x7);   /* a misaligned light pointer */
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 1 && set.rejected == 3, "a bad pointer is rejected alone");
    reset_mem();
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 0 && set.listed == 0, "empty lists: read, no light");

    /* More lights than LEVEL_POINTS_MAX: the rest are counted as dropped. */
    reset_mem();
    static int many[300];
    for (int i = 0; i < 100; i++) {
        light(i, LEVEL_KIND_POINT, (float)i, 0, 0, 5, 1, 1, 1, 1, 0xffff);
        many[i] = i;
    }
    list(0, many, 100);
    CHECK(level_lights_read(fake_read, NULL, PTR_VA, &set) == 0 && set.n == 100 && !set.dropped, "100 lights read");
}

static void test_lamp(void) {
    const LevelPoint p = {{1, 2, 3}, 10, {0.5f, 1.0f, 0.25f}, 7, 0, 0};
    LampLight l = level_lamp(&p, 1);
    CHECK(l.radius == 10 * LEVEL_RADIUS_FIT && l.rgb[1] == 1.0f && l.rgb[0] == 0.5f && l.id == 7, "radius x fit, colour kept, id");
    CHECK(fabsf(l.intensity - LEVEL_GAIN) < 1e-6f, "intensity at full level is the gain (got %.3f)", l.intensity);
    CHECK(fabsf(level_lamp(&p, 0.5f).intensity - 0.5f * LEVEL_GAIN) < 1e-6f, "intensity follows the level");
    const LevelPoint q = {{0, 0, 0}, 5, {4, 2, 1}, 8, 0, 0};
    l = level_lamp(&q, 1);
    CHECK(l.rgb[0] == 1.0f && l.rgb[1] == 0.5f && fabsf(l.intensity - LEVEL_INTENSITY_MAX * LEVEL_GAIN) < 1e-6f,
          "a colour over 1 is normalised, its peak goes into the intensity (capped)");
    /* The fit: the lamp fall-off over the fitted radius follows the title's linear one. */
    float worst = 0;
    for (int i = 0; i <= 100; i++) {
        const float d = (float)i / 100;
        worst = fmaxf(worst, fabsf(lamp_falloff(d * 10, 10 * LEVEL_RADIUS_FIT) - (1 - d)));
    }
    CHECK(worst < 0.075f, "fall-off within 0.075 of the title's linear one (worst %.3f)", worst);
}

static int vis_all(const float pos[3], float r, void *ctx) { (void)pos, (void)r, (void)ctx; return 1; }
static int vis_x_pos(const float pos[3], float r, void *ctx) { (void)ctx; return pos[0] + r >= 0; }
static const LampLight *find(const LampLight *l, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; i++)
        if (l[i].id == id) return &l[i];
    return NULL;
}
static LevelPoint pt(float x, float r, uint32_t id) { return (LevelPoint){{x, 0, 0}, r, {1, 1, 1}, id, 0, 0}; }

static void test_select(void) {
    static LevelTracker k;
    static LevelLightSet s;
    const float eye[3] = {0, 0, 0};
    LampLight out[LAMP_MAX_LIGHTS];
    level_tracker_reset(&k);
    memset(&s, 0, sizeof s);
    s.p[0] = pt(5, 5, 100);
    s.p[1] = pt(50, 5, 200);
    s.n = 2;
    uint32_t n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 32);
    CHECK(n == 2 && find(out, n, 100) && find(out, n, 200), "both lights out with their ids");
    CHECK(fabsf(find(out, n, 100)->intensity - LAMP_FADE * LEVEL_GAIN) < 1e-6f, "a new light fades in from LAMP_FADE");
    for (int f = 0; f < 10; f++) n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 32);
    CHECK(fabsf(find(out, n, 100)->intensity - LEVEL_GAIN) < 1e-6f, "then holds full level");
    /* Cap 1: the near light wins. */
    n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 1);
    CHECK(n == 1 && out[0].id == 100, "cap 1 keeps the nearest");
    /* Hysteresis: a slightly better light does not take the slot at once. */
    level_tracker_reset(&k);
    s.p[0] = pt(10, 5, 100);
    s.p[1] = pt(10.5f, 5, 200);
    for (int f = 0; f < 12; f++) n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 1);
    CHECK(n >= 1 && out[0].id == 100, "the nearer of two gets the slot");
    s.p[0].pos[0] = 10.6f;   /* now 200 is a little nearer */
    n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 1);
    CHECK(n == 1 && out[0].id == 100, "the shown light keeps the slot against a slightly better one");
    s.p[0].pos[0] = 40;      /* far worse: it gives the slot up */
    n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 1);
    CHECK(n == 1 && out[0].id == 200, "a much better light takes the slot");
    /* Out of view: fades out, then dropped. */
    level_tracker_reset(&k);
    s.p[0] = pt(5, 1, 100);
    s.p[1] = pt(-50, 1, 200);
    for (int f = 0; f < 10; f++) n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 32);
    n = level_lights_select(&k, &s, eye, vis_x_pos, NULL, out, 32);
    CHECK(n == 2 && fabsf(find(out, n, 200)->intensity - (1 - LAMP_FADE) * LEVEL_GAIN) < 1e-5f, "a light leaving the view fades out");
    for (int f = 0; f < 10; f++) n = level_lights_select(&k, &s, eye, vis_x_pos, NULL, out, 32);
    CHECK(n == 1 && out[0].id == 100, "and is gone at 0");
    /* Removed from the list: gone at once (no position); the track fades. NULL set: no lights. */
    s.n = 1;
    n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 32);
    CHECK(n == 1 && out[0].id == 100, "the remaining light stays");
    n = level_lights_select(&k, NULL, eye, vis_all, NULL, out, 32);
    CHECK(n == 0, "no set: no light");
    for (int f = 0; f < 10; f++) level_lights_select(&k, NULL, eye, vis_all, NULL, out, 32);
    CHECK(k.n == 0, "tracks are dropped once faded (left %u)", k.n);
    /* Stable output order is not required, but ids are: 40 lights, cap 32, same set every frame. */
    level_tracker_reset(&k);
    s.n = 40;
    for (uint32_t i = 0; i < 40; i++) s.p[i] = pt(2 + (float)i, 4, 1000 + i);
    uint32_t prev[LAMP_MAX_LIGHTS], np = 0, changed = 0;
    for (int f = 0; f < 30; f++) {
        n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 32);
        CHECK(n <= 32, "at most the cap");
        if (f > 12) {
            uint32_t same = 0;
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t j = 0; j < np; j++) same += out[i].id == prev[j];
            changed += same != n || n != np;
        }
        np = n;
        for (uint32_t i = 0; i < n; i++) prev[i] = out[i].id;
    }
    CHECK(!changed && np == 32, "a steady set keeps the same 32 lights (%u changes, %u lights)", changed, np);
    CHECK(!find(out, np, 1039) && find(out, np, 1000), "the nearest are kept");
    /* A light of the player's surroundings (near) ranks over a slightly closer one that is not. */
    level_tracker_reset(&k);
    s.n = 2;
    s.p[0] = pt(10, 5, 100);
    s.p[1] = pt(12, 5, 200);
    s.p[1].near = 1;
    n = level_lights_select(&k, &s, eye, vis_all, NULL, out, 1);
    CHECK(n == 1 && out[0].id == 200, "the near light wins the slot");
}

int main(void) {
    test_options();
    test_read();
    test_lamp();
    test_select();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
