/* Game-independent test of the Steam Audio integration (built by
 * runtime/Makefile as build/steam_audio_test, run from runtime/).
 *
 *   steam_audio_test [LIBRARY]   (default: the program's folder, then build/steam-audio/<platform>/)
 *
 * 1. Without the library (a path that does not exist) sa_open fails cleanly and nothing is created.
 * 2. With it (built by `make steam-audio`): the context and the default HRTF are created, and one
 *    block of noise from a source on the right is louder in the right ear (and on the left louder
 *    left; ahead balanced). Prints SKIP for this part when the library was not built.
 * 3. sa_listener_dir maps the title's DirectSound axes onto Steam Audio's.
 * 4. src/acoustics.c: the MATERIAL -> acoustic material table, the axes, the direct path -> muffling and
 *    RT60 -> I3DL2 mappings; with the library, a synthetic level (collision records as in a .lev): a
 *    source behind a brick wall is attenuated and duller, one beside it is not, an opened door lets it
 *    through, and a closed concrete room rings longer than a gravel one. */
#define _GNU_SOURCE
#include "../runtime/src/steam_audio.h"
#include "../runtime/src/acoustics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* Energy of each ear for white noise from dir, after a few blocks (the filter's history filled). */
static void ears(SaBinaural *b, const float dir[3], double e[2]) {
    static float in[SA_FRAME], l[SA_FRAME], r[SA_FRAME];
    uint32_t seed = 12345;
    e[0] = e[1] = 0;
    for (int blk = 0; blk < 8; blk++) {
        for (int i = 0; i < SA_FRAME; i++) {
            seed = seed * 1664525u + 1013904223u;
            in[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.5f;
        }
        sa_binaural_apply(b, dir, 1.0f, in, l, r);
        if (blk >= 4)
            for (int i = 0; i < SA_FRAME; i++) e[0] += (double)l[i] * l[i], e[1] += (double)r[i] * r[i];
    }
}


/* ---- a synthetic collision mesh (materials.h layout: 10-byte triangles, 16-byte vertices) ---- */
static uint8_t m_tri[128 * 10], m_vert[512 * 16];
static int m_nt, m_nv;
static void put16(uint8_t *p, unsigned v) { p[0] = v & 255, p[1] = v >> 8 & 255; }
static int vert(float x, float y, float z) {
    float f[4] = {x, y, z, 1};
    memcpy(m_vert + 16 * m_nv, f, 16);
    return m_nv++;
}
static void tri(int a, int b, int c, uint8_t mat) {
    uint8_t *r = m_tri + 10 * m_nt++;
    put16(r, (unsigned)a), put16(r + 2, (unsigned)b), put16(r + 4, (unsigned)c), put16(r + 6, 1);
    r[8] = mat, r[9] = 0;
}
/* a rectangle as two triangles: corners a b c d in order */
static void quad(const float a[3], const float b[3], const float c[3], const float d[3], uint8_t mat) {
    const int i = vert(a[0], a[1], a[2]), j = vert(b[0], b[1], b[2]), k = vert(c[0], c[1], c[2]), l = vert(d[0], d[1], d[2]);
    tri(i, j, k, mat);
    tri(i, k, l, mat);
}
static MatMesh mesh_now(void) { MatMesh m = {m_tri, m_vert, (uint32_t)m_nt, (uint32_t)m_nv}; return m; }
/* a closed box [-s, s] x [-s, s] x [0, h] (world, z up) of one material */
static void box(float s, float h, uint8_t mat) {
    const float p[8][3] = {{-s, -s, 0}, {s, -s, 0}, {s, s, 0}, {-s, s, 0}, {-s, -s, h}, {s, -s, h}, {s, s, h}, {-s, s, h}};
    /* every face towards the inside (counter-clockwise seen from inside) */
    quad(p[0], p[1], p[2], p[3], mat), quad(p[4], p[7], p[6], p[5], mat);
    quad(p[0], p[4], p[5], p[1], mat), quad(p[1], p[5], p[6], p[2], mat);
    quad(p[2], p[6], p[7], p[3], mat), quad(p[3], p[7], p[4], p[0], mat);
}

static void test_acoustics_pure(void) {
    SaMaterial m;
    ac_material(116, &m);
    CHECK(m.absorption[0] == 0.03f && m.transmission[1] == 0.015f, "material: BRICKWALL -> brick (absorption %.2f, transmission %.3f)", m.absorption[0], m.transmission[1]);
    ac_material(5, &m);
    CHECK(m.transmission[1] == 0.002f, "material: CONCRETE -> concrete");
    ac_material(6, &m);
    CHECK(m.transmission[1] == 0.002f, "material: ASHPHALT -> concrete");
    ac_material(36, &m);
    CHECK(m.absorption[2] == 0.80f, "material: DIRT -> gravel (absorbent)");
    ac_material(118, &m);
    CHECK(m.transmission[0] == 0.200f, "material: STEELWALL -> metal");
    ac_material(2, &m);
    CHECK(m.transmission[0] == 0.060f, "material: GLASS -> glass");
    ac_material(16, &m);
    CHECK(m.transmission[0] == 0.070f, "material: WOOD -> wood");
    ac_material(0, &m);
    CHECK(m.transmission[0] == 0.070f, "material: door panels (0) -> wood");
    ac_material(15, &m);
    CHECK(m.transmission[2] == 0.004f, "material: DRYWALL -> plaster");
    ac_material(107, &m);
    CHECK(m.transmission[0] >= 0.8f && m.transmission[2] >= 0.7f, "material: chain-link fence nearly transparent (%.2f)", m.transmission[0]);
    ac_material(200, &m);
    CHECK(m.absorption[0] == 0.10f && m.transmission[0] == 0.100f, "material: an id without a class -> generic");
    CHECK(!ac_tri_kept(MAT_ID_CAMERA_GHOST) && ac_tri_kept(116) && ac_tri_kept(0), "scene: camera ghosts left out, walls and doors kept");
    float o[3];
    ac_world_to_sa((const float[3]){1, 2, 3}, o);
    CHECK(o[0] == 1 && o[1] == 3 && o[2] == -2, "axes: world (x, y, z up) -> Steam Audio (x, z, -y)");
    ac_audio_to_sa((const float[3]){1, 2, 3}, o);
    CHECK(o[0] == 1 && o[1] == 2 && o[2] == -3, "axes: audio (DirectSound) -> Steam Audio (x, y, -z)");

    float g, lp;
    ac_direct_to_muffle(1, (const float[3]){0, 0, 0}, &g, &lp);
    CHECK(g == 1 && lp == 1, "muffle: in the clear: unchanged");
    const float a600 = 1 - expf(-2 * 3.14159265f * 600 / 48000), a1000 = 1 - expf(-2 * 3.14159265f * 1000 / 48000);
    const float a1500 = 1 - expf(-2 * 3.14159265f * 1500 / 48000), a2500 = 1 - expf(-2 * 3.14159265f * 2500 / 48000);
    ac_direct_to_muffle(0, (const float[3]){0.015f, 0.002f, 0.001f}, &g, &lp);
    CHECK(near(g, powf(0.015f, 0.25f), 1e-4f) && lp > a600 && lp < a1000, "muffle: behind concrete x%.2f (%.1f dB), about 800 Hz (a %.3f)", g, 20 * log10f(g), lp);
    ac_direct_to_muffle(0, (const float[3]){0.015f, 0.015f, 0.015f}, &g, &lp);
    CHECK(lp > a1500 && lp < a2500, "muffle: behind brick (flat transmission) still duller: about 1.9 kHz (a %.3f)", lp);
    ac_direct_to_muffle(0, (const float[3]){0, 0, 0}, &g, &lp);
    CHECK(g == AC_GAIN_MIN, "muffle: nothing through: the floor x%.2f", g);
    ac_direct_to_muffle(0, (const float[3]){0.9f, 0.85f, 0.8f}, &g, &lp);
    CHECK(g > 0.97f && lp > 1 - expf(-2 * 3.14159265f * 5000 / 48000), "muffle: behind a chain-link fence nearly unchanged (x%.3f, a %.3f)", g, lp);
    ac_direct_to_muffle(0.5f, (const float[3]){0.015f, 0.002f, 0.001f}, &g, &lp);
    CHECK(g > 0.8f && g < 1, "muffle: half hidden in between (x%.2f)", g);

    float d, r;
    CHECK(!ac_reverb_to_i3dl2((const float[3]){1.0f, 1.0f, 0.5f}, 5000, &d, &r) && near(d, 1, 1e-5f) && near(r, 0.807f, 0.005f),
          "reverb: RT60 1/1/0.5 s -> decay 1 s, HF ratio %.3f at 5 kHz", r);
    CHECK(!ac_reverb_to_i3dl2((const float[3]){40, 40, 40}, 5000, &d, &r) && d == 20, "reverb: clamped to 20 s");
    CHECK(ac_reverb_to_i3dl2((const float[3]){0, 0, 0}, 5000, &d, &r) == -1, "reverb: nothing returned (open space): no result");
}

/* ---- reverb on demand: when the reflections are traced, the per-cell cache ---- */
static AcRevCache rc;
static int plan_at(uint32_t scene, float x, float z, double t, float out[3]) {
    const float p[3] = {x, 1.5f, z};
    return ac_rev_plan(&rc, scene, p, t, out);
}
static void result_at(uint32_t scene, float x, float z, double t, float rt, float out[3]) {
    const float p[3] = {x, 1.5f, z}, r[3] = {rt, rt, rt};
    ac_rev_result(&rc, scene, p, t, r, out);
}
static void test_reverb_on_demand(void) {
    float o[3] = {0};
    const double G = AC_REVERB_GAP_S;
    ac_rev_init(&rc);
    /* a still listener: traced at once, then refined every gap until the cell has its runs, then never again */
    CHECK(plan_at(1, 0.2f, 0.2f, 10, o) == AC_REV_TRACE, "reverb cache: a new spot is traced at once");
    result_at(1, 0.2f, 0.2f, 10, 1.0f, o);
    CHECK(o[0] == 1.0f, "reverb cache: the first run is published (%.2f)", o[0]);
    CHECK(plan_at(1, 0.2f, 0.2f, 10 + G / 2, o) == AC_REV_IDLE, "reverb cache: no second run within %.2f s", G);
    int traced = 1;
    double t = 10;
    for (int k = 0; k < 20; k++) {
        t += G;
        if (plan_at(1, 0.2f, 0.2f, t, o) == AC_REV_TRACE) {
            result_at(1, 0.2f, 0.2f, t, traced % 2 ? 2.0f : 1.0f, o);
            traced++;
        }
    }
    CHECK(traced == AC_REVERB_CELL_RUNS, "reverb cache: a still listener's cell gets %d runs in all (%d), then none", AC_REVERB_CELL_RUNS, traced);
    CHECK(near(o[1], 1.5f, 1e-5f), "reverb cache: the cell's runs averaged (%.3f)", o[1]);
    CHECK(plan_at(1, 0.2f, 0.2f, t + 1000, o) == AC_REV_IDLE, "reverb cache: still there 1000 s later: no run");
    /* small moves inside the converged cell: nothing */
    CHECK(plan_at(1, 0.5f, 0.2f, t + 1001, o) == AC_REV_IDLE && plan_at(1, 0.9f, 0.9f, t + 1002, o) == AC_REV_IDLE,
          "reverb cache: moving inside a known cell: no run, nothing new");
    /* into a new cell: traced (after the gap since the last run) */
    t += 1003;
    const float nx = 0.2f + AC_REVERB_CELL_M * 2;
    CHECK(plan_at(1, nx, 0.2f, t, o) == AC_REV_TRACE, "reverb cache: a new cell %.1f m on is traced", nx - 0.2f);
    result_at(1, nx, 0.2f, t, 3.0f, o);
    CHECK(plan_at(1, nx + 0.1f, 0.2f, t + G / 4, o) == AC_REV_IDLE, "reverb cache: not twice within the gap");
    /* back to the first: its cached result, no run */
    CHECK(plan_at(1, 0.2f, 0.2f, t + G / 2, o) == AC_REV_PUBLISH && near(o[0], 1.5f, 1e-5f), "reverb cache: back in a known spot: its result from the cache (%.2f), no run", o[0]);
    CHECK(plan_at(1, 0.2f, 0.2f, t + 5 * G, o) == AC_REV_IDLE, "reverb cache: and nothing more there");
    /* the edge between two known cells: a step back and forth under AC_REVERB_MOVE_M switches nothing */
    ac_rev_init(&rc);
    t = 0;
    const float e = AC_REVERB_CELL_M;   /* cells [0, e) and [e, 2e) */
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < AC_REVERB_CELL_RUNS; k++, t += G) {
            const float x = c ? e + 0.8f : e - 0.1f;
            if (plan_at(1, x, 0.2f, t, o) == AC_REV_TRACE) result_at(1, x, 0.2f, t, c ? 2.0f : 1.0f, o);
        }
    CHECK(o[0] == 2.0f, "reverb cache: two neighbouring cells known (%.1f)", o[0]);
    CHECK(plan_at(1, e + 0.1f, 0.2f, t, o) == AC_REV_IDLE, "reverb cache: to the edge inside the same cell: nothing");
    int pub = 0;
    for (int k = 0; k < 10; k++) pub += plan_at(1, k % 2 ? e + 0.1f : e - 0.1f, 0.2f, t + k, o) != AC_REV_IDLE;
    CHECK(pub == 0, "reverb cache: 0.2 m back and forth over a cell edge: no switch (%d)", pub);
    CHECK(plan_at(1, e - 0.6f, 0.2f, t + 20, o) == AC_REV_PUBLISH && o[0] == 1.0f, "reverb cache: %.1f m into the other cell: its result (%.1f)", 0.5f, o[0]);
    /* a new scene (a level or a door): traced at once, the old scene's results kept for its return */
    CHECK(plan_at(2, e - 0.6f, 0.2f, t + 20.01, o) == AC_REV_TRACE, "reverb cache: another scene at the same spot: traced at once");
    result_at(2, e - 0.6f, 0.2f, t + 20.01, 0.5f, o);
    CHECK(plan_at(1, e - 0.6f, 0.2f, t + 20.02, o) == AC_REV_PUBLISH && o[0] == 1.0f, "reverb cache: the first scene again (the door shut): its cached result (%.1f)", o[0]);
    /* runs with no tail (an open space: 0 s) do not shorten a cell's result */
    ac_rev_init(&rc);
    CHECK(plan_at(1, 0, 0, 0, o) == AC_REV_TRACE, "reverb cache: a fresh cache traces");
    result_at(1, 0, 0, 0, 1.2f, o);
    plan_at(1, 0, 0, G, o);
    result_at(1, 0, 0, G, 0.0f, o);
    CHECK(o[0] == 1.2f, "reverb cache: a run with no tail leaves the cell's %.1f s", o[0]);
    ac_rev_init(&rc);
    plan_at(1, 0, 0, 0, o);
    result_at(1, 0, 0, 0, 0.0f, o);
    CHECK(o[0] == 0, "reverb cache: only runs with no tail: none (%.1f)", o[0]);
    /* least recently used out: AC_REVERB_CELLS + 1 cells, the first visited is gone, a later one kept */
    ac_rev_init(&rc);
    t = 0;
    for (int i = 0; i <= AC_REVERB_CELLS; i++)
        for (int k = 0; k < AC_REVERB_CELL_RUNS; k++, t += G) {
            const float x = (i + 0.5f) * AC_REVERB_CELL_M;
            if (plan_at(1, x, 0, t, o) == AC_REV_TRACE) result_at(1, x, 0, t, 1.0f, o);
        }
    CHECK(plan_at(1, 0.5f * AC_REVERB_CELL_M, 0, t + 1, o) == AC_REV_TRACE, "reverb cache: %d cells later the first is forgotten (traced again)", AC_REVERB_CELLS);
    result_at(1, 0.5f * AC_REVERB_CELL_M, 0, t + 1, 1.0f, o);
    CHECK(plan_at(1, 5.5f * AC_REVERB_CELL_M, 0, t + 2, o) == AC_REV_PUBLISH, "reverb cache: a recent one is still known");
}

