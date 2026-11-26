/* Game-independent test of the --gpu-skinning option, the program matcher, the instruction and
 * constant packing, the world-program eligibility, the per-program check accounting and the
 * weld/adjacency tables (src/gpu_skin.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gpu_skin.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    GpuSkinConfig c = GPU_SKIN_DEFAULTS;
    CHECK(c.on, "on by default");
    CHECK(gpu_skin_arg(&c, "--char-lighting") == 0 && c.on, "other options are not consumed");
    CHECK(gpu_skin_arg(&c, "--no-gpu-skinning") == 1 && !c.on, "--no-gpu-skinning turns it off");
    CHECK(gpu_skin_arg(&c, "--gpu-skinning") == 1 && c.on, "--gpu-skinning turns it on");
    CHECK(c.world, "world vertex programs on the GPU by default");
    CHECK(gpu_skin_arg(&c, "--no-gpu-vertex-programs") == 1 && !c.world && c.on, "--no-gpu-vertex-programs turns only them off");
    CHECK(gpu_skin_arg(&c, "--gpu-vertex-programs") == 1 && c.world, "--gpu-vertex-programs turns them on");
}

static void test_matcher(void) {
    const uint32_t one = 0x61;   /* the byte "a", then three zero bytes */
    uint64_t h = gsk_program_hash(&one, 1);
    /* FNV-1a 64 of 61 00 00 00 */
    uint64_t want = 14695981039346656037ull;
    const uint8_t bytes[4] = {0x61, 0, 0, 0};
    for (int i = 0; i < 4; i++) want = (want ^ bytes[i]) * 1099511628211ull;
    CHECK(h == want, "hash is FNV-1a 64 over the bytes (%016llx, want %016llx)", (unsigned long long)h, (unsigned long long)want);
    CHECK(gsk_program_allowed(0xb031486a132e9bc0ull), "the 69-instruction character program is allowed");
    CHECK(gsk_program_allowed(0x94ac45eefa1d8809ull), "the 23-instruction character program is allowed");
    CHECK(!gsk_program_allowed(0xcac4d87017910be5ull), "a world program is not");
    CHECK(!gsk_program_allowed(0), "zero is not");
    VshProgram p;
    memset(&p, 0, sizeof p);
    p.n = 3;
    p.ins[0].mac = 13;   /* ARL */
    p.ins[1].ilu = 7;    /* LIT */
    p.ins[2].mac = 4;
    CHECK(gsk_program_supported(&p), "every MAC up to ARL and ILU up to LIT is supported");
    p.ins[2].mac = 14;
    CHECK(!gsk_program_supported(&p), "an unknown MAC operation is not");
    p.ins[2].mac = 4;
    p.n = 0;
    CHECK(!gsk_program_supported(&p), "an empty program is not");

    CHECK(gsk_type_supported(GEOM_FLOAT3) && gsk_type_supported(GEOM_NORMPACKED3) && gsk_type_supported(GEOM_SHORT3) &&
              gsk_type_supported(GEOM_D3DCOLOR) && gsk_type_supported(GEOM_PBYTE4) && gsk_type_supported(GEOM_NORMSHORT2) &&
              gsk_type_supported(GEOM_NONE),
          "the character format's types and the other GEOM types are fetched");
    CHECK(!gsk_type_supported(GEOM_FLOAT2H) && !gsk_type_supported(0x77), "FLOAT2H and unknown types are not");

    /* The character format: stream 0 float3 position, NORMPACKED3 normal, 2x float2; stream 1
     * SHORT3 palette offsets, float3 weights. */
    GeomLayout l = {{{0, 0, GEOM_FLOAT3, 0}, {1, 0, GEOM_NORMPACKED3, 12}, {2, 0, GEOM_FLOAT2, 16}, {3, 0, GEOM_FLOAT2, 24},
                     {4, 1, GEOM_SHORT3, 0}, {5, 1, GEOM_FLOAT3, 6}},
                    6, {32, 18, 0, 0}};
    CHECK(gsk_layout_supported(&l), "the character layout is supported");
    GskJob j;
    memset(&j, 0xAB, sizeof j);
    gsk_job_layout(&j, &l);
    CHECK(j.nel == 6, "6 elements (%u)", j.nel);
    CHECK(j.fed == 0x3F, "registers 0-5 fed (%x)", j.fed);
    CHECK(j.el[1] == (1u | 0u << 4 | (uint32_t)GEOM_NORMPACKED3 << 8 | 12u << 16), "element 1 packed (%08x)", j.el[1]);
    CHECK(j.el[5] == (5u | 1u << 4 | (uint32_t)GEOM_FLOAT3 << 8 | 6u << 16), "element 5 packed (%08x)", j.el[5]);
    GeomLayout bad = l;
    bad.e[2].type = GEOM_FLOAT2H;
    CHECK(!gsk_layout_supported(&bad), "a FLOAT2H element is not supported");
    bad = l;
    bad.e[0].reg = 7;
    CHECK(!gsk_layout_supported(&bad), "no position input: not supported");
    bad = l;
    bad.e[6] = (GeomElement){20, 2, GEOM_FLOAT4, 0};   /* register >= 16: not an input, ignored */
    bad.n = 7;
    CHECK(gsk_layout_supported(&bad), "non-input registers are ignored");
    gsk_job_layout(&j, &bad);
    CHECK(j.nel == 6, "and left out of the job (%u)", j.nel);
    CHECK(sizeof(GskJob) == 176, "GskJob is 44 words (std430 Job) (%zu)", sizeof(GskJob));
}

