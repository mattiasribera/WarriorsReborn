/* Game-independent test of the vertex-program worker pool (src/hle_d3d.c, --vp-threads). The same
 * sequence of program draws (sizes from one vertex to several thousand, inputs and constants
 * overwritten after every draw, as the title reuses its buffers) runs once per pool size in a
 * child process; every draw's RVertex output must be byte-identical to the synchronous run
 * (--vp-threads 0). Also: draws of one batch run on the recording thread (nothing is queued), and
 * the default pool size is physical cores - 2 (at least 1, at most 7). */
#define _GNU_SOURCE
#include "../src/hle_d3d.h"
#include "../src/platform.h"
#include "../src/vsh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- program encoder (the xemu field table, as tools/vsh_test.c) ---- */
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_MOV = 1, MAC_MUL = 2, MAC_DP4 = 7 };
enum { ILU_RCP = 2 };
#define XYZW 0x1B
typedef struct { int mux, reg; } Src;
typedef struct { int mac, ilu; Src a, b, c; int o_mask, o_addr, o_ilu, final; } Ins;
static void put(uint32_t *t, int dw, int pos, int bits, uint32_t v) { t[dw] |= (v & ((1u << bits) - 1)) << pos; }
static void encode(const Ins *i, uint32_t t[4]) {
    memset(t, 0, 16);
    int v = i->a.mux == MUX_V ? i->a.reg : i->b.mux == MUX_V ? i->b.reg : i->c.mux == MUX_V ? i->c.reg : 0;
    int k = i->a.mux == MUX_C ? i->a.reg : i->b.mux == MUX_C ? i->b.reg : i->c.mux == MUX_C ? i->c.reg : 0;
    put(t, 1, 25, 3, i->ilu);
    put(t, 1, 21, 4, i->mac);
    put(t, 1, 13, 8, k + 96);
    put(t, 1, 9, 4, v);
    put(t, 1, 0, 8, XYZW);
    put(t, 2, 26, 2, i->a.mux);
    put(t, 2, 17, 8, XYZW);
    put(t, 2, 11, 2, i->b.mux);
    put(t, 2, 2, 8, XYZW);
    put(t, 3, 28, 2, i->c.mux);
    put(t, 3, 12, 4, i->o_mask);
    put(t, 3, 11, 1, 1);
    put(t, 3, 3, 8, i->o_addr);
    put(t, 3, 2, 1, i->o_ilu);
    put(t, 3, 0, 1, i->final);
}
/* oPos = v0 . c0..c3, oD0 = v0 * c4, oD1 = rcp(v0.w) (ILU), oT0 = v0: transform, a multiply and a
 * reciprocal, so the outputs depend on the inputs and the constants of their own draw. */
static int build_program(VshProgram *p) {
    static uint32_t w[1 + 4 * 8];
    const Ins ins[] = {
        {.mac = MAC_DP4, .a = {MUX_V, 0}, .b = {MUX_C, 0}, .o_mask = 8, .o_addr = VSH_OPOS},
        {.mac = MAC_DP4, .a = {MUX_V, 0}, .b = {MUX_C, 1}, .o_mask = 4, .o_addr = VSH_OPOS},
        {.mac = MAC_DP4, .a = {MUX_V, 0}, .b = {MUX_C, 2}, .o_mask = 2, .o_addr = VSH_OPOS},
        {.mac = MAC_DP4, .a = {MUX_V, 0}, .b = {MUX_C, 3}, .o_mask = 1, .o_addr = VSH_OPOS},
        {.mac = MAC_MUL, .a = {MUX_V, 0}, .b = {MUX_C, 4}, .o_mask = 15, .o_addr = VSH_OD0},
        {.ilu = ILU_RCP, .c = {MUX_V, 0}, .o_mask = 15, .o_addr = VSH_OD1, .o_ilu = 1},
        {.mac = MAC_MOV, .a = {MUX_V, 0}, .o_mask = 15, .o_addr = VSH_OT0, .final = 1},
    };
    int n = sizeof ins / sizeof *ins;
    memset(w, 0, sizeof w);
    for (int i = 0; i < n; i++) encode(&ins[i], &w[1 + 4 * i]);
    w[0] = 0x2078u | (uint32_t)n << 16;
    return vsh_load(w, 1 + 4 * n, p);
}

/* ---- the draw sequence ---- */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static float rndf(void) { return (float)(rnd() >> 8) / (1 << 24) * 4 - 2; }   /* [-2, 2) */
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    for (const uint8_t *b = p; n--; b++) h = (h ^ *b) * 0x100000001b3ull;
    return h;
}