static float query_gain(uint32_t key, const float lis[3], const float src[3], float *lp) {
    float g = -1, l = -1;
    ac_query(key, lis, src, &g, &l, NULL);   /* queues the position */
    ac_step();
    if (ac_query(key, lis, src, &g, &l, NULL)) return -1;
    if (lp) *lp = l;
    return g;
}

static void test_acoustics_scene(void) {
    /* a floor and a brick wall (y = 5, x -3..3, z 0..4) between the listener (world 0,0,1.8) and a
     * source at (0, 10, 1.8); audio axes are (x, z, y) of the world */
    m_nt = m_nv = 0;
    quad((const float[3]){-50, -50, 0}, (const float[3]){50, -50, 0}, (const float[3]){50, 50, 0}, (const float[3]){-50, 50, 0}, 6);
    const int wall0 = m_nt;
    quad((const float[3]){-3, 5, 0}, (const float[3]){3, 5, 0}, (const float[3]){3, 5, 4}, (const float[3]){-3, 5, 4}, 116);
    MatMesh m = mesh_now();
    CHECK(!ac_start(0, 1, 0), "acoustics started (no thread: the test steps it)");
    const float lis[3] = {0, 1.8f, 0}, behind[3] = {0, 1.8f, 10}, beside[3] = {10, 1.8f, 10};
    float g, lp;
    CHECK(ac_query(1, lis, behind, &g, &lp, NULL) == -1, "no scene yet: no result (the wall map stays)");
    ac_set_mesh(&m, 1, "synthetic");
    ac_step();
    g = query_gain(1, lis, behind, &lp);
    CHECK(g > 0 && g < 0.5f && lp < 0.3f, "a source behind a brick wall: attenuated x%.2f (%.1f dB) and duller (a %.3f)", g, 20 * log10f(g), lp);
    g = query_gain(2, lis, beside, &lp);
    CHECK(g == 1 && lp == 1, "a source beside the wall: unchanged (x%.2f)", g);
    const float under[3] = {0, 0.2f, 3};
    g = query_gain(3, lis, under, &lp);
    CHECK(g == 1, "a footstep on the floor in front: not occluded by the floor (x%.2f)", g);
    const uint16_t door[2] = {(uint16_t)wall0, (uint16_t)(wall0 + 1)};
    ac_doors(1, door, 2);
    g = query_gain(1, lis, behind, &lp);
    CHECK(g == 1, "the wall's triangles opened (a door): the source in the clear again (x%.2f)", g);
    ac_doors(1, door, 0);
    g = query_gain(1, lis, behind, &lp);
    CHECK(g < 0.5f, "closed again: attenuated (x%.2f)", g);

    /* the reverb of a closed room: concrete rings longer than gravel */
    float rt_c[3], rt_g[3];
    m_nt = m_nv = 0;
    box(4, 3, 5);
    m = mesh_now();
    ac_set_mesh(&m, 2, "concrete room");
    ac_listener((const float[3]){0, 1.5f, 0}, (const float[3]){0, 0, 1}, (const float[3]){0, 1, 0});
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ac_step();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ac_step();
    ac_step();
    const uint32_t q1 = ac_reverb(rt_c);
    /* on demand: a still listener's cell is done after its runs; another spot is traced, the first comes back from the cache */
    {
        char st[512];
        unsigned long long runs = 9, cached = 9;
        ac_stats(st, sizeof st);
        for (int k = 0; k < 10; k++) ac_step();
        ac_stats(st, sizeof st);
        sscanf(strstr(st, "reverb ") ? strstr(st, "reverb ") : "", "reverb %llu runs, %*f ms each, %llu from the cache", &runs, &cached);
        CHECK(runs == AC_REVERB_CELL_RUNS - 3 && cached == 0, "reverb on demand: a still listener: %d runs in all, then none (%llu more in 10 steps)", AC_REVERB_CELL_RUNS, runs);
        ac_listener((const float[3]){3, 1.5f, 0}, (const float[3]){0, 0, 1}, (const float[3]){0, 1, 0});
        ac_step();
        ac_listener((const float[3]){0, 1.5f, 0}, (const float[3]){0, 0, 1}, (const float[3]){0, 1, 0});
        ac_step();
        const uint32_t q_back = ac_reverb(rt_c);
        ac_stats(st, sizeof st);
        sscanf(strstr(st, "reverb ") ? strstr(st, "reverb ") : "", "reverb %llu runs, %*f ms each, %llu from the cache", &runs, &cached);
        CHECK(runs == 1 && cached == 1 && q_back == q1 + 3, "reverb on demand: 3 m away traced (%llu run), back again from the cache (%llu)", runs, cached);
    }
    m_nt = m_nv = 0;
    box(4, 3, 36);
    m = mesh_now();
    ac_set_mesh(&m, 3, "gravel room");
    ac_step();
    ac_step();
    ac_step();
    const uint32_t q2 = ac_reverb(rt_g);
    CHECK(q1 && q2 > q1, "reverb: results for both rooms");
    CHECK(rt_c[1] > 0.3f && rt_c[1] > 2 * rt_g[1], "reverb: concrete room RT60 %.2f/%.2f/%.2f s, gravel %.2f/%.2f/%.2f s", rt_c[0], rt_c[1], rt_c[2], rt_g[0],
          rt_g[1], rt_g[2]);
    float d, r;
    CHECK(!ac_reverb_to_i3dl2(rt_c, 5000, &d, &r) && d > 0.3f, "reverb: concrete room -> I3DL2 decay %.2f s, HF ratio %.2f", d, r);
    printf("cost: scene build + one direct and one reverb run (%d rays x %d bounces): %.1f ms\n", AC_REVERB_RAYS, AC_REVERB_BOUNCES,
           ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6);
    char buf[512];
    ac_stats(buf, sizeof buf);
    printf("%s\n", buf);
    ac_stop();
}