static void test_packing(void) {
    srand(7);
    VshProgram p;
    memset(&p, 0, sizeof p);
    p.n = VSH_MAX_INSTRUCTIONS;
    for (uint32_t i = 0; i < p.n; i++) {
        VshIns *x = &p.ins[i];
        x->mac = rand() % 14;
        x->ilu = rand() % 8;
        x->a0x = rand() % 2;
        x->mac_mask = rand() % 16;
        x->ilu_mask = rand() % 16;
        x->out_r = rand() % 16;
        x->o_mask = rand() % 16;
        x->o_reg = rand() % 2;
        x->o_addr = rand() % 256;
        x->o_ilu = rand() % 2;
        x->vreg = rand() % 16;
        x->creg = rand() % 256;
        VshSrc *s[3] = {&x->a, &x->b, &x->c};
        for (int k = 0; k < 3; k++) {
            s[k]->mux = rand() % 4;
            s[k]->reg = rand() % 16;
            s[k]->neg = rand() % 2;
            for (int q = 0; q < 4; q++) s[k]->swz[q] = rand() % 4;
        }
    }
    static uint32_t w[VSH_MAX_INSTRUCTIONS][4];
    gsk_pack_program(&p, w);
    int bad = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        VshIns y;
        gsk_unpack_ins(w[i], &y);
        bad += memcmp(&y, &p.ins[i], sizeof y) != 0;
    }
    CHECK(!bad, "%d of %u random instructions do not round-trip", bad, p.n);
    VshIns one;
    memset(&one, 0, sizeof one);
    one.mac = 4;
    one.creg = 0xC5;
    one.o_addr = 9;
    one.a = (VshSrc){3, 0, 1, {3, 2, 1, 0}};
    VshProgram q;
    memset(&q, 0, sizeof q);
    q.n = 1;
    q.ins[0] = one;
    gsk_pack_program(&q, w);
    CHECK(w[0][0] == 4 && w[0][1] == (9u | 0xC5u << 8), "w0/w1 fields (%08x %08x)", w[0][0], w[0][1]);
    CHECK(w[0][2] == (3u | 1u << 6 | 3u << 7 | 2u << 9 | 1u << 11 | 0u << 13), "operand a bits (%08x)", w[0][2]);

    /* Constants: the recorded palette as is, the screen constants c-38/c-37 = 1 and 0. */
    static float c[VSH_NCONST][4], o[VSH_NCONST][4];
    for (int i = 0; i < VSH_NCONST; i++)
        for (int k = 0; k < 4; k++) c[i][k] = (float)(i * 4 + k) + 0.25f;
    gsk_pack_constants((const float (*)[4])c, o);
    int diff = 0;
    for (int i = 0; i < VSH_NCONST; i++)
        for (int k = 0; k < 4; k++) {
            float want = i == 58 ? 1.0f : i == 59 ? 0.0f : c[i][k];
            diff += o[i][k] != want;
        }
    CHECK(!diff, "%d constant components differ from the recorded ones (screen constants neutralised)", diff);
    CHECK(o[60][0] == c[60][0] && o[60 + 95][3] == c[60 + 95][3], "the bone palette (c-36 on) is unchanged");
}

