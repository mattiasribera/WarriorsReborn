/* --gpu-skinning helpers (gpu_skin.h). */
#include "gpu_skin.h"
#include <stdlib.h>
#include <string.h>

int gpu_skin_arg(GpuSkinConfig *c, const char *opt) {
    if (!strcmp(opt, "--gpu-skinning") || !strcmp(opt, "--no-gpu-skinning")) {
        c->on = opt[2] == 'g';
        return 1;
    }
    if (!strcmp(opt, "--gpu-vertex-programs") || !strcmp(opt, "--no-gpu-vertex-programs")) {
        c->world = opt[2] == 'g';
        return 1;
    }
    return 0;
}

uint64_t gsk_program_hash(const uint32_t *function, uint32_t ndwords) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t *p = (const uint8_t *)function;
    for (size_t i = 0; i < (size_t)ndwords * 4; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* The title's two character programs (vsCharacter): 69 and 23
 * instructions. */
static const uint64_t allowed[] = {0xb031486a132e9bc0ull, 0x94ac45eefa1d8809ull};
int gsk_program_allowed(uint64_t hash) {
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (allowed[i] == hash) return 1;
    return 0;
}

/* The title's world and object programs seen in the hideout and mission 8 (XB_GSK_CHECK
 * and same-frame A/B): instruction counts 21, 60, 58, 54, 21, 58, 60, 7. */
static const uint64_t world_allowed[] = {0x07b80c34cbd02b70ull, 0xfe21acdbabb3d365ull, 0x6443c2b26f986af7ull, 0x16afd01778316737ull,
                                         0xed8687a9bc7c452bull, 0xe58427ea10aa8444ull, 0x3a658fa0f386f99bull, 0xe10ebcbe3cff479dull};
int gsk_world_program_allowed(uint64_t hash) {
    for (size_t i = 0; i < sizeof world_allowed / sizeof world_allowed[0]; i++)
        if (world_allowed[i] == hash) return 1;
    return 0;
}

int gsk_program_supported(const VshProgram *p) {
    if (!p->n || p->n > VSH_MAX_INSTRUCTIONS) return 0;
    for (uint32_t i = 0; i < p->n; i++)
        if (p->ins[i].mac > 13 || p->ins[i].ilu > 7) return 0;   /* MAC_ARL, ILU_LIT (vsh.c) */
    return 1;
}

int gsk_type_supported(uint32_t type) {
    switch (type) {
    case GEOM_FLOAT1: case GEOM_FLOAT2: case GEOM_FLOAT3: case GEOM_FLOAT4:
    case GEOM_D3DCOLOR:
    case GEOM_SHORT1: case GEOM_SHORT2: case GEOM_SHORT3: case GEOM_SHORT4:
    case GEOM_NORMSHORT1: case GEOM_NORMSHORT2: case GEOM_NORMSHORT3: case GEOM_NORMSHORT4:
    case GEOM_NORMPACKED3:
    case GEOM_PBYTE1: case GEOM_PBYTE2: case GEOM_PBYTE3: case GEOM_PBYTE4:
    case GEOM_NONE:
        return 1;
    default:
        return 0;
    }
}

int gsk_layout_supported(const GeomLayout *l) {
    int n = 0, pos = 0;
    for (int k = 0; k < l->n; k++) {
        const GeomElement *e = &l->e[k];
        if (e->reg >= 16) continue;
        if (!gsk_type_supported(e->type) || e->stream > 3 || ++n > GSK_MAX_ELEMENTS) return 0;
        pos |= e->reg == 0;
    }
    return pos;
}

static uint32_t pack_src(const VshSrc *s) {
    return (uint32_t)(s->mux & 3) | (uint32_t)(s->reg & 15) << 2 | (uint32_t)(s->neg & 1) << 6 | (uint32_t)(s->swz[0] & 3) << 7 |
           (uint32_t)(s->swz[1] & 3) << 9 | (uint32_t)(s->swz[2] & 3) << 11 | (uint32_t)(s->swz[3] & 3) << 13;
}
static void unpack_src(uint32_t w, VshSrc *s) {
    s->mux = w & 3;
    s->reg = w >> 2 & 15;
    s->neg = w >> 6 & 1;
    for (int k = 0; k < 4; k++) s->swz[k] = w >> (7 + 2 * k) & 3;
}

void gsk_pack_program(const VshProgram *p, uint32_t (*words)[4]) {
    for (uint32_t i = 0; i < p->n; i++) {
        const VshIns *x = &p->ins[i];
        words[i][0] = (uint32_t)(x->mac & 15) | (uint32_t)(x->ilu & 7) << 4 | (uint32_t)(x->a0x & 1) << 7 | (uint32_t)(x->mac_mask & 15) << 8 |
                      (uint32_t)(x->ilu_mask & 15) << 12 | (uint32_t)(x->out_r & 15) << 16 | (uint32_t)(x->o_mask & 15) << 20 |
                      (uint32_t)(x->o_reg & 1) << 24 | (uint32_t)(x->o_ilu & 1) << 25 | (uint32_t)(x->vreg & 15) << 26;
        words[i][1] = (uint32_t)x->o_addr | (uint32_t)(x->creg & 255) << 8;
        words[i][2] = pack_src(&x->a) | pack_src(&x->b) << 15;
        words[i][3] = pack_src(&x->c);
    }
}

void gsk_unpack_ins(const uint32_t w[4], VshIns *x) {
    memset(x, 0, sizeof *x);
    x->mac = w[0] & 15;
    x->ilu = w[0] >> 4 & 7;
    x->a0x = w[0] >> 7 & 1;
    x->mac_mask = w[0] >> 8 & 15;
    x->ilu_mask = w[0] >> 12 & 15;
    x->out_r = w[0] >> 16 & 15;
    x->o_mask = w[0] >> 20 & 15;
    x->o_reg = w[0] >> 24 & 1;
    x->o_ilu = w[0] >> 25 & 1;
    x->vreg = w[0] >> 26 & 15;
    x->o_addr = w[1] & 255;
    x->creg = w[1] >> 8 & 255;
    unpack_src(w[2] & 0x7FFF, &x->a);
    unpack_src(w[2] >> 15 & 0x7FFF, &x->b);
    unpack_src(w[3] & 0x7FFF, &x->c);
}

void gsk_pack_constants(const float (*vsc)[4], float out[VSH_NCONST][4]) {
    memcpy(out, vsc, VSH_NCONST * 4 * sizeof(float));
    vsh_set_screen_constants(out);
}

void gsk_job_layout(GskJob *j, const GeomLayout *l) {
    j->nel = j->fed = 0;
    for (int k = 0; k < l->n && j->nel < GSK_MAX_ELEMENTS; k++) {
        const GeomElement *e = &l->e[k];
        if (e->reg >= 16) continue;
        j->el[j->nel++] = (uint32_t)e->reg | (uint32_t)e->stream << 4 | (uint32_t)e->type << 8 | (uint32_t)e->offset << 16;
        j->fed |= 1u << e->reg;
    }
    for (uint32_t k = j->nel; k < GSK_MAX_ELEMENTS; k++) j->el[k] = 0;
}

/* ---- weld groups ---- */
typedef struct {
    const uint8_t *keys;
    uint32_t bytes;
} KeyCtx;
static KeyCtx key_ctx;   /* qsort has no context argument; one caller (the host thread) */
static int key_cmp(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    const int c = memcmp(key_ctx.keys + (size_t)x * key_ctx.bytes, key_ctx.keys + (size_t)y * key_ctx.bytes, key_ctx.bytes);
    return c ? c : (x > y) - (x < y);
}

void gsk_adjacency_free(GskAdjacency *a) {
    free(a->group);
    free(a->start);
    free(a->corner);
    memset(a, 0, sizeof *a);
}

int gsk_adjacency_build(const uint32_t *idx, uint32_t ni, uint32_t nv, const uint8_t *keys, uint32_t key_bytes, GskAdjacency *a) {
    memset(a, 0, sizeof *a);
    uint32_t *order = malloc(((size_t)nv + 1) * sizeof *order);
    a->group = malloc(((size_t)nv + 1) * sizeof *a->group);
    a->start = calloc((size_t)nv + 2, sizeof *a->start);
    if (!order || !a->group || !a->start) goto oom;
    /* Groups: vertices with equal keys, numbered in sorted key order. */
    for (uint32_t i = 0; i < nv; i++) order[i] = i;
    key_ctx = (KeyCtx){keys, key_bytes};
    if (nv > 1 && key_bytes) qsort(order, nv, sizeof *order, key_cmp);
    uint32_t ng = 0;
    for (uint32_t i = 0; i < nv; i++) {
        if (i && (!key_bytes || memcmp(keys + (size_t)order[i] * key_bytes, keys + (size_t)order[i - 1] * key_bytes, key_bytes))) ng++;
        a->group[order[i]] = ng;
    }
    a->ng = nv ? ng + 1 : 0;
    a->nv = nv;
    /* Corners per group (counting pass, prefix sums, fill in index order). */
    uint32_t nt = ni / 3, nc = 0;
    for (uint32_t t = 0; t < nt; t++) {
        const uint32_t *c = idx + 3 * t;
        if (c[0] >= nv || c[1] >= nv || c[2] >= nv) continue;
        for (int k = 0; k < 3; k++) a->start[a->group[c[k]] + 1]++;
        nc += 3;
    }
    for (uint32_t g = 0; g < a->ng; g++) a->start[g + 1] += a->start[g];
    a->corner = malloc(((size_t)nc + 1) * sizeof *a->corner);
    if (!a->corner) goto oom;
    memcpy(order, a->start, (size_t)(a->ng ? a->ng : 1) * sizeof *order);   /* fill cursors */
    for (uint32_t t = 0; t < nt; t++) {
        const uint32_t *c = idx + 3 * t;
        if (c[0] >= nv || c[1] >= nv || c[2] >= nv) continue;
        for (int k = 0; k < 3; k++) a->corner[order[a->group[c[k]]]++] = t;
    }
    a->ncorner = nc;
    free(order);
    return 0;
oom:
    free(order);
    gsk_adjacency_free(a);
    return -1;
}

uint64_t gsk_hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    const uint64_t m = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * m;
        h ^= h >> 29;
    }
    uint64_t t = 0;
    for (size_t k = 0; i + k < n; k++) t |= (uint64_t)b[i + k] << (8 * k);
    h = (h ^ t ^ (uint64_t)n << 56) * m;
    return h ^ (h >> 32);
}