/* ---- sound around corners ----
 * An L-shaped corridor of concrete (world, z up; 4 m wide, 3 m high): leg A along +x (x -2..20, y -2..2),
 * leg B along +y from its far end (x 16..20, y 2..20), a door panel (MATERIAL 0) across leg B at y = 8.
 * The listener stands in leg A at (2, 0), the source in leg B at (18, 15): the inner corner's walls hide
 * it, and the way round is the corner at (18, 0), straight down leg A (+x). */
static int door0;
static void corridor(void) {
    m_nt = m_nv = 0;
    const uint8_t C = 5;   /* concrete */
    /* floor (facing up) and ceiling (facing down) */
    quad((const float[3]){-2, -2, 0}, (const float[3]){20, -2, 0}, (const float[3]){20, 2, 0}, (const float[3]){-2, 2, 0}, C);
    quad((const float[3]){16, 2, 0}, (const float[3]){20, 2, 0}, (const float[3]){20, 20, 0}, (const float[3]){16, 20, 0}, C);
    quad((const float[3]){-2, -2, 3}, (const float[3]){-2, 2, 3}, (const float[3]){20, 2, 3}, (const float[3]){20, -2, 3}, C);
    quad((const float[3]){16, 2, 3}, (const float[3]){16, 20, 3}, (const float[3]){20, 20, 3}, (const float[3]){20, 2, 3}, C);
    /* walls: (x0, y0) - (x1, y1) from the floor to the ceiling */
    const float w[6][4] = {{-2, -2, 20, -2}, {-2, 2, 16, 2}, {-2, -2, -2, 2}, {20, -2, 20, 20}, {16, 2, 16, 20}, {16, 20, 20, 20}};
    for (int i = 0; i < 6; i++)
        quad((const float[3]){w[i][0], w[i][1], 0}, (const float[3]){w[i][2], w[i][3], 0}, (const float[3]){w[i][2], w[i][3], 3},
             (const float[3]){w[i][0], w[i][1], 3}, C);
    door0 = m_nt;
    quad((const float[3]){16, 8, 0}, (const float[3]){20, 8, 0}, (const float[3]){20, 8, 3}, (const float[3]){16, 8, 3}, 0);
}