/* A quad split in two triangles, its vertex 1 duplicated (a UV seam: vertex 4 has vertex 1's
 * position and bones), and a triangle with an out-of-range index. */
static void test_adjacency(void) {
    const uint8_t keys[5][2] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {2, 0}};
    const uint32_t idx[] = {0, 1, 2, 4, 3, 2, 0, 1, 9};
    GskAdjacency a;
    CHECK(!gsk_adjacency_build(idx, 9, 5, &keys[0][0], 2, &a), "built");
    CHECK(a.nv == 5 && a.ng == 4, "5 vertices in 4 groups (%u, %u)", a.nv, a.ng);
    CHECK(a.group[1] == a.group[4], "the seam vertices share a group");
    CHECK(a.group[0] != a.group[1] && a.group[2] != a.group[3] && a.group[0] != a.group[3], "the others do not");
    CHECK(a.ncorner == 6, "two triangles' corners (%u)", a.ncorner);
    const uint32_t g = a.group[1], n = a.start[g + 1] - a.start[g];
    CHECK(n == 2 && a.corner[a.start[g]] == 0 && a.corner[a.start[g] + 1] == 1, "the seam group has a corner in each triangle (%u)", n);
    const uint32_t g2 = a.group[2];
    CHECK(a.start[g2 + 1] - a.start[g2] == 2, "vertex 2 is a corner of both triangles");
    CHECK(a.start[a.ng] == a.ncorner, "start ends at the corner count");
    gsk_adjacency_free(&a);
    CHECK(!a.group && !a.ng, "freed");
    CHECK(!gsk_adjacency_build(idx, 0, 0, NULL, 2, &a) && a.ng == 0 && a.ncorner == 0, "an empty mesh");
    gsk_adjacency_free(&a);

    const uint8_t buf[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(gsk_hash_bytes(1, buf, 11) != gsk_hash_bytes(1, buf, 10), "the tail bytes count");
    CHECK(gsk_hash_bytes(1, buf, 11) == gsk_hash_bytes(1, buf, 11), "deterministic");
    CHECK(gsk_hash_bytes(1, buf, 11) != gsk_hash_bytes(2, buf, 11), "seeded");
}