/* Frames of draws: 0 = mixed sizes (1-400 vertices plus a few up to 6000), 1 = 9000 two-batch
 * draws (more than the pool's 8192 queued jobs: it drains mid-frame), 2 = single-batch draws only. */
enum { MAX_DRAWS = 9000, MAX_VERTS = 6000 };
static const int frame_draws[3] = {3000, 9000, 2000};
static uint32_t draw_nv(int frame, int i) {
    if (frame == 1) return 33 + rnd() % 32;
    if (frame == 2) return 1 + rnd() % VSH_BATCH;
    return i % 97 == 0 ? 2000 + rnd() % (MAX_VERTS - 2000) : 1 + rnd() % 400;
}

/* Runs every frame with the pool at threads (hle_d3d_vp_threads); returns the per-draw output hashes
 * in h[] (frame after frame) and the queued / recording-thread draw counts of frame 2. */
static void run_sequence(int threads, uint64_t *h, uint32_t *queued2, uint32_t *inline2) {
    static VshProgram prog;
    static float stream[MAX_VERTS][4], c[VSH_NCONST][4];
    static RVertex *out[MAX_DRAWS];
    static uint32_t nv[MAX_DRAWS];
    hle_d3d_vp_threads = threads;
    hle_d3d_stats = 1;
    if (build_program(&prog)) { printf("FAIL program does not load\n"); exit(1); }
    rng = 12345;
    int k = 0;
    for (int f = 0; f < 3; f++) {
        uint32_t q, in;
        hle_d3d_vp_test_counts(&q, &in);   /* clears them */
        for (int i = 0; i < frame_draws[f]; i++) {
            nv[i] = draw_nv(f, i);
            for (uint32_t v = 0; v < nv[i]; v++)
                for (int q4 = 0; q4 < 4; q4++) stream[v][q4] = q4 == 3 ? 0.5f + (rnd() & 255) / 64.0f : rndf();
            for (int r = 0; r < 5; r++)
                for (int q4 = 0; q4 < 4; q4++) c[r][q4] = rndf();
            out[i] = malloc(nv[i] * sizeof(RVertex));
            memset(out[i], 0xAB, nv[i] * sizeof(RVertex));
            hle_d3d_vp_test_draw(&prog, (const uint8_t *)stream, 16, nv[i], (const float (*)[4])c, out[i]);
        }
        hle_d3d_vp_test_wait();   /* as at Swap */
        for (int i = 0; i < frame_draws[f]; i++) {
            h[k++] = fnv(out[i], nv[i] * sizeof(RVertex), 0xcbf29ce484222325ull);
            free(out[i]);
        }
        hle_d3d_vp_test_counts(&q, &in);
        if (f == 2) { *queued2 = q; *inline2 = in; }
    }
}

/* A declaration whose position starts 4 bytes into a 16-byte vertex: each vertex's w is the next
 * vertex's first float, and the last vertex's w is the float after the draw's vertices in the
 * buffer (the Xbox reads the stream bytes there). Draws of several batches with the pool on must
 * read that float from the buffer too, not whatever follows the copied vertices. Returns the draws
 * whose output differs from the synchronous run (threads 0) in a child with the pool at threads. */