/* ac_query after a step: gain, and the angle (degrees) between where it seems to come from and +x */
static float corner_query(uint32_t key, const float lis[3], const float src[3], float *deg) {
    float g = -1, l, pos[3];
    ac_query(key, lis, src, &g, &l, pos);
    ac_step();
    if (ac_query(key, lis, src, &g, &l, pos)) return -1;
    const float v[3] = {pos[0] - lis[0], pos[1] - lis[1], pos[2] - lis[2]};
    *deg = acosf(v[0] / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])) * 180 / 3.14159265f;
    return g;
}

static void test_pathing(const char *cache) {
    /* the pure mix: no path -> exactly the direct result */
    const float tc[3] = {0.015f, 0.002f, 0.001f}, ds[3] = {0, 0, -1};
    float g0, lp0, g1, lp1, dir[3];
    ac_direct_to_muffle(0, tc, &g0, &lp0);
    ac_mix_paths(0, tc, NULL, 10, ds, &g1, &lp1, dir);
    CHECK(g0 == g1 && lp0 == lp1 && dir[2] == -1 && dir[0] == 0, "pathing mix: no path -> the direct result (x%.3f)", g1);
    const SaPath side = {0.08f, {0.9f, 0.6f, 0.4f}, {1, 0, 0}}, same = {0.1f, {1, 1, 1}, {0, 0, -1}};
    ac_mix_paths(0, tc, &side, 10, ds, &g1, &lp1, dir);
    CHECK(g1 > g0 && dir[0] > 0.9f, "pathing mix: a path from the right -> louder (x%.3f vs x%.3f), from the right (%.2f %.2f %.2f)", g1, g0, dir[0], dir[1], dir[2]);
    ac_mix_paths(0, tc, &same, 10, ds, &g1, &lp1, dir);
    CHECK(g1 == g0 && lp1 == lp0, "pathing mix: a 'path' along the direct line is the direct path itself: unchanged");
    ac_mix_paths(1, tc, &side, 10, ds, &g1, &lp1, dir);
    CHECK(g1 == 1 && dir[2] == -1, "pathing mix: a source in view ignores paths");

    corridor();
    MatMesh m = mesh_now();
    const float lis[3] = {2, 1.5f, 0}, src[3] = {18, 1.5f, 15};   /* audio axes: (x, z, y) of the world */
    const uint16_t open[2] = {(uint16_t)door0, (uint16_t)(door0 + 1)};
    float deg, g_direct, g_path;
    /* occlusion only */
    CHECK(!ac_start(0, 0, 0), "acoustics without pathing");
    ac_set_mesh(&m, 7, "corridor");
    ac_doors(7, open, 2);
    ac_step();
    g_direct = corner_query(1, lis, src, &deg);
    CHECK(g_direct > 0 && deg > 35, "L corridor, occlusion only: x%.3f (%.1f dB) from the source's own direction (%.0f deg from the corridor)", g_direct,
          20 * log10f(g_direct), deg);
    CHECK(ac_pathing_probes(NULL) == 0, "no pathing: no probes");
    ac_stop();
    /* with pathing */
    ac_set_cache_dir(cache);
    CHECK(!ac_start(0, 0, 1), "acoustics with pathing");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ac_set_mesh(&m, 8, "corridor");
    ac_doors(8, open, 2);
    ac_step();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int cached = -1;
    const int np = ac_pathing_probes(&cached);
    CHECK(np > 4, "pathing: %d probes (%s) in %.0f ms", np, cached ? "from the cache" : "baked", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6);
    g_path = corner_query(1, lis, src, &deg);
    CHECK(g_path > 1.5f * g_direct, "door open: around the corner louder than occlusion only (x%.3f, %.1f dB, vs x%.3f)", g_path, 20 * log10f(g_path), g_direct);
    CHECK(deg < 15, "door open: it comes from the opening down the corridor (%.0f deg from +x; the source itself is at 43 deg)", deg);
    ac_doors(8, open, 0);
    float deg2, g_closed = corner_query(1, lis, src, &deg2);
    CHECK(g_closed > 0 && g_closed < 0.7f * g_path && deg2 > 35, "door closed: the way round is blocked: x%.3f from the source's own direction (%.0f deg)", g_closed, deg2);
    ac_doors(8, open, 2);
    g_path = corner_query(1, lis, src, &deg);
    CHECK(g_path > 1.5f * g_closed && deg < 15, "door open again: x%.3f from the opening (%.0f deg)", g_path, deg);
    const float near_src[3] = {10, 1.5f, 0};   /* in view down leg A */
    float g_view = corner_query(2, lis, near_src, &deg);
    CHECK(g_view == 1 && deg < 1, "a source in view: unchanged (x%.3f)", g_view);
    char buf[512];
    ac_stats(buf, sizeof buf);
    printf("%s\n", buf);
    ac_stop();
    if (cache) {   /* the second load reads the cache */
        CHECK(!ac_start(0, 0, 1), "acoustics with pathing again");
        ac_set_mesh(&m, 9, "corridor");
        ac_doors(9, open, 2);
        ac_step();
        CHECK(ac_pathing_probes(&cached) == np && cached == 1, "pathing: the same %d probes read back from the cache", np);
        float g_again = corner_query(1, lis, src, &deg);
        CHECK(fabsf(g_again - g_path) < 1e-4f, "from the cache: the same result (x%.3f)", g_again);
        ac_stop();
    }
    ac_set_cache_dir(NULL);
}