/* A world program's eligibility for the device: what the shader implements, not a hash list. */
static void test_world_eligible(void) {
    VshProgram p;
    memset(&p, 0, sizeof p);
    p.n = 4;
    p.ins[0].mac = 7;    /* DP4 */
    p.ins[1].mac = 4;    /* MAD */
    p.ins[2].ilu = 5;    /* EXP */
    p.ins[3].mac = 13;   /* ARL (no relative read: not a palette) */
    GeomLayout l = {{{0, 0, GEOM_FLOAT3, 0}, {3, 0, GEOM_D3DCOLOR, 12}, {9, 0, GEOM_FLOAT2, 16}}, 3, {24, 0, 0, 0}};
    char why[128];
    why[0] = 'x';
    CHECK(gsk_world_program_eligible(&p, &l, why, sizeof why), "a supported program and layout run on the device (%s)", why);
    CHECK(why[0] == 0, "no reason when eligible (%s)", why);
    /* Any hash: the old list's programs and one never seen before are judged alike. */
    p.ins[1].mac = 14;
    CHECK(!gsk_world_program_eligible(&p, &l, why, sizeof why), "an unknown MAC operation stays on the CPU");
    CHECK(strstr(why, "instruction 1") && strstr(why, "MAC 14"), "and says which (%s)", why);
    p.ins[1].mac = 4;
    p.ins[2].mac = 15;
    CHECK(!gsk_world_program_eligible(&p, &l, why, sizeof why) && strstr(why, "instruction 2") && strstr(why, "MAC 15"), "MAC 15 (%s)", why);
    p.ins[2].mac = 0;
    p.n = 0;
    CHECK(!gsk_world_program_eligible(&p, &l, why, sizeof why) && strstr(why, "no instructions"), "an empty program (%s)", why);
    p.n = 4;
    GeomLayout bad = l;
    bad.e[2].type = GEOM_FLOAT2H;
    CHECK(!gsk_world_program_eligible(&p, &bad, why, sizeof why) && strstr(why, "v9") && strstr(why, "type"), "a FLOAT2H input (%s)", why);
    bad = l;
    bad.e[1].stream = 4;
    CHECK(!gsk_world_program_eligible(&p, &bad, why, sizeof why) && strstr(why, "v3") && strstr(why, "stream 4"), "stream 4 (%s)", why);
    bad = l;
    bad.e[0].reg = 7;
    CHECK(!gsk_world_program_eligible(&p, &bad, why, sizeof why) && strstr(why, "position"), "no v0 (%s)", why);
    bad = l;
    bad.n = 0;
    for (int r = 0; r < 16; r++) bad.e[bad.n++] = (GeomElement){(uint8_t)r, 0, GEOM_FLOAT1, (uint16_t)(4 * r)};
    CHECK(gsk_world_program_eligible(&p, &bad, why, sizeof why), "16 inputs fit (%s)", why);
    bad.e[15] = (GeomElement){20, 0, GEOM_FLOAT1, 0};   /* register >= 16: not an input */
    CHECK(gsk_world_program_eligible(&p, &bad, why, sizeof why), "a non-input element is ignored (%s)", why);
    /* A bone palette (arl and relative constant reads) is a character's, never a world program. */
    p.ins[1].a0x = 1;
    p.ins[1].a.mux = 3;
    CHECK(!gsk_world_program_eligible(&p, &l, why, sizeof why) && strstr(why, "palette"), "a palette program (%s)", why);
    p.ins[1].a0x = 0;
    CHECK(gsk_world_program_eligible(&p, &l, NULL, 0), "why may be NULL");
    char tiny[8];
    bad.e[0].reg = 7;
    gsk_world_program_eligible(&p, &bad, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny, "the reason is cut to the buffer");

    /* One log line per program (hash), however often the title creates it. */
    CHECK(gsk_log_once(0x1234), "first time: log");
    CHECK(!gsk_log_once(0x1234), "second time: not");
    CHECK(gsk_log_once(0x5678), "another program: log");
}

/* XB_GSK_CHECK: the CPU-vs-GPU errors per program. */
static void test_check_programs(void) {
    gsk_check_reset();
    gsk_check_note(0xAAull, 100, 1e-6, 2e-7, 0, 0);
    gsk_check_note(0xBBull, 50, 3e-3, 0, 0, 0);
    gsk_check_note(0xAAull, 20, 5e-6, 1e-7, 1e-7, 0);
    const GskCheckProg *c;
    const uint32_t n = gsk_check_programs(&c);
    CHECK(n == 2, "two programs (%u)", n);
    const GskCheckProg *a = n == 2 ? (c[0].hash == 0xAA ? &c[0] : &c[1]) : NULL, *b = a == &c[0] ? &c[1] : &c[0];
    if (a) {
        CHECK(a->draws == 2 && a->verts == 120, "draws and vertices add up (%u, %u)", a->draws, a->verts);
        CHECK(a->pos == 5e-6 && a->tex == 2e-7 && a->fog == 1e-7, "the worst errors are kept");
        CHECK(gsk_check_within(a), "within tolerance");
        CHECK(!gsk_check_within(b), "a position off by 3e-3 of max(|w|, 1) is not");
    }
    GskCheckProg q = {0};
    q.col_off = 1;
    CHECK(!gsk_check_within(&q), "a colour channel off by more than 1 is not");
    q.col_off = 0;
    q.tex = 2e-3;
    CHECK(!gsk_check_within(&q), "texture coordinates off by 2e-3 (relative) are not");
    q.tex = 0;
    q.fog = 2e-3;
    CHECK(!gsk_check_within(&q), "fog off by 2e-3 is not");
    gsk_check_reset();
    CHECK(gsk_check_programs(&c) == 0, "reset");
}

int main(void) {
    test_options();
    test_matcher();
    test_world_eligible();
    test_check_programs();
    test_packing();
    test_adjacency();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