enum { OVER_DRAWS = 200, OVER_MAX = 700 };
static uint64_t overhang_hash[OVER_DRAWS];
static void overhang_sequence(int threads) {
    static VshProgram prog;
    static float stream[OVER_MAX + 1][4], c[VSH_NCONST][4];
    static RVertex out[OVER_DRAWS][OVER_MAX];
    hle_d3d_vp_threads = threads;
    if (build_program(&prog)) { printf("FAIL program does not load\n"); exit(1); }
    rng = 777;
    uint32_t nv[OVER_DRAWS];
    for (int i = 0; i < OVER_DRAWS; i++) {
        nv[i] = 2 * VSH_BATCH + rnd() % (OVER_MAX - 2 * VSH_BATCH);
        for (uint32_t v = 0; v <= nv[i]; v++)
            for (int q4 = 0; q4 < 4; q4++) stream[v][q4] = q4 == 0 ? 0.5f + (rnd() & 255) / 64.0f : rndf();
        for (int r = 0; r < 5; r++)
            for (int q4 = 0; q4 < 4; q4++) c[r][q4] = rndf();
        hle_d3d_vp_test_draw_at(&prog, (const uint8_t *)stream, 16, 4, nv[i], (const float (*)[4])c, out[i]);
        if (i % 7 == 6) hle_d3d_vp_test_wait();   /* the title overwrites its buffers: inputs are copied */
    }
    hle_d3d_vp_test_wait();
    for (int i = 0; i < OVER_DRAWS; i++) overhang_hash[i] = fnv(out[i], nv[i] * sizeof(RVertex), 0xcbf29ce484222325ull);
}
static int overhang_child(int threads, uint64_t *h) {
    int fd[2];
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        overhang_sequence(threads);
        _exit(write(fd[1], overhang_hash, sizeof overhang_hash) == (ssize_t)sizeof overhang_hash ? 0 : 2);
    }
    close(fd[1]);
    size_t got = 0;
    while (got < sizeof overhang_hash) {
        ssize_t r = read(fd[0], (uint8_t *)h + got, sizeof overhang_hash - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return got == sizeof overhang_hash && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

enum { NHASH = 3000 + 9000 + 2000 };
/* run_sequence in a child (the pool is sized once per process); 0 when it ran. */
static int run_child(int threads, uint64_t *h, uint32_t counts[2]) {
    int fd[2];
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        static uint64_t ch[NHASH];
        uint32_t c2[2];
        close(fd[0]);
        run_sequence(threads, ch, &c2[0], &c2[1]);
        if (write(fd[1], ch, sizeof ch) != (ssize_t)sizeof ch || write(fd[1], c2, sizeof c2) != (ssize_t)sizeof c2) _exit(2);
        _exit(0);
    }
    close(fd[1]);
    size_t got = 0, want = NHASH * sizeof *h;
    while (got < want) {
        ssize_t r = read(fd[0], (uint8_t *)h + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    ssize_t rc = got == want ? read(fd[0], counts, 2 * sizeof *counts) : 0;
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return got == want && rc == (ssize_t)(2 * sizeof *counts) && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(void) {
    static uint64_t ref[NHASH], h[NHASH];
    uint32_t counts[2];
    CHECK(run_child(0, ref, counts) == 0, "synchronous run (--vp-threads 0) completes");
    CHECK(counts[0] == 0 && counts[1] == (uint32_t)frame_draws[2], "pool off: every draw on the recording thread (%u queued, %u)",
          counts[0], counts[1]);
    int distinct = 0;
    for (int i = 1; i < 200; i++) distinct += ref[i] != ref[i - 1];
    CHECK(distinct == 199, "each draw's output differs from the previous one (the program reads its own inputs and constants)");
    const int sizes[] = {1, 2, 3, 7, -1};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        memset(h, 0, sizeof h);
        int ok = run_child(sizes[s], h, counts) == 0;
        int first_diff = -1;
        for (int i = 0; i < NHASH && first_diff < 0; i++)
            if (h[i] != ref[i]) first_diff = i;
        CHECK(ok && first_diff < 0, "--vp-threads %d: all %d draws byte-identical to --vp-threads 0 (first difference: draw %d)", sizes[s],
              NHASH, first_diff);
        CHECK(ok && counts[0] == 0 && counts[1] == (uint32_t)frame_draws[2],
              "--vp-threads %d: single-batch draws run on the recording thread (%u queued, %u inline of %d)", sizes[s], counts[0], counts[1],
              frame_draws[2]);
    }
    {
        static uint64_t oref[OVER_DRAWS], oh[OVER_DRAWS];
        const int ok = overhang_child(0, oref) == 0 && overhang_child(3, oh) == 0;
        int differ = 0;
        for (int i = 0; i < OVER_DRAWS; i++) differ += oh[i] != oref[i];
        CHECK(ok && !differ, "--vp-threads 3, position reaching past the 16-byte stride: %d of %d draws differ from --vp-threads 0", differ, OVER_DRAWS);
    }
    static const int phys[][2] = {{1, 1}, {2, 1}, {3, 1}, {4, 2}, {6, 4}, {8, 6}, {9, 7}, {16, 7}, {0, 1}};
    for (unsigned i = 0; i < sizeof phys / sizeof *phys; i++)
        CHECK(hle_d3d_vp_default_workers(phys[i][0]) == phys[i][1], "default pool size for %d physical cores: %d (want %d)", phys[i][0],
              hle_d3d_vp_default_workers(phys[i][0]), phys[i][1]);
    int pc = plat_physical_cores();
    CHECK(pc >= 1 && pc <= sysconf(_SC_NPROCESSORS_ONLN), "physical cores detected: %d (of %ld logical)", pc, sysconf(_SC_NPROCESSORS_ONLN));
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