/* The reverb cells' cache file: a room's finished cell is read back at the next start (no run there). */
static unsigned long long stat_after(const char *st, const char *what) {
    const char *p = strstr(st, "reverb ");
    unsigned long long runs = 99, cached = 99;
    if (p) sscanf(p, "reverb %llu runs, %*f ms each, %llu from the cache", &runs, &cached);
    return !strcmp(what, "runs") ? runs : cached;
}
static void test_reverb_cache_file(const char *cache) {
    char tmpl[] = "/tmp/steam_audio_test.XXXXXX", st[512];
    const char *dir = cache ? cache : mkdtemp(tmpl);
    if (!dir) { printf("SKIP reverb cache file: no folder\n"); return; }
    ac_set_cache_dir(dir);
    m_nt = m_nv = 0;
    box(4, 3, 5);
    MatMesh m = mesh_now();
    const float at[3] = {0, 1.5f, 0}, ahead[3] = {0, 0, 1}, top[3] = {0, 1, 0};
    float rt[3];
    CHECK(!ac_start(0, 1, 0), "reverb cache file: acoustics started");
    ac_set_mesh(&m, 1, "reverb room");
    ac_listener(at, ahead, top);
    for (int k = 0; k < AC_REVERB_CELL_RUNS + 2; k++) ac_step();
    ac_reverb(rt);
    ac_stats(st, sizeof st);
    /* written while the game runs (the runtime never calls ac_stop at exit): AC_REVERB_SAVE_S after the cell finished, no trace in between */
    {
        long size = 0;
        for (int k = 0; k < (int)(AC_REVERB_SAVE_S / AC_REVERB_GAP_S) + 2; k++) ac_step();
        DIR *dd = opendir(dir);
        struct dirent *de;
        char fp[600];
        while (dd && (de = readdir(dd)))
            if (!strncmp(de->d_name, "reverb-", 7)) {
                snprintf(fp, sizeof fp, "%s/%s", dir, de->d_name);
                FILE *ff = fopen(fp, "rb");
                if (ff) fseek(ff, 0, SEEK_END), size = ftell(ff), fclose(ff);
            }
        if (dd) closedir(dd);
        CHECK(size == 12 + (long)sizeof(AcRevCell), "reverb cache file: written %.0f s after the cell finished, without ac_stop (%ld bytes)", AC_REVERB_SAVE_S, size);
    }
    ac_stop();
    CHECK(!ac_start(0, 1, 0), "reverb cache file: started again");
    ac_set_mesh(&m, 2, "reverb room");
    ac_listener(at, ahead, top);
    ac_step();
    float rt2[3];
    const uint32_t q = ac_reverb(rt2);
    ac_stats(st, sizeof st);
    CHECK(stat_after(st, "runs") == 0 && stat_after(st, "cached") == 1 && q && rt2[1] == rt[1] && rt[1] > 0.3f,
          "reverb cache file: the room's spot read back: no run, the same %.2f s", rt2[1]);
    ac_stop();
    ac_set_cache_dir(NULL);
    if (!cache) {   /* our own folder: its files and itself */
        DIR *d = opendir(dir);
        struct dirent *e;
        char f[600];
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') snprintf(f, sizeof f, "%s/%s", dir, e->d_name), remove(f);
        if (d) closedir(d);
        rmdir(dir);
    }
}

int main(int argc, char **argv) {
    /* 3. axes: listener at the origin facing +z (DirectSound), up +y: right = top x front = +x */
    const float pos[3] = {0, 0, 0}, front[3] = {0, 0, 1}, top[3] = {0, 1, 0};
    float d[3];
    sa_listener_dir(pos, front, top, (const float[3]){5, 0, 0}, d);
    CHECK(near(d[0], 1, 1e-5f) && near(d[1], 0, 1e-5f) && near(d[2], 0, 1e-5f), "source on the right -> Steam Audio +x (%.2f %.2f %.2f)", d[0], d[1], d[2]);
    sa_listener_dir(pos, front, top, (const float[3]){0, 0, 3}, d);
    CHECK(near(d[2], -1, 1e-5f), "source ahead -> Steam Audio -z (%.2f)", d[2]);
    sa_listener_dir(pos, front, top, (const float[3]){0, 2, 0}, d);
    CHECK(near(d[1], 1, 1e-5f), "source above -> +y (%.2f)", d[1]);
    sa_listener_dir(pos, front, top, pos, d);
    CHECK(d[0] == 0 && d[1] == 0 && d[2] == -1, "coincident source -> ahead");

    test_acoustics_pure();
    test_reverb_on_demand();
    /* 1. a missing library */
    CHECK(sa_open("/nonexistent/libphonon.so") == -1 && !sa_ok() && !sa_binaural_new(), "missing library: sa_open fails, nothing created");
    CHECK(ac_start(1, 1, 1) == -1 && !ac_running(), "missing library: no acoustics (and no pathing)");
    sa_close();

    /* 2. the real library */
    struct timespec t0, t1;
    if (sa_open(argc > 1 ? argv[1] : NULL)) {
        printf("SKIP Steam Audio library not built (make steam-audio)\n");
    } else {
        CHECK(sa_ok() && !strcmp(sa_version(), "4.8.1"), "library loaded, API %s", sa_version());
        SaBinaural *b = sa_binaural_new();
        CHECK(b != NULL, "binaural effect created");
        if (b) {
            double e[2];
            ears(b, (const float[3]){1, 0, 0}, e);
            CHECK(e[1] > 4 * e[0], "source on the right: right ear louder (%.1f dB)", 10 * log10(e[1] / e[0]));
            sa_binaural_reset(b);
            ears(b, (const float[3]){-1, 0, 0}, e);
            CHECK(e[0] > 4 * e[1], "source on the left: left ear louder (%.1f dB)", 10 * log10(e[0] / e[1]));
            sa_binaural_reset(b);
            ears(b, (const float[3]){0, 0, -1}, e);
            CHECK(fabs(10 * log10(e[0] / e[1])) < 3, "source ahead: ears within 3 dB (a measured head is not symmetric) (%.2f dB), energy %.3g per ear", 10 * log10(e[0] / e[1]), e[0]);
            /* cost: bilinear HRTF, one voice, one 5 ms block */
            static float in[SA_FRAME], l[SA_FRAME], r[SA_FRAME];
            for (int i = 0; i < SA_FRAME; i++) in[i] = sinf(i * 0.1f) * 0.3f;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            const int n = 2000;
            for (int k = 0; k < n; k++) {
                const float a = k * 0.01f, dir[3] = {sinf(a), 0, -cosf(a)};
                sa_binaural_apply(b, dir, 1.0f, in, l, r);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            const double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / n;
            printf("cost: %.1f us per voice per 5 ms block (%.2f%% of a core), moving source, bilinear\n", us, us / 50);
            sa_binaural_free(b);
        }
        test_acoustics_scene();
        test_pathing(argc > 2 ? argv[2] : NULL);
        test_reverb_cache_file(argc > 2 ? argv[2] : NULL);
        sa_close();
    }
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
