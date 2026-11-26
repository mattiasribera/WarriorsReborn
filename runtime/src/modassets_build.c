/* The build steps of assets.ini (docs/MODDING.md "assets.ini"): sources from the disc, UV islands,
 * head swaps, headgear cut out as a rigid object, texture pastes, colour masks, procedural fills,
 * atlases, the shadow-box frame, broken glasses and the surface effects of texture layers (dirt,
 * stains and tears over a character's clothes), written as mesh override / texture / layer files.
 *
 * Every step reproduces the original Python recipes the assets were designed with, operation by operation, in
 * double precision (Python floats), with Python's rounding rules (round(x, n) correctly rounded,
 * round half to even) and Python's random numbers (src/pyrand.h), so the files match theirs: the
 * meshes byte for byte and the textures texel for texel. No fused multiply-add here. */
#pragma STDC FP_CONTRACT OFF
#include "modassets.h"
#include "disc.h"
#include "meshpack.h"
#include "pyrand.h"
#include "texpack.h"
#include "xtex.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define ma_mkdir(p) _mkdir(p)
#else
#define ma_mkdir(p) mkdir(p, 0755)
#endif

#define MAX_SOURCES 16
#define MAX_TEXTURES 32
#define MAX_REGIONS 8
#define MAX_SKINS 32

/* The game's 34 rig joints: ma_joints (src/modassets_own.c); palette slot k = joint k + 2. */
#define joints ma_joints

/* Host objects a mod may draw its own mesh as: objects of the game that no level, script or
 * character places. Each is a static prop chunk of the disc (its 0x47 hash); the draw the game makes
 * of it is that chunk's vertex and index bytes as stored, so its content hash (`xbrun --mesh-log`)
 * is computed from the disc (host_resolve). `hash` is the draw hash `--mesh-log` logs on the
 * developer's disc, used only when a disc lacks the chunk. The engine writes one layout: float3
 * position, float3 normal, D3DCOLOR diffuse, float2 uv (36 bytes, FVF 0x152). */
typedef struct {
    const char *model;
    uint32_t prop;
    uint64_t hash;
} HostKnown;
static const HostKnown hosts[] = {
    {"dyn_dest_ltb", 0x17268DF0u, 0x6830D7EB9625CFADull},       /* a Destroyers hat (98 triangles) */
    {"dyn_s_paper_list", 0xF616FD29u, 0x7B6F89AA421DA125ull},   /* a sheet of paper (8 triangles) */
    {"dyn_civl_hl_ho", 0xBC02029Bu, 0x5399C5A77DD5DD91ull},     /* a hat (116 triangles) */
    {"dyn_pl_dr", 0x9B1C1D32u, 0x1A086F6393580BF8ull},          /* a hat (177 triangles) */
    {"dyn_s_newspaper_c", 0x3394B3BCu, 0xC52A15938C5F92B8ull},  /* a folded newspaper (40 triangles) */
};
#define HOST_NAMES "dyn_dest_ltb, dyn_s_paper_list, dyn_civl_hl_ho, dyn_pl_dr, dyn_s_newspaper_c"
typedef struct {
    uint64_t hash;
    int stride, pos, normal, colour, uv;
} Host;

typedef struct {
    int *tris;   /* triangle numbers (t: indices 3t..3t+2) */
    int n;
    double zmin, zmax, u0, v0, u1, v1;
} Island;

typedef struct {
    int w, h;
    uint8_t *rgba;
    uint64_t hash;   /* runtime texture hash (texpack_hash) */
} Skin;

typedef struct {
    char name[64], model[64];
    int line;
    int ready;
    int is_prop;      /* prop = (a static prop), else char = */
    int is_own;       /* file = own/NAME.glb: the mod's own mesh (own) */
    MaOwnMesh own;
    uint8_t *payload;
    size_t size;
    uint32_t chunk_hash, item_hash;
    DiscChar ch;
    DiscProp pr;
    uint16_t *pidx;   /* a prop's triangle-list indices (a strip unrolled as mesh.parse_static_prop) */
    int pni;
    uint64_t mesh_hash;
    Island *isl;
    int nisl;
    DiscSkin skins[MAX_SKINS];
    int nskins;
    Skin skin[MAX_SKINS];
} Source;

typedef struct {
    char name[64];
    int x, y, w, h;
} Region;

typedef struct {
    char name[64];
    int w, h;
    uint8_t *rgba;
    uint8_t *mask;   /* mask = ...: one byte per texel, or NULL */
    Region reg[MAX_REGIONS];
    int nreg;
} Tex;

/* [set NAME]: CODE = MODEL[, MODEL...] lines, the entries a [layer] runs for (for = NAME). */
#define MAX_SETS 8
#define SET_MAX 32
#define SET_MODELS 4
typedef struct {
    char code[32];
    char model[SET_MODELS][64];
    int nm, line;
} SetEntry;
typedef struct {
    char name[64];
    SetEntry e[SET_MAX];
    int n;
} Set;

typedef struct {
    MaRecipe *r;
    const char *disc_dir;
    Disc *disc;       /* the shared scan (below) */
    int disc_state;   /* 0 not opened, 1 scanned, -1 failed */
    DiscSkin *recs;
    int nrec;
    Source src[MAX_SOURCES];
    int nsrc;
    Tex tex[MAX_TEXTURES];
    int ntex;
    Set set[MAX_SETS];
    int nset;
    const char *out;
    int files;
    uint64_t bytes;
    double t0;
    char *err;
    size_t cap;
    uint64_t sound_bytes;   /* decoded [sound] samples written (MA_SOUND_BYTES_MAX) */
} Ctx;

/* The first warning of a build (MaRecipe.warn). */
static void warn(Ctx *c, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void warn(Ctx *c, int line, const char *fmt, ...) {
    if (c->r->warn[0]) return;
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(c->r->warn, sizeof c->r->warn, "%s:%d: %s", c->r->file, line, msg);
}

static int ferr(Ctx *c, int line, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (c->err && c->cap) snprintf(c->err, c->cap, "%s:%d: %s", c->r->file, line, msg);
    return -1;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int over_time(Ctx *c, int line) {
    if (now_ms() - c->t0 <= modassets_time_ms) return 0;
    ferr(c, line, "the build took more than %u ms (budget); stopped", modassets_time_ms);
    return 1;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}
static void wrf(uint8_t *p, double v) {
    float f = (float)v;
    memcpy(p, &f, 4);
}

/* Python's round(x, nd): correctly rounded to nd decimals, ties to even (printf of the exact
 * binary value does the same); -0.0 becomes 0.0 (the two compare equal in Python). */
static double py_round(double x, int nd) {
    char b[80];
    snprintf(b, sizeof b, "%.*f", nd, x);
    return strtod(b, NULL) + 0.0;
}

/* ---- value parsing ---- */

/* Splits v at the commas outside parentheses into at most max clauses (trimmed). */
static int clauses(const char *v, char out[][160], int max) {
    int n = 0, depth = 0;
    size_t k = 0;
    for (const char *s = v;; s++) {
        if (!*s || (*s == ',' && !depth)) {
            if (n < max) {
                out[n][k < 159 ? k : 159] = 0;
                char *t = out[n];
                while (*t == ' ' || *t == '\t') memmove(t, t + 1, strlen(t));
                size_t L = strlen(t);
                while (L && (t[L - 1] == ' ' || t[L - 1] == '\t')) t[--L] = 0;
                n++;
            }
            k = 0;
            if (!*s) break;
            continue;
        }
        if (*s == '(') depth++;
        if (*s == ')') depth--;
        if (n < max && k < 159) out[n][k++] = *s;
    }
    return n;
}

/* Splits a clause into words at spaces outside parentheses. */
static int words(const char *v, char out[][160], int max) {
    int n = 0, depth = 0;
    size_t k = 0;
    for (const char *s = v;; s++) {
        int sep = !*s || ((*s == ' ' || *s == '\t') && !depth);
        if (sep) {
            if (k && n < max) {
                out[n][k < 159 ? k : 159] = 0;
                n++;
            }
            k = 0;
            if (!*s) break;
            continue;
        }
        if (*s == '(') depth++;
        if (*s == ')') depth--;
        if (n < max && k < 159) out[n][k++] = *s;
    }
    return n;
}

static int num(const char *s, double *v) {
    char *end;
    *v = strtod(s, &end);
    return end != s && !*end && isfinite(*v);
}

static int inum(const char *s, int *v) {
    double d;
    if (!num(s, &d) || d != floor(d) || d < -1e9 || d > 1e9) return 0;
    *v = (int)d;
    return 1;
}

static const MaLine *line_of(const MaSection *s, const MaRecipe *r, const char *key) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(r->lines[s->first + i].key, key)) return &r->lines[s->first + i];
    return NULL;
}

/* ---- the disc ---- */

/* One scan of the disc per process: every mod of a run builds from the same disc, so the second
 * mod's build reuses the first's chunk list and skin table (same folder, same XBoxWad.idx). */
static struct {
    char dir[1024];
    uint64_t fingerprint;
    int ok;
    Disc d;
    DiscSkin *recs;
    int nrec;
} shared;

static char index_path[1300];
void ma_set_index_path(const char *path) { snprintf(index_path, sizeof index_path, "%s", path ? path : ""); }

static int disc_ready(Ctx *c, int line) {
    if (c->disc_state == 1) return 0;
    if (c->disc_state < 0) return ferr(c, line, "no game files in %s", c->disc_dir);
    c->disc_state = -1;
    Disc probe;
    if (disc_open(&probe, c->disc_dir)) return ferr(c, line, "no game files in %s", c->disc_dir);
    if (shared.ok && !strcmp(shared.dir, c->disc_dir) && shared.fingerprint == probe.fingerprint) {
        disc_close(&probe);
    } else {
        if (shared.ok) disc_close(&shared.d), free(shared.recs);
        memset(&shared, 0, sizeof shared);
        shared.d = probe;
        if (disc_scan_cached(&shared.d, index_path, NULL) <= 0) {
            disc_close(&shared.d);
            memset(&shared, 0, sizeof shared);
            return ferr(c, line, "no game files in %s", c->disc_dir);
        }
        Disc *d = &shared.d;
        /* bindings.load_skin_bindings: every 0x44 table once (the glr is stored twice) */
        uint8_t **seen = calloc(d->ntables + 1, sizeof *seen);
        uint32_t *seen_n = calloc(d->ntables + 1, sizeof *seen_n);
        int nseen = 0;
        for (uint32_t i = 0; seen && seen_n && i < d->ntables; i++) {
            const DiscChunk *t = &d->tables[i];
            uint8_t *p = disc_chunk_read(d, t);
            if (!p) continue;
            int dup = 0;
            for (int k = 0; k < nseen && !dup; k++) dup = seen_n[k] == t->size && !memcmp(seen[k], p, t->size);
            int n = dup ? 0 : disc_skins_parse(p, t->size, NULL, 0);
            if (n > 0) {
                DiscSkin *nr = realloc(shared.recs, sizeof *nr * (size_t)(shared.nrec + n));
                if (nr) {
                    shared.recs = nr;
                    shared.nrec += disc_skins_parse(p, t->size, nr + shared.nrec, n);
                }
                seen[nseen] = p, seen_n[nseen++] = t->size;
            } else {
                free(p);
            }
        }
        for (int k = 0; k < nseen; k++) free(seen[k]);
        free(seen), free(seen_n);
        snprintf(shared.dir, sizeof shared.dir, "%s", c->disc_dir);
        shared.fingerprint = d->fingerprint;
        shared.ok = 1;
    }
    c->disc = &shared.d;
    c->recs = shared.recs;
    c->nrec = shared.nrec;
    c->disc_state = 1;
    return 0;
}

/* The first chunk of class cls and this hash (head8: for 0x47, the kind's first two words). */
static const DiscChunk *find_chunk(Ctx *c, uint32_t cls, uint32_t hash, uint32_t h0, uint32_t h1) {
    for (uint32_t i = 0; i < c->disc->nchunks; i++) {
        const DiscChunk *k = &c->disc->chunks[i];
        if (k->cls != cls || k->hash != hash) continue;
        if (cls != DISC_CLASS_MESH) return k;
        uint8_t h[16];
        if (k->size >= 8 && !disc_pread(c->disc, k->file, k->off, h, 8) && rd32(h) == h0 && rd32(h + 4) == h1) return k;
    }
    return NULL;
}

/* "0x" and 8 hex digits: a chunk hash (a model the scripts name by a record of another name). */
static int hex_hash(const char *s, uint32_t *v) {
    if (strncmp(s, "0x", 2) || strlen(s) != 10) return 0;
    char *end;
    *v = (uint32_t)strtoul(s + 2, &end, 16);
    return !*end;
}


/* ---- the mod's own files (own/, src/modassets_own.c) ---- */

static int ext_is(const char *s, const char *ext) {
    size_t a = strlen(s), b = strlen(ext);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if ((s[a - b + i] | 0x20) != ext[i]) return 0;
    return 1;
}

/* The bytes of own file rel (validated: inside the mod's own/ folder), malloc'd. */
static uint8_t *own_read(Ctx *c, const char *rel, size_t *n, int line) {
    char why[200];
    if (ma_own_path_check(rel, why, sizeof why)) return ferr(c, line, "%s", why), NULL;
    if (!c->r->dir[0]) return ferr(c, line, "%s: no mod folder to read it from", rel), NULL;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", c->r->dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return ferr(c, line, "%s: not in the mod's folder", rel), NULL;
    long len = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    uint8_t *b = NULL;
    if (len >= 0 && len <= (long)MA_OWN_BYTES_MAX && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)len + 1)) && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (!b) return ferr(c, line, "%s: cannot read it (at most %u MiB)", rel, MA_OWN_BYTES_MAX >> 20), NULL;
    *n = (size_t)len;
    return b;
}

static Source *own_load(Ctx *c, Source *s) {
    size_t n;
    uint8_t *b = own_read(c, s->model, &n, s->line);
    if (!b) return NULL;
    char why[300];
    int r = ma_glb_load(b, n, &s->own, why, sizeof why);
    free(b);
    if (r) return ferr(c, s->line, "%s: %s", s->model, why), NULL;
    if (s->own.trimmed) warn(c, s->line, "%s: %d vertices had more than 3 joints: the 3 strongest were kept", s->model, s->own.trimmed);
    else if (s->own.moved) warn(c, s->line, "%s: the skinned mesh's object is moved, turned or scaled: ignored (the game uses its rest pose)", s->model);
    s->ready = 1;
    return s;
}

/* Reads source s (its model, is_prop and line set) from the disc. */
static Source *source_load(Ctx *c, Source *s, int line) {
    if (s->is_own) return own_load(c, s);
    {
        if (disc_ready(c, s->line)) return NULL;
        uint32_t want = 0;
        const int by_hash = hex_hash(s->model, &want);
        if (!by_hash) want = disc_model_hash(s->model);
        const DiscChunk *k = find_chunk(c, DISC_CLASS_MESH, want, s->is_prop ? 2 : 1, s->is_prop ? 0x6D : 0x76);
        if (!k) return ferr(c, s->line, "%s %s not found on the disc", s->is_prop ? "prop" : "character", s->model), NULL;
        s->payload = disc_chunk_read(c->disc, k);
        s->size = k->size;
        char why[128];
        if (s->is_prop) {
            if (!s->payload || disc_prop_parse(s->payload, s->size, &s->pr, why, sizeof why))
                return ferr(c, s->line, "prop %s: %s", s->model, s->payload ? why : "read error"), NULL;
            if (s->pr.normal < 0 && s->pr.normal_packed < 0) return ferr(c, s->line, "prop %s has no normals", s->model), NULL;
            if (s->pr.uv0 < 0) return ferr(c, s->line, "prop %s has no UVs", s->model), NULL;
            s->pidx = malloc(sizeof(uint16_t) * (s->pr.nidx * 3 + 3));
            if (!s->pidx) return ferr(c, line, "out of memory"), NULL;
            const uint8_t *ib = s->pr.idx;
            for (uint32_t i = 0; s->pr.prim == 5 && i < s->pr.nidx; i++) s->pidx[s->pni++] = (uint16_t)(ib[2 * i] | ib[2 * i + 1] << 8);
            for (uint32_t i = 0; s->pr.prim == 6 && i + 2 < s->pr.nidx; i++) {
                const uint16_t a = (uint16_t)(ib[2 * i] | ib[2 * i + 1] << 8), b = (uint16_t)(ib[2 * i + 2] | ib[2 * i + 3] << 8),
                               d = (uint16_t)(ib[2 * i + 4] | ib[2 * i + 5] << 8);
                if (a == b || b == d || a == d) continue;
                const uint16_t t[3] = {i % 2 ? b : a, i % 2 ? a : b, d};
                memcpy(s->pidx + s->pni, t, 6), s->pni += 3;
            }
            if (!s->pni) return ferr(c, s->line, "prop %s has no triangles", s->model), NULL;
            s->chunk_hash = k->hash, s->item_hash = k->item;
            s->ready = 1;
            return s;
        }
        if (!s->payload || disc_char_parse(s->payload, s->size, &s->ch, why, sizeof why))
            return ferr(c, s->line, "character %s: %s", s->model, s->payload ? why : "read error"), NULL;
        if (!s->ch.ni) return ferr(c, s->line, "character %s has no triangles", s->model), NULL;
        s->chunk_hash = k->hash;
        s->item_hash = k->item;
        uint32_t lo = 0xffff, hi = 0;
        for (uint32_t t = 0; t < s->ch.ni; t++) {
            uint32_t v = s->ch.idx[2 * t] | s->ch.idx[2 * t + 1] << 8;
            lo = v < lo ? v : lo, hi = v > hi ? v : hi;
        }
        uint16_t *idx = malloc(s->ch.ni * 2);
        if (!idx) return ferr(c, line, "out of memory"), NULL;
        memcpy(idx, s->ch.idx, s->ch.ni * 2);
        s->mesh_hash = meshpack_hash(idx, s->ch.ni, lo, hi, s->ch.s0, DISC_STRIDE0, s->ch.s1, DISC_STRIDE1);
        free(idx);
        s->nskins = disc_skins_for(c->recs, c->nrec, s->chunk_hash, s->item_hash, s->skins, MAX_SKINS);
        s->ready = 1;
        return s;
    }
}

static Source *source(Ctx *c, const char *name, int line) {
    for (int i = 0; i < c->nsrc; i++) {
        Source *s = &c->src[i];
        if (strcmp(s->name, name)) continue;
        return s->ready ? s : source_load(c, s, line);
    }
    return ferr(c, line, "no [source %s] before this line", name), NULL;
}

static void source_free(Source *s) {
    ma_own_mesh_free(&s->own);
    free(s->payload), free(s->pidx);
    for (int k = 0; k < s->nisl; k++) free(s->isl[k].tris);
    free(s->isl);
    for (int k = 0; k < MAX_SKINS; k++) free(s->skin[k].rgba);
}

static uint32_t vidx(const Source *s, int i) { return s->ch.idx[2 * i] | (uint32_t)s->ch.idx[2 * i + 1] << 8; }
static const uint8_t *vtx(const Source *s, uint32_t v) { return s->ch.s0 + (size_t)v * DISC_STRIDE0; }

static Skin *skin(Ctx *c, Source *s, int n, int line) {
    if (n < 0 || n >= MAX_SKINS || n >= s->nskins) return ferr(c, line, "%s has no skin %d", s->model, n), NULL;
    Skin *k = &s->skin[n];
    if (k->rgba) return k;
    const DiscChunk *ch = find_chunk(c, DISC_CLASS_TEXTURE, s->skins[n].tex, 0, 0);
    if (!ch) return ferr(c, line, "the texture of %s skin %d is not on the disc", s->model, n), NULL;
    uint8_t *p = disc_chunk_read(c->disc, ch);
    uint32_t fmt, size, off;
    XTexInfo t;
    if (!p || disc_tex_parse(p, ch->size, &fmt, &size, &off) || xtex_info(fmt, size, &t) || XTEX_DIM(fmt) != 2 || t.w > MODASSETS_TEX_MAX ||
        t.h > MODASSETS_TEX_MAX || off > ch->size || xtex_level_bytes(&t, 0) > ch->size - off) {
        free(p);
        return ferr(c, line, "the texture of %s skin %d cannot be decoded", s->model, n), NULL;
    }
    uint32_t *px = malloc((size_t)t.w * t.h * 4);
    k->rgba = malloc((size_t)t.w * t.h * 4);
    if (!px || !k->rgba || xtex_decode_level(&t, 0, p + off, px)) {
        free(p), free(px), free(k->rgba);
        k->rgba = NULL;
        return ferr(c, line, "the texture of %s skin %d cannot be decoded", s->model, n), NULL;
    }
    for (size_t i = 0; i < (size_t)t.w * t.h; i++) {
        k->rgba[4 * i] = (uint8_t)(px[i] >> 16), k->rgba[4 * i + 1] = (uint8_t)(px[i] >> 8);
        k->rgba[4 * i + 2] = (uint8_t)px[i], k->rgba[4 * i + 3] = (uint8_t)(px[i] >> 24);
    }
    k->w = (int)t.w, k->h = (int)t.h;
    k->hash = texpack_hash(&t, p + off);
    free(px), free(p);
    return k;
}

/* "NAME.skinN" -> source and N. */
static int skin_ref(const char *v, char *name, int *n) {
    const char *dot = strrchr(v, '.');
    if (!dot || strncmp(dot + 1, "skin", 4) || dot == v || (size_t)(dot - v) >= 64) return 0;
    memcpy(name, v, (size_t)(dot - v));
    name[dot - v] = 0;
    return inum(dot + 5, n);
}

/* ---- UV islands (core.uv_islands) ---- */

typedef struct {
    double u, v;
    int i;
} UvKey;

static int cmp_uv(const void *a, const void *b) {
    const UvKey *x = a, *y = b;
    if (x->u != y->u) return x->u < y->u ? -1 : 1;
    if (x->v != y->v) return x->v < y->v ? -1 : 1;
    return x->i - y->i;
}

static int find_root(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

static int cmp_island(const void *a, const void *b) {
    const Island *x = a, *y = b;
    if (x->v0 != y->v0) return x->v0 < y->v0 ? -1 : 1;
    if (x->u0 != y->u0) return x->u0 < y->u0 ? -1 : 1;
    if (x->n != y->n) return x->n < y->n ? -1 : 1;
    return 0;
}

static int islands(Ctx *c, Source *s, int line) {
    if (s->isl) return 0;
    const uint32_t nv = s->ch.nv, nt = s->ch.ni / 3;
    UvKey *k = malloc(sizeof *k * (nv ? nv : 1));
    int *key = malloc(sizeof(int) * (nv ? nv : 1)), *par = malloc(sizeof(int) * (nv ? nv : 1));
    int *gid = malloc(sizeof(int) * (nv ? nv : 1)), *tri_g = malloc(sizeof(int) * (nt ? nt : 1));
    if (!k || !key || !par || !gid || !tri_g) {
        free(k), free(key), free(par), free(gid), free(tri_g);
        return ferr(c, line, "out of memory");
    }
    for (uint32_t v = 0; v < nv; v++) {
        k[v].u = py_round(rdf(vtx(s, v) + 16), 5);
        k[v].v = py_round(rdf(vtx(s, v) + 20), 5);
        k[v].i = (int)v;
    }
    qsort(k, nv, sizeof *k, cmp_uv);
    int nk = 0;
    for (uint32_t i = 0; i < nv; i++) {
        if (i && (k[i].u != k[i - 1].u || k[i].v != k[i - 1].v)) nk++;
        key[k[i].i] = nk;
    }
    for (int i = 0; i <= nk; i++) par[i] = i, gid[i] = -1;
    for (uint32_t t = 0; t < nt; t++)
        for (int j = 1; j < 3; j++) par[find_root(par, key[vidx(s, 3 * t + j)])] = find_root(par, key[vidx(s, 3 * t)]);
    int ng = 0;
    for (uint32_t t = 0; t < nt; t++) {
        int r = find_root(par, key[vidx(s, 3 * t)]);
        if (gid[r] < 0) gid[r] = ng++;
        tri_g[t] = gid[r];
    }
    s->isl = calloc((size_t)(ng ? ng : 1), sizeof *s->isl);
    int ok = s->isl != NULL;
    for (int g = 0; ok && g < ng; g++) {
        Island *is = &s->isl[g];
        is->tris = malloc(sizeof(int) * nt);
        ok = is->tris != NULL;
        is->zmin = is->u0 = is->v0 = INFINITY;
        is->zmax = is->u1 = is->v1 = -INFINITY;
    }
    for (uint32_t t = 0; ok && t < nt; t++) {
        Island *is = &s->isl[tri_g[t]];
        is->tris[is->n++] = (int)t;
        for (int j = 0; j < 3; j++) {
            const uint8_t *p = vtx(s, vidx(s, 3 * (int)t + j));
            double z = rdf(p + 8), u = rdf(p + 16), v = rdf(p + 20);
            if (z < is->zmin) is->zmin = z;
            if (z > is->zmax) is->zmax = z;
            if (u < is->u0) is->u0 = u;
            if (u > is->u1) is->u1 = u;
            if (v < is->v0) is->v0 = v;
            if (v > is->v1) is->v1 = v;
        }
    }
    free(k), free(key), free(par), free(gid), free(tri_g);
    if (!ok) return ferr(c, line, "out of memory");
    s->nisl = ng;
    /* sorted by (v0, u0, triangle count), stable: insertion sort keeps equal islands in order */
    for (int i = 1; i < ng; i++) {
        Island t = s->isl[i];
        int j = i - 1;
        while (j >= 0 && cmp_island(&s->isl[j], &t) > 0) s->isl[j + 1] = s->isl[j], j--;
        s->isl[j + 1] = t;
    }
    return 0;
}

/* A triangle list of one source. */
typedef struct {
    Source *s;
    int *t;
    int n;
} Tris;

static void tris_free(Tris *t) {
    free(t->t);
    t->t = NULL, t->n = 0;
}

/* Islands above z (zmin > z) or not above (zmin <= z: `below`). */
static int island_tris(Ctx *c, Source *s, int above, double z, Tris *out, int line) {
    if (islands(c, s, line)) return -1;
    out->s = s;
    out->n = 0;
    out->t = malloc(sizeof(int) * (s->ch.ni / 3 + 1));
    if (!out->t) return ferr(c, line, "out of memory");
    for (int i = 0; i < s->nisl; i++)
        if ((s->isl[i].zmin > z) == above)
            for (int k = 0; k < s->isl[i].n; k++) out->t[out->n++] = s->isl[i].tris[k];
    return 0;
}

static double vdist(const uint8_t *a, const uint8_t *b) {
    double dx = (double)rdf(a) - rdf(b), dy = (double)rdf(a + 4) - rdf(b + 4), dz = (double)rdf(a + 8) - rdf(b + 8);
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* core.split_headgear: the head islands of a (above z) farther than tol (mean distance of their
 * vertices to the nearest vertex of b's head) from b's head. */
static int headgear(Ctx *c, Source *a, Source *b, double z, double tol, Tris *out, int line) {
    Tris bh = {0};
    if (island_tris(c, b, 1, z, &bh, line) || islands(c, a, line)) {
        tris_free(&bh);
        return -1;
    }
    out->s = a;
    out->n = 0;
    out->t = malloc(sizeof(int) * (a->ch.ni / 3 + 1));
    uint8_t *seen = calloc(a->ch.nv + 1, 1);
    if (!out->t || !seen) {
        tris_free(&bh), free(seen);
        return ferr(c, line, "out of memory");
    }
    for (int i = 0; i < a->nisl; i++) {
        const Island *is = &a->isl[i];
        if (!(is->zmin > z)) continue;
        memset(seen, 0, a->ch.nv);
        for (int k = 0; k < is->n; k++)
            for (int j = 0; j < 3; j++) seen[vidx(a, 3 * is->tris[k] + j)] = 1;
        double sum = 0;
        int nvs = 0;
        for (uint32_t v = 0; v < a->ch.nv; v++) {
            if (!seen[v]) continue;
            double best = INFINITY;
            for (int k = 0; k < bh.n; k++)
                for (int j = 0; j < 3; j++) {
                    double d = vdist(vtx(a, v), vtx(b, vidx(b, 3 * bh.t[k] + j)));
                    if (d < best) best = d;
                }
            sum += best;
            nvs++;
        }
        double mean = nvs && bh.n ? sum / nvs : INFINITY;
        if (mean > tol)
            for (int k = 0; k < is->n; k++) out->t[out->n++] = is->tris[k];
    }
    tris_free(&bh);
    free(seen);
    return 0;
}

/* ---- open edges (core.boundary_edges) ---- */

typedef struct {
    double p[6];
} Edge;

static int cmp_edge(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    for (int i = 0; i < 6; i++)
        if (x->p[i] != y->p[i]) return x->p[i] < y->p[i] ? -1 : 1;
    return 0;
}

static int cmp3(const double *a, const double *b) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* The open edges (used once) of the triangle lists (sorted, unique). */
static Edge *open_edges(const Tris *lists, int nl, int *n) {
    int total = 0;
    for (int l = 0; l < nl; l++) total += lists[l].n * 3;
    Edge *e = malloc(sizeof *e * (size_t)(total ? total : 1));
    if (!e) return NULL;
    int k = 0;
    for (int l = 0; l < nl; l++)
        for (int i = 0; i < lists[l].n; i++) {
            const Source *s = lists[l].s;
            for (int j = 0; j < 3; j++) {
                double a[3], b[3];
                const uint8_t *pa = vtx(s, vidx(s, 3 * lists[l].t[i] + j)), *pb = vtx(s, vidx(s, 3 * lists[l].t[i] + (j + 1) % 3));
                for (int q = 0; q < 3; q++) a[q] = py_round(rdf(pa + 4 * q), 4), b[q] = py_round(rdf(pb + 4 * q), 4);
                const double *lo = cmp3(a, b) <= 0 ? a : b, *hi = lo == a ? b : a;
                memcpy(e[k].p, lo, 24);
                memcpy(e[k].p + 3, hi, 24);
                k++;
            }
        }
    qsort(e, (size_t)k, sizeof *e, cmp_edge);
    int m = 0;
    for (int i = 0; i < k;) {
        int j = i;
        while (j < k && !cmp_edge(&e[i], &e[j])) j++;
        if (j - i == 1) e[m++] = e[i];
        i = j;
    }
    *n = m;
    return e;
}

/* a minus b (both sorted unique), in place; the count left. */
static int edge_minus(Edge *a, int na, const Edge *b, int nb) {
    int m = 0, j = 0;
    for (int i = 0; i < na; i++) {
        while (j < nb && cmp_edge(&b[j], &a[i]) < 0) j++;
        if (j < nb && !cmp_edge(&b[j], &a[i])) continue;
        a[m++] = a[i];
    }
    return m;
}

/* ---- meshes ---- */

typedef struct {
    /* character layout (head swap) */
    uint8_t *s0, *s1;
    int nv;
    /* object layout */
    double (*pos)[3], (*nrm)[3], (*uv)[2];
    int no;
    uint16_t *idx;
    int ni, cap_v, cap_i;
} Mesh;

static void mesh_free(Mesh *m) {
    free(m->s0), free(m->s1), free(m->pos), free(m->nrm), free(m->uv), free(m->idx);
    memset(m, 0, sizeof *m);
}

static int head_swap(Ctx *c, Source *a, Source *b, double z, int need_closed, Mesh *m, int line) {
    Tris body = {0}, ahead = {0}, bhead = {0};
    if (island_tris(c, a, 0, z, &body, line) || island_tris(c, a, 1, z, &ahead, line) || island_tris(c, b, 1, z, &bhead, line)) {
        tris_free(&body), tris_free(&ahead), tris_free(&bhead);
        return -1;
    }
    int nb = 0, nc = 0, nh = 0;
    Tris both[2] = {body, ahead};
    Edge *eb = open_edges(&body, 1, &nb), *ec = open_edges(both, 2, &nc), *eh = open_edges(&bhead, 1, &nh);
    int opened = eb && ec ? edge_minus(eb, nb, ec, nc) : 0;
    int open = eb && eh ? edge_minus(eb, opened, eh, nh) : 0;
    int ok = eb && ec && eh;
    free(eb), free(ec), free(eh);
    if (!ok) {
        tris_free(&body), tris_free(&ahead), tris_free(&bhead);
        return ferr(c, line, "out of memory");
    }
    if (need_closed && open) {
        tris_free(&body), tris_free(&ahead), tris_free(&bhead);
        return ferr(c, line, "head_swap: %s's head does not close %s's neck seam (%d open edges)", b->model, a->model, open);
    }
    int *ra = malloc(sizeof(int) * a->ch.nv), *rb = malloc(sizeof(int) * b->ch.nv);
    const int ni = (body.n + bhead.n) * 3;
    m->idx = malloc(sizeof(uint16_t) * (size_t)(ni ? ni : 1));
    m->s0 = malloc((size_t)(a->ch.nv + b->ch.nv) * DISC_STRIDE0);
    m->s1 = malloc((size_t)(a->ch.nv + b->ch.nv) * DISC_STRIDE1);
    if (!ra || !rb || !m->idx || !m->s0 || !m->s1) {
        free(ra), free(rb), tris_free(&body), tris_free(&ahead), tris_free(&bhead);
        return ferr(c, line, "out of memory");
    }
    memset(ra, -1, sizeof(int) * a->ch.nv);
    memset(rb, -1, sizeof(int) * b->ch.nv);
    const Tris *parts[2] = {&body, &bhead};
    int *maps[2] = {ra, rb};
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < parts[p]->n; i++)
            for (int j = 0; j < 3; j++) {
                const Source *s = parts[p]->s;
                uint32_t v = vidx(s, 3 * parts[p]->t[i] + j);
                if (maps[p][v] < 0) {
                    maps[p][v] = m->nv;
                    memcpy(m->s0 + (size_t)m->nv * DISC_STRIDE0, s->ch.s0 + (size_t)v * DISC_STRIDE0, DISC_STRIDE0);
                    memcpy(m->s1 + (size_t)m->nv * DISC_STRIDE1, s->ch.s1 + (size_t)v * DISC_STRIDE1, DISC_STRIDE1);
                    m->nv++;
                }
                m->idx[m->ni++] = (uint16_t)maps[p][v];
            }
    free(ra), free(rb), tris_free(&body), tris_free(&ahead), tris_free(&bhead);
    if (m->nv > MESHPACK_MAX_VERTS) return ferr(c, line, "head_swap: %d vertices (at most %d)", m->nv, MESHPACK_MAX_VERTS);
    return 0;
}

static int mesh_push(Mesh *m, const double p[3], const double n[3], const double uv[2]) {
    if (m->no == m->cap_v) {
        int cap = m->cap_v ? m->cap_v * 2 : 64;
        void *a = realloc(m->pos, sizeof *m->pos * (size_t)cap), *b = a ? realloc(m->nrm, sizeof *m->nrm * (size_t)cap) : NULL;
        if (a) m->pos = a;
        if (b) m->nrm = b;
        void *u = b ? realloc(m->uv, sizeof *m->uv * (size_t)cap) : NULL;
        if (!u) return -1;
        m->uv = u;
        m->cap_v = cap;
    }
    memcpy(m->pos[m->no], p, 24), memcpy(m->nrm[m->no], n, 24), memcpy(m->uv[m->no], uv, 16);
    return m->no++;
}

static int idx_push(Mesh *m, int v) {
    if (m->ni == m->cap_i) {
        int cap = m->cap_i ? m->cap_i * 2 : 192;
        uint16_t *ni = realloc(m->idx, sizeof *ni * (size_t)cap);
        if (!ni) return -1;
        m->idx = ni, m->cap_i = cap;
    }
    m->idx[m->ni++] = (uint16_t)v;
    return 0;
}

static int sext(uint32_t v, int bits) {
    v &= (1u << bits) - 1;
    return v & (1u << (bits - 1)) ? (int)v - (1 << bits) : (int)v;
}

/* [p, w] times a D3D row-vector matrix (core.row_transform). */
static void row_transform(const float *m, const double p[3], double w, double out[3]) {
    for (int k = 0; k < 3; k++) out[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + w * m[12 + k];
}

static void normalize(double v[3]) {
    double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n == 0) n = 1.0;
    for (int k = 0; k < 3; k++) v[k] /= n;
}

/* core.rigid_object: the triangles in the frame of matrix m (positions and normals), UVs kept. */
static int rigid(Ctx *c, const Tris *t, const float *m, Mesh *out, int line) {
    const Source *s = t->s;
    int *remap = malloc(sizeof(int) * s->ch.nv);
    if (!remap) return ferr(c, line, "out of memory");
    memset(remap, -1, sizeof(int) * s->ch.nv);
    for (int i = 0; i < t->n; i++)
        for (int j = 0; j < 3; j++) {
            uint32_t v = vidx(s, 3 * t->t[i] + j);
            if (remap[v] < 0) {
                const uint8_t *p = vtx(s, v);
                const double pos[3] = {rdf(p), rdf(p + 4), rdf(p + 8)};
                const uint32_t pn = rd32(p + 12);
                const double nrm[3] = {sext(pn, 11) / 1023.0, sext(pn >> 11, 11) / 1023.0, sext(pn >> 22, 10) / 511.0};
                const double uv[2] = {rdf(p + 16), rdf(p + 20)};
                double tp[3], tn[3];
                row_transform(m, pos, 1.0, tp);
                row_transform(m, nrm, 0.0, tn);
                normalize(tn);
                remap[v] = mesh_push(out, tp, tn, uv);
                if (remap[v] < 0 || remap[v] > MESHPACK_MAX_VERTS - 1) {
                    free(remap);
                    return ferr(c, line, "too many vertices");
                }
            }
            if (idx_push(out, remap[v])) {
                free(remap);
                return ferr(c, line, "out of memory");
            }
        }
    free(remap);
    return 0;
}

/* ---- the shadow-box frame (cleon_necklace.frame_mesh) ---- */

typedef struct {
    double u0, v0, u1, v1;
    double lo[3], hi[3];
    int board;   /* the velvet board's front face uses board_uv */
    double hw, board_w, board_h;
    const double *board_uv;
} BoxUv;

static void wood_uv(const BoxUv *b, const double n[3], const double p[3], double out[2]) {
    double size[3];
    for (int k = 0; k < 3; k++) size[k] = b->hi[k] - b->lo[k];
    int longest = 0;
    for (int k = 1; k < 3; k++)
        if (size[k] > size[longest]) longest = k;
    int axes[2], na = 0;
    for (int k = 0; k < 3; k++)
        if (n[k] == 0) axes[na++] = k;
    int a = longest == axes[0] || longest == axes[1] ? longest : axes[0];
    int bb = axes[0] == a ? axes[1] : axes[0];
    double u = (p[a] - b->lo[a]) / 0.45, v = (p[bb] - b->lo[bb]) / 0.05;
    if (u > 1.0) u = 1.0;
    if (v > 1.0) v = 1.0;
    out[0] = b->u0 + (b->u1 - b->u0) * u;
    out[1] = b->v0 + (b->v1 - b->v0) * v;
}

static void box_uv(const BoxUv *b, const double n[3], const double p[3], double out[2]) {
    if (b->board && n[1] == 1) {
        const double *q = b->board_uv;
        out[0] = q[0] + (q[2] - q[0]) * (b->hw - p[0]) / b->board_w;
        out[1] = q[1] + (q[3] - q[1]) * (b->hi[2] - p[2]) / b->board_h;
        return;
    }
    wood_uv(b, n, p, out);
}

static int box(Mesh *m, const BoxUv *b) {
    const double x0 = b->lo[0], y0 = b->lo[1], z0 = b->lo[2], x1 = b->hi[0], y1 = b->hi[1], z1 = b->hi[2];
    const double faces[6][3] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    const double corners[6][4][3] = {
        {{x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}}, {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},
        {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}, {{x0, y1, z0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}},
        {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}, {{x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, {x0, y0, z0}},
    };
    for (int f = 0; f < 6; f++) {
        int base = m->no;
        for (int k = 0; k < 4; k++) {
            double uv[2];
            box_uv(b, faces[f], corners[f][k], uv);
            if (mesh_push(m, corners[f][k], faces[f], uv) < 0) return -1;
        }
        const int q[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++)
            if (idx_push(m, base + q[k])) return -1;
    }
    return 0;
}

static int shadow_box(Mesh *m, double board_w, double board_h, double bar, double depth, double board_depth, const double board_uv[4],
                      const double wood[4]) {
    const double hw = board_w / 2, W = hw + bar, H = board_h + 2 * bar;
    BoxUv b = {wood[0], wood[1], wood[2], wood[3], {-hw, 0, bar}, {hw, board_depth, bar + board_h}, 1, hw, board_w, board_h, board_uv};
    if (box(m, &b)) return -1;
    const double bars[4][2][3] = {{{-W, 0, 0}, {W, depth, bar}}, {{-W, 0, H - bar}, {W, depth, H}},
                                  {{-W, 0, bar}, {-hw, depth, H - bar}}, {{hw, 0, bar}, {W, depth, H - bar}}};
    for (int i = 0; i < 4; i++) {
        BoxUv w = {wood[0], wood[1], wood[2], wood[3], {0}, {0}, 0, 0, 0, 0, NULL};
        memcpy(w.lo, bars[i][0], 24), memcpy(w.hi, bars[i][1], 24);
        if (box(m, &w)) return -1;
    }
    return 0;
}

/* ---- textures ---- */

static Tex *tex_find(Ctx *c, const char *name) {
    for (int i = 0; i < c->ntex; i++)
        if (!strcmp(c->tex[i].name, name)) return &c->tex[i];
    return NULL;
}

/* An image by reference: "SRC.skinN" or a [texture] name. *w, *h; the texels (not owned). */
static const uint8_t *image_ref(Ctx *c, const char *ref, int *w, int *h, uint64_t *hash, int line) {
    char name[64];
    int n;
    if (hash) *hash = 0;
    if (skin_ref(ref, name, &n)) {
        Source *s = source(c, name, line);
        Skin *k = s ? skin(c, s, n, line) : NULL;
        if (!k) return NULL;
        *w = k->w, *h = k->h;
        if (hash) *hash = k->hash;
        return k->rgba;
    }
    Tex *t = tex_find(c, ref);
    if (!t && !strncmp(ref, "own/", 4)) {   /* the mod's own PNG / DDS, or the texture inside a .glb */
        if (c->ntex == MAX_TEXTURES) return ferr(c, line, "more than %d textures", MAX_TEXTURES), NULL;
        size_t n, in = 0;
        uint8_t *b = own_read(c, ref, &n, line), *img = b;
        char why[300];
        if (!b) return NULL;
        if (ext_is(ref, ".glb") && !(img = ma_glb_image(b, n, &in, why, sizeof why))) {
            free(b);
            return ferr(c, line, "%s: %s", ref, why), NULL;
        }
        Tex *nt = &c->tex[c->ntex];
        memset(nt, 0, sizeof *nt);
        snprintf(nt->name, sizeof nt->name, "%s", ref);
        int r = ma_image_load(img, img == b ? n : in, &nt->w, &nt->h, &nt->rgba, why, sizeof why);
        if (img != b) free(img);
        free(b);
        if (r) return ferr(c, line, "%s: %s", ref, why), NULL;
        t = &c->tex[c->ntex++];
    }
    if (!t || !t->rgba) return ferr(c, line, "no texture %s (SOURCE.skinN, own/FILE or an earlier [texture])", ref), NULL;
    *w = t->w, *h = t->h;
    return t->rgba;
}

/* core.raster_uv, one triangle (u, v): the texels whose centres lie inside or on an edge, wrapped
 * like REPEAT (the triangle moved by whole units into 0..1), marked in cov. */
static void raster_tri(const double u[3], const double v[3], int w, int h, uint8_t *cov) {
    const double fu = floor(fmin(fmin(u[0], u[1]), u[2])), fv = floor(fmin(fmin(v[0], v[1]), v[2]));
    double px[3], py[3];
    for (int j = 0; j < 3; j++) px[j] = (u[j] - fu) * w - 0.5, py[j] = (v[j] - fv) * h - 0.5;
    int x0 = (int)floor(fmin(fmin(px[0], px[1]), px[2])), x1 = (int)ceil(fmax(fmax(px[0], px[1]), px[2]));
    int y0 = (int)floor(fmin(fmin(py[0], py[1]), py[2])), y1 = (int)ceil(fmax(fmax(py[0], py[1]), py[2]));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    const double ax = px[0], ay = py[0], bx = px[1], by = py[1], cx = px[2], cy = py[2];
    const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabs(area) < 1e-12) return;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const double w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
            const double w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
            const double w2 = 1 - w0 - w1;
            if (fmin(fmin(w0, w1), w2) >= -1e-6) cov[y * w + x] = 1;
        }
}

static void dilate_cov(uint8_t *cov, int w, int h, int dilate) {
    uint8_t *prev = malloc((size_t)w * h);
    for (int d = 0; prev && d < dilate; d++) {
        memcpy(prev, cov, (size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (prev[y * w + x]) {
                    if (x + 1 < w) cov[y * w + x + 1] = 1;
                    if (x > 0) cov[y * w + x - 1] = 1;
                    if (y + 1 < h) cov[(y + 1) * w + x] = 1;
                    if (y > 0) cov[(y - 1) * w + x] = 1;
                }
    }
    free(prev);
}

/* core.raster_uv: texel coverage of a source's triangles in UV, grown by dilate. */
static void raster(const Tris *t, int w, int h, int dilate, uint8_t *cov) {
    const Source *s = t->s;
    memset(cov, 0, (size_t)w * h);
    for (int i = 0; i < t->n; i++) {
        double u[3], v[3];
        for (int j = 0; j < 3; j++) {
            const uint8_t *p = vtx(s, vidx(s, 3 * t->t[i] + j));
            u[j] = rdf(p + 16), v[j] = rdf(p + 20);
        }
        raster_tri(u, v, w, h, cov);
    }
    dilate_cov(cov, w, h, dilate);
}

/* ---- welded pieces (dirty_fights.components / find_glasses) ---- */

typedef struct {
    double p[3];
    int v;
} PosKey;

static int cmp_pos(const void *a, const void *b) {
    const PosKey *x = a, *y = b;
    int k = cmp3(x->p, y->p);
    return k ? k : x->v - y->v;
}

typedef struct {
    int root, first, n;
} Piece;

static int cmp_piece(const void *a, const void *b) {   /* larger first; equal sizes by first triangle (a stable sort) */
    const Piece *x = a, *y = b;
    if (x->n != y->n) return y->n - x->n;
    return x->first - y->first;
}

/* The largest piece apart from the body (welded connectivity: vertices at the same position, to
 * 0.1 mm, are one) with at least min_tris triangles, at least min_width across (x or y) and lying
 * within top of the character's highest vertex; out->n = 0 when there is none. */
static int largest_apart(Ctx *c, Source *s, int min_tris, double min_width, double top, Tris *out, int line) {
    const uint32_t nv = s->ch.nv, nt = s->ch.ni / 3;
    PosKey *k = malloc(sizeof *k * (nv ? nv : 1));
    int *key = malloc(sizeof(int) * (nv ? nv : 1)), *par = malloc(sizeof(int) * (nv ? nv : 1)), *tg = malloc(sizeof(int) * (nt ? nt : 1));
    Piece *pc = malloc(sizeof *pc * (nt ? nt : 1));
    int *slot = malloc(sizeof(int) * (nv ? nv : 1));
    out->s = s, out->n = 0;
    out->t = malloc(sizeof(int) * (nt + 1));
    if (!k || !key || !par || !tg || !pc || !slot || !out->t) {
        free(k), free(key), free(par), free(tg), free(pc), free(slot);
        return ferr(c, line, "out of memory");
    }
    double zmax = -INFINITY;
    for (uint32_t v = 0; v < nv; v++) {
        const uint8_t *p = vtx(s, v);
        for (int q = 0; q < 3; q++) k[v].p[q] = py_round(rdf(p + 4 * q), 4);
        k[v].v = (int)v;
        if (rdf(p + 8) > zmax) zmax = rdf(p + 8);
    }
    qsort(k, nv, sizeof *k, cmp_pos);
    int head = 0;
    for (uint32_t i = 0; i < nv; i++) {   /* rep.setdefault: the first vertex at that position */
        if (!i || cmp3(k[i].p, k[i - 1].p)) head = k[i].v;
        key[k[i].v] = head;
    }
    for (uint32_t v = 0; v < nv; v++) par[v] = (int)v, slot[v] = -1;
    for (uint32_t t = 0; t < nt; t++) {
        const int r = find_root(par, key[vidx(s, 3 * t)]);
        for (int j = 1; j < 3; j++) par[find_root(par, key[vidx(s, 3 * t + j)])] = r;
    }
    int np = 0;
    for (uint32_t t = 0; t < nt; t++) {
        const int r = find_root(par, key[vidx(s, 3 * t)]);
        if (slot[r] < 0) slot[r] = np, pc[np].root = r, pc[np].first = (int)t, pc[np].n = 0, np++;
        pc[slot[r]].n++;
        tg[t] = r;
    }
    qsort(pc, (size_t)np, sizeof *pc, cmp_piece);
    for (int i = 1; i < np && !out->n; i++) {
        if (pc[i].n < min_tris) continue;
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (uint32_t t = 0; t < nt; t++) {
            if (tg[t] != pc[i].root) continue;
            for (int j = 0; j < 3; j++) {
                const uint8_t *p = vtx(s, vidx(s, 3 * (int)t + j));
                for (int q = 0; q < 3; q++) {
                    const double x = rdf(p + 4 * q);
                    if (x < lo[q]) lo[q] = x;
                    if (x > hi[q]) hi[q] = x;
                }
            }
        }
        if (fmax(hi[0] - lo[0], hi[1] - lo[1]) >= min_width && lo[2] > zmax - top)
            for (uint32_t t = 0; t < nt; t++)
                if (tg[t] == pc[i].root) out->t[out->n++] = (int)t;
    }
    free(k), free(key), free(par), free(tg), free(pc), free(slot);
    return 0;
}

/* ---- broken glasses (dirty_fights.lying / lens_triangles / own_colours / broken_glasses / crack_texture) ---- */

/* A prop's vertices (position, normal, uv0) and triangle-list indices as an object mesh (find_props). */
static int prop_mesh(Ctx *c, const Source *s, Mesh *m, int line) {
    const DiscProp *p = &s->pr;
    for (uint32_t v = 0; v < p->nv; v++) {
        const uint8_t *b = p->vb + (size_t)v * p->stride;
        double pos[3] = {rdf(b), rdf(b + 4), rdf(b + 8)}, n[3], uv[2] = {rdf(b + p->uv0), rdf(b + p->uv0 + 4)};
        if (p->normal >= 0) {
            for (int q = 0; q < 3; q++) n[q] = rdf(b + p->normal + 4 * q);
        } else {
            const uint32_t u = rd32(b + p->normal_packed);
            n[0] = sext(u, 11) / 1023.0, n[1] = sext(u >> 11, 11) / 1023.0, n[2] = sext(u >> 22, 10) / 511.0;
        }
        if (mesh_push(m, pos, n, uv) < 0) return ferr(c, line, "out of memory");
    }
    if (m->no > MESHPACK_MAX_VERTS) return ferr(c, line, "prop %s: %d vertices (at most %d)", s->model, m->no, MESHPACK_MAX_VERTS);
    for (int i = 0; i < s->pni; i++)
        if (idx_push(m, s->pidx[i])) return ferr(c, line, "out of memory");
    return 0;
}

/* lying: the glasses on the ground: widest extent along x, thinnest along z, the other along y
 * (a proper rotation), centred on x = y = 0, resting 3 mm above z = 0. pos/nrm of m rewritten into
 * out_pos/out_nrm (n vertices). */
static void lying(const Mesh *m, double (*op)[3], double (*on)[3]) {
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int v = 0; v < m->no; v++)
        for (int k = 0; k < 3; k++) {
            if (m->pos[v][k] < lo[k]) lo[k] = m->pos[v][k];
            if (m->pos[v][k] > hi[k]) hi[k] = m->pos[v][k];
        }
    double ext[3];
    int order[3] = {0, 1, 2};
    for (int k = 0; k < 3; k++) ext[k] = hi[k] - lo[k];
    for (int i = 1; i < 3; i++)   /* sorted(range(3), key=ext): stable */
        for (int j = i; j > 0 && ext[order[j - 1]] > ext[order[j]]; j--) {
            int t = order[j];
            order[j] = order[j - 1], order[j - 1] = t;
        }
    const int ax = order[2], ay = order[1], az = order[0];
    const double sign = (ax == 0 && ay == 1) || (ax == 1 && ay == 2) || (ax == 2 && ay == 0) ? 1 : -1;
    double cen[3];
    for (int k = 0; k < 3; k++) cen[k] = (lo[k] + hi[k]) / 2;
    double zmin = INFINITY;
    for (int v = 0; v < m->no; v++) {
        double q[3], n[3];
        for (int k = 0; k < 3; k++) q[k] = m->pos[v][k] - cen[k], n[k] = m->nrm[v][k] - 0.0;
        op[v][0] = q[ax], op[v][1] = q[ay] * sign, op[v][2] = q[az];
        on[v][0] = n[ax], on[v][1] = n[ay] * sign, on[v][2] = n[az];
        normalize(on[v]);
        if (op[v][2] < zmin) zmin = op[v][2];
    }
    for (int v = 0; v < m->no; v++) op[v][2] = op[v][2] - zmin + 0.003;
}

/* lens_triangles: the triangles whose normal lies within `deg` degrees of the axis (y or z) most of
 * the surface faces. lens[t] = 1 for those; returns that axis. */
static int lens_tris(double (*pos)[3], const uint16_t *idx, int ni, double deg, uint8_t *lens) {
    const int nt = ni / 3;
    double wt[3] = {0, 0, 0};
    double (*nn)[4] = malloc(sizeof *nn * (size_t)(nt ? nt : 1));
    if (!nn) return -1;
    for (int t = 0; t < nt; t++) {
        const double *a = pos[idx[3 * t]], *b = pos[idx[3 * t + 1]], *d = pos[idx[3 * t + 2]];
        const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
        nn[t][0] = e1[1] * e2[2] - e1[2] * e2[1];
        nn[t][1] = e1[2] * e2[0] - e1[0] * e2[2];
        nn[t][2] = e1[0] * e2[1] - e1[1] * e2[0];
        nn[t][3] = sqrt(nn[t][0] * nn[t][0] + nn[t][1] * nn[t][1] + nn[t][2] * nn[t][2]);
        for (int k = 1; k < 3; k++) wt[k] += fabs(nn[t][k]);
    }
    const int front = wt[1] >= wt[2] ? 1 : 2;
    const double lim = cos(deg * (M_PI / 180.0));
    for (int t = 0; t < nt; t++) lens[t] = nn[t][3] > 1e-12 && fabs(nn[t][front]) / nn[t][3] > lim;
    free(nn);
    return front;
}

/* own_colours: the glasses drawn with a texture made here: the lens triangles mapped flat onto the
 * left half (across the pair and up it), every other vertex onto one texel of the frame colour on
 * the right half; *rgba gets size x size texels. */
static int paint(Mesh *m, double deg, const int frame[3], const int lensc[3], int size, uint8_t **rgba) {
    double (*lp)[3] = malloc(sizeof *lp * (size_t)m->no), (*ln)[3] = malloc(sizeof *ln * (size_t)m->no);
    uint8_t *lens = malloc((size_t)m->ni / 3 + 1), *lv = calloc((size_t)m->no, 1);
    *rgba = malloc((size_t)size * size * 4);
    if (!lp || !ln || !lens || !lv || !*rgba) {
        free(lp), free(ln), free(lens), free(lv), free(*rgba);
        *rgba = NULL;
        return -1;
    }
    lying(m, lp, ln);
    const int front = lens_tris(lp, m->idx, m->ni, deg, lens), up = front == 1 ? 2 : 1;
    for (int t = 0; front > 0 && t < m->ni / 3; t++)
        if (lens[t])
            for (int j = 0; j < 3; j++) lv[m->idx[3 * t + j]] = 1;
    double x0 = INFINITY, x1 = -INFINITY, u0 = INFINITY, u1 = -INFINITY;
    int any = 0;
    for (int v = 0; v < m->no; v++) {
        if (!lv[v]) continue;
        any = 1;
        x0 = fmin(x0, lp[v][0]), x1 = fmax(x1, lp[v][0]), u0 = fmin(u0, lp[v][up]), u1 = fmax(u1, lp[v][up]);
    }
    if (!any) x0 = u0 = 0.0, x1 = u1 = 1.0;
    const double dx = x1 - x0 != 0 ? x1 - x0 : 1.0, du = u1 - u0 != 0 ? u1 - u0 : 1.0;
    for (int v = 0; v < m->no; v++) {
        if (lv[v]) {
            m->uv[v][0] = 0.02 + 0.46 * (lp[v][0] - x0) / dx;
            m->uv[v][1] = 0.05 + 0.9 * (lp[v][up] - u0) / du;
        } else {
            m->uv[v][0] = 0.75, m->uv[v][1] = 0.5;
        }
    }
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const int *col = x < size / 2 ? lensc : frame;
            uint8_t *o = *rgba + ((size_t)y * size + x) * 4;
            o[0] = (uint8_t)col[0], o[1] = (uint8_t)col[1], o[2] = (uint8_t)col[2], o[3] = 255;
        }
    free(lp), free(ln), free(lens), free(lv);
    return 0;
}

typedef struct {
    double turn, gap, lens;
    int shards, seed;
} Break;

/* broken_glasses: m lying on the ground, snapped at the bridge (the half at x > 0 turned and moved
 * away), that half's lens triangles left out and shards of glass (small flat triangles with that
 * lens's UVs) scattered around. *kept / *nkept: the UV triangles of the kept lens (to crack). */
static int break_glasses(const Mesh *m, const Break *bk, Mesh *out, double (**kept)[3][2], int *nkept) {
    const int nt = m->ni / 3;
    double (*lp)[3] = malloc(sizeof *lp * (size_t)m->no), (*ln)[3] = malloc(sizeof *ln * (size_t)m->no);
    uint8_t *lens = malloc((size_t)nt + 1);
    int *remap = malloc(sizeof(int) * 2 * (size_t)m->no);
    double (*kp)[3][2] = malloc(sizeof *kp * (size_t)(nt + 1)), (*gn)[3][2] = malloc(sizeof *gn * (size_t)(nt + 1));
    int nk = 0, ng = 0, rc = -1;
    if (!lp || !ln || !lens || !remap || !kp || !gn) goto done;
    lying(m, lp, ln);
    if (lens_tris(lp, m->idx, m->ni, bk->lens, lens) < 0) goto done;
    memset(remap, -1, sizeof(int) * 2 * (size_t)m->no);
    const double t_ang = bk->turn * (M_PI / 180.0), ca = cos(t_ang), sa = sin(t_ang);
    for (int t = 0; t < nt; t++) {
        const int a = m->idx[3 * t], b = m->idx[3 * t + 1], d = m->idx[3 * t + 2];
        const int moved = (0.0 + lp[a][0] + lp[b][0] + lp[d][0]) / 3 > 0;
        if (lens[t]) {
            double (*dst)[2] = moved ? gn[ng++] : kp[nk++];
            for (int j = 0; j < 3; j++) dst[j][0] = m->uv[m->idx[3 * t + j]][0], dst[j][1] = m->uv[m->idx[3 * t + j]][1];
            if (moved) continue;
        }
        for (int j = 0; j < 3; j++) {
            const int v = m->idx[3 * t + j];
            int *r = &remap[2 * v + moved];
            if (*r < 0) {
                double p[3] = {lp[v][0], lp[v][1], lp[v][2]}, n[3] = {ln[v][0], ln[v][1], ln[v][2]};
                if (moved) {
                    const double x = p[0], y = p[1];
                    p[0] = x * ca - y * sa + bk->gap, p[1] = x * sa + y * ca;
                    n[0] = ln[v][0] * ca - ln[v][1] * sa, n[1] = ln[v][0] * sa + ln[v][1] * ca;
                    normalize(n);
                }
                *r = mesh_push(out, p, n, m->uv[v]);
                if (*r < 0) goto done;
            }
            if (idx_push(out, *r)) goto done;
        }
    }
    {
        PyRand rnd;
        pyrand_seed(&rnd, bk->seed);
        double (*src)[3][2] = ng ? gn : kp;
        const int nsrc = ng ? ng : nk;
        double xlo = INFINITY, xhi = -INFINITY, ylo = INFINITY, yhi = -INFINITY;
        for (int v = 0; v < out->no; v++)
            xlo = fmin(xlo, out->pos[v][0]), xhi = fmax(xhi, out->pos[v][0]), ylo = fmin(ylo, out->pos[v][1]), yhi = fmax(yhi, out->pos[v][1]);
        for (int k = 0; k < bk->shards; k++) {
            static const double zero[3][2];
            const double (*uv)[2] = nsrc ? (const double (*)[2])src[pyrand_randrange(&rnd, 0, nsrc, 1)] : zero;
            const double x = pyrand_uniform(&rnd, xlo - 0.03, xhi + 0.05), y = pyrand_uniform(&rnd, ylo - 0.05, yhi + 0.05);
            const double size = pyrand_uniform(&rnd, 0.008, 0.02), a0 = pyrand_uniform(&rnd, 0, 2 * M_PI);
            const int base = out->no;
            for (int j = 0; j < 3; j++) {
                const double a = a0 + j * 2 * M_PI / 3 + pyrand_uniform(&rnd, -0.4, 0.4);
                const double sy = sin(a) * size;
                const double p[3] = {x + cos(a) * size, y + sy * pyrand_uniform(&rnd, 0.5, 1.0), 0.002}, n[3] = {0.0, 0.0, 1.0};
                if (mesh_push(out, p, n, uv[j]) < 0) goto done;
            }
            for (int j = 0; j < 3; j++)
                if (idx_push(out, base + j)) goto done;
        }
    }
    rc = out->no > MESHPACK_MAX_VERTS ? -1 : 0;
done:
    free(lp), free(ln), free(lens), free(remap), free(gn);
    if (rc) free(kp), kp = NULL, nk = 0;
    *kept = kp, *nkept = nk;
    return rc;
}

/* crack_texture: light, thin lines out from the centre of the kept lens's texels, each bending a
 * little as it goes, drawn into rgba (w x h). */
static int crack(uint8_t *rgba, int w, int h, double (*tri)[3][2], int nt, int lines, int seed) {
    uint8_t *cov = calloc((size_t)w * h, 1);
    if (!cov) return -1;
    for (int t = 0; t < nt; t++) {
        const double u[3] = {tri[t][0][0], tri[t][1][0], tri[t][2][0]}, v[3] = {tri[t][0][1], tri[t][1][1], tri[t][2][1]};
        raster_tri(u, v, w, h, cov);
    }
    long n = 0;
    double sx = 0, sy = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (cov[y * w + x]) n++, sx += x, sy += y;
    if (!n) {
        free(cov);
        return 0;
    }
    PyRand rnd;
    pyrand_seed(&rnd, seed);
    const double cx = sx / n, cy = sy / n, reach = fmax(2.0, sqrt((double)n) * 0.9);
    for (int k = 0; k < lines; k++) {
        double ang = (k + pyrand_random(&rnd) * 0.6) * 2 * M_PI / lines, x = cx, y = cy;
        for (int step = 0; step < (int)(reach * 2); step++) {
            ang += (pyrand_random(&rnd) - 0.5) * 0.5;
            x += cos(ang) * 0.5;
            y += sin(ang) * 0.5;
            const double px = nearbyint(x), py = nearbyint(y);   /* round(): ties to even */
            if (px < 0 || py < 0 || px >= w || py >= h || !cov[(int)py * w + (int)px]) break;
            uint8_t *o = rgba + ((size_t)py * w + (size_t)px) * 4;
            for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)((o[ch] + 3 * 225) / 4 > 255 ? 255 : (o[ch] + 3 * 225) / 4);
            if (o[3] < 230) o[3] = 230;
        }
    }
    free(cov);
    return 0;
}

/* A selector: [SRC.]islands(above Z | below Z) or headgear(OTHER, above Z, tolerance D). */
static int selector(Ctx *c, const char *v, Source *dflt, Tris *out, int line) {
    char name[64] = "";
    const char *f = v, *dot = strchr(v, '.'), *paren = strchr(v, '(');
    if (dot && paren && dot < paren) {
        if ((size_t)(dot - v) >= sizeof name) return ferr(c, line, "bad selector %s", v);
        memcpy(name, v, (size_t)(dot - v));
        name[dot - v] = 0;
        f = dot + 1;
    }
    size_t L = strlen(f);
    if (!paren || f[L - 1] != ')') return ferr(c, line, "a selector is islands(...), headgear(...) or largest_apart(...), not %s", v);
    char args[160];
    size_t fn = (size_t)(strchr(f, '(') - f);
    snprintf(args, sizeof args, "%.*s", (int)(L - fn - 2), f + fn + 1);
    Source *s = name[0] ? source(c, name, line) : dflt;
    if (!s) return name[0] ? -1 : ferr(c, line, "%s: no source (from = ...)", v);
    if (!source(c, s->name, line)) return -1;
    if (s->is_prop) return ferr(c, line, "%s is a prop: select its triangles with take = all", s->model);
    if (s->is_own) return ferr(c, line, "%s is your own mesh: take = all", s->model);
    char cl[8][160], w[4][160];
    int nc = clauses(args, cl, 8);
    if (!strncmp(f, "largest_apart(", 14)) {
        int mt = -1;
        double mw = -1, top = -1;
        for (int k = 0; k < nc; k++) {
            if (words(cl[k], w, 4) != 2) mt = -2;
            else if (!strcmp(w[0], "min_tris") && inum(w[1], &mt) && mt >= 1) {}
            else if (!strcmp(w[0], "min_width") && num(w[1], &mw) && mw >= 0) {}
            else if (!strcmp(w[0], "top") && num(w[1], &top) && top > 0) {}
            else mt = -2;
        }
        if (nc != 3 || mt < 1 || mw < 0 || top <= 0) return ferr(c, line, "largest_apart(min_tris N, min_width W, top T), not %s", v);
        if (largest_apart(c, s, mt, mw, top, out, line)) return -1;
        if (!out->n) return ferr(c, line, "%s: no piece apart from the body with at least %d triangles, %g m wide, in the top %g m", s->model, mt, mw, top);
        return 0;
    }
    if (!strncmp(f, "islands(", 8)) {
        double z;
        if (nc != 1 || words(cl[0], w, 4) != 2 || (strcmp(w[0], "above") && strcmp(w[0], "below")) || !num(w[1], &z))
            return ferr(c, line, "islands(above Z) or islands(below Z), not %s", v);
        return island_tris(c, s, !strcmp(w[0], "above"), z, out, line);
    }
    if (!strncmp(f, "headgear(", 9)) {
        double z, tol;
        Source *b = nc == 3 ? source(c, cl[0], line) : NULL;
        if (nc == 3 && !b) return -1;
        if (!b || words(cl[1], w, 4) != 2 || strcmp(w[0], "above") || !num(w[1], &z) || words(cl[2], w, 4) != 2 || strcmp(w[0], "tolerance") ||
            !num(w[1], &tol))
            return ferr(c, line, "headgear(OTHER, above Z, tolerance D), not %s", v);
        return headgear(c, s, b, z, tol, out, line);
    }
    return ferr(c, line, "unknown selector %s", v);
}

static int tex_alloc(Tex *t, int w, int h) {
    uint8_t *p = malloc((size_t)w * h * 4);
    if (!p) return -1;
    free(t->rgba), free(t->mask);
    t->rgba = p, t->mask = NULL, t->w = w, t->h = h, t->nreg = 0;
    return 0;
}

/* cleon_necklace.wood: wood grain along x. */
static void wood_fill(uint8_t *out, int w, int h, int stride, const int col[3], int64_t seed) {
    PyRand r;
    pyrand_seed(&r, seed);
    double phase[3];
    for (int i = 0; i < 3; i++) phase[i] = pyrand_uniform(&r, 0, 6.3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double t = y * 0.55 + 1.2 * sin(x * 0.013 + phase[0]) + 0.5 * sin(x * 0.041 + phase[1]);
            double ring = 0.5 + 0.5 * sin(t);
            double k = 0.8 + 0.28 * pow(ring, 4) + pyrand_uniform(&r, -0.035, 0.035);
            uint8_t *o = out + ((size_t)y * stride + x) * 4;
            for (int ch = 0; ch < 3; ch++) {
                double v = col[ch] * k;
                int iv = (int)v;   /* int() truncates toward zero */
                o[ch] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
            }
            o[3] = 255;
        }
}

static int rgb_args(char w[][160], int n, int at, int col[3]) {
    if (at + 3 > n) return 0;
    for (int k = 0; k < 3; k++)
        if (!inum(w[at + k], &col[k]) || col[k] < 0 || col[k] > 255) return 0;
    return 1;
}

static const char *const tex_keys[] = {"base", "require", "paste", "crop", "mask", "polish", "fill", "scale", "canvas", "place", NULL};

static int tex_section(Ctx *c, const MaSection *sec) {
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        int k = 0;
        while (tex_keys[k] && strcmp(tex_keys[k], l->key)) k++;
        if (!tex_keys[k]) return ferr(c, l->line, "unknown key %s in [texture %s]", l->key, sec->name);
    }
    if (c->ntex == MAX_TEXTURES) return ferr(c, sec->line, "more than %d textures", MAX_TEXTURES);
    Tex *t = &c->tex[c->ntex++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", sec->name);
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        if (over_time(c, l->line)) return -1;
        char cl[8][160], w[16][160];
        int nc = clauses(l->value, cl, 8), nw = nc ? words(cl[0], w, 16) : 0;
        if (!strcmp(l->key, "base")) {
            int iw, ih;
            const uint8_t *p = image_ref(c, l->value, &iw, &ih, NULL, l->line);
            if (!p || tex_alloc(t, iw, ih)) return p ? ferr(c, l->line, "out of memory") : -1;
            memcpy(t->rgba, p, (size_t)iw * ih * 4);
        } else if (!t->rgba && strcmp(l->key, "canvas") && strcmp(l->key, "require")) {
            return ferr(c, l->line, "%s before base = or canvas =", l->key);
        } else if (!strcmp(l->key, "require")) {
            int rw, rh;
            if (nw == 2 && !strcmp(w[0], "same_size")) {
                if (!t->rgba) return ferr(c, l->line, "require before base =");
                if (!image_ref(c, w[1], &rw, &rh, NULL, l->line)) return -1;
                if (rw != t->w || rh != t->h) return ferr(c, l->line, "%s is %dx%d, %s is %dx%d", w[1], rw, rh, t->name, t->w, t->h);
            } else if (nw == 3 && !strcmp(w[0], "size") && inum(w[1], &rw) && inum(w[2], &rh)) {
                if (!t->rgba) return ferr(c, l->line, "require before base =");
                if (rw != t->w || rh != t->h) return ferr(c, l->line, "the texture is %dx%d, not %dx%d (another disc?)", t->w, t->h, rw, rh);
            } else {
                return ferr(c, l->line, "require = same_size TEXTURE or require = size W H");
            }
        } else if (!strcmp(l->key, "paste")) {
            /* paste = SRC.skinN, uv_of SELECTOR, dilate N, keep SELECTOR */
            char ww[4][160];
            int pw, ph, dil = 0;
            const char *uv_of = NULL, *keep = NULL;
            for (int k = 1; k < nc; k++) {
                int n2 = words(cl[k], ww, 4);
                if (n2 == 2 && !strcmp(ww[0], "uv_of")) uv_of = strstr(cl[k], ww[1]);
                else if (n2 == 2 && !strcmp(ww[0], "keep")) keep = strstr(cl[k], ww[1]);
                else if (n2 == 2 && !strcmp(ww[0], "dilate") && inum(ww[1], &dil) && dil >= 0 && dil <= 16) {}
                else return ferr(c, l->line, "paste = TEXTURE, uv_of SELECTOR, dilate N, keep SELECTOR");
            }
            if (!nc || !uv_of) return ferr(c, l->line, "paste = TEXTURE, uv_of SELECTOR, dilate N, keep SELECTOR");
            const uint8_t *src = image_ref(c, cl[0], &pw, &ph, NULL, l->line);
            if (!src) return -1;
            if (pw != t->w || ph != t->h) return ferr(c, l->line, "paste: %s is %dx%d, %s is %dx%d", cl[0], pw, ph, t->name, t->w, t->h);
            char sel1[160], sel2[160] = "";
            snprintf(sel1, sizeof sel1, "%s", uv_of);
            if (keep) snprintf(sel2, sizeof sel2, "%s", keep);
            Tris a = {0}, b = {0};
            uint8_t *cov = malloc((size_t)t->w * t->h), *kp = calloc((size_t)t->w * t->h, 1);
            if (!cov || !kp) {
                free(cov), free(kp);
                return ferr(c, l->line, "out of memory");
            }
            if (selector(c, sel1, NULL, &a, l->line) || (keep && selector(c, sel2, NULL, &b, l->line))) {
                free(cov), free(kp), tris_free(&a), tris_free(&b);
                return -1;
            }
            raster(&a, t->w, t->h, dil, cov);
            if (keep) raster(&b, t->w, t->h, 0, kp);
            for (int k = 0; k < t->w * t->h; k++)
                if (cov[k] && !kp[k]) memcpy(t->rgba + 4 * k, src + 4 * k, 4);
            free(cov), free(kp), tris_free(&a), tris_free(&b);
        } else if (!strcmp(l->key, "crop")) {
            int x, y, cw, ch;
            /* Texel (x, y) of the rectangle is texel y * width + x of the texture: a rectangle past
             * the right edge continues at the start of the next row (as the reference recipe reads
             * Cleon's 512-wide skin at x 430..517); it must end inside the texture. */
            if (nw != 4 || !inum(w[0], &x) || !inum(w[1], &y) || !inum(w[2], &cw) || !inum(w[3], &ch) || x < 0 || y < 0 || cw < 1 || ch < 1 ||
                x >= t->w || y + ch > t->h || (int64_t)(y + ch - 1) * t->w + x + cw > (int64_t)t->w * t->h)
                return ferr(c, l->line, "crop = X Y W H inside the %dx%d texture", t->w, t->h);
            uint8_t *p = malloc((size_t)cw * ch * 4);
            if (!p) return ferr(c, l->line, "out of memory");
            for (int yy = 0; yy < ch; yy++) memcpy(p + (size_t)yy * cw * 4, t->rgba + ((size_t)(y + yy) * t->w + x) * 4, (size_t)cw * 4);
            free(t->rgba), free(t->mask);
            t->rgba = p, t->mask = NULL, t->w = cw, t->h = ch;
        } else if (!strcmp(l->key, "mask")) {
            /* mask = colour(r-b <= A, sum >= B), parts >= N, inside, at_least M */
            int rb = 0, sum = 0, parts = 1, inside = 0, least = 0;
            char cw2[4][160];
            if (nc < 1 || sscanf(cl[0], "colour(r-b <= %d , sum >= %d )", &rb, &sum) != 2)
                return ferr(c, l->line, "mask = colour(r-b <= A, sum >= B)[, parts >= N][, inside][, at_least M]");
            for (int k = 1; k < nc; k++) {
                int n2 = words(cl[k], cw2, 4);
                if (n2 == 3 && !strcmp(cw2[0], "parts") && !strcmp(cw2[1], ">=") && inum(cw2[2], &parts)) {}
                else if (n2 == 1 && !strcmp(cw2[0], "inside")) inside = 1;
                else if (n2 == 2 && !strcmp(cw2[0], "at_least") && inum(cw2[1], &least)) {}
                else return ferr(c, l->line, "mask = colour(r-b <= A, sum >= B)[, parts >= N][, inside][, at_least M]");
            }
            const int W = t->w, H = t->h;
            uint8_t *raw = calloc((size_t)W * H, 1), *m = calloc((size_t)W * H, 1), *seen = calloc((size_t)W * H, 1);
            int *todo = malloc(sizeof(int) * (size_t)W * H), *part = malloc(sizeof(int) * (size_t)W * H);
            if (!raw || !m || !seen || !todo || !part) {
                free(raw), free(m), free(seen), free(todo), free(part);
                return ferr(c, l->line, "out of memory");
            }
            for (int k = 0; k < W * H; k++) {
                const uint8_t *p = t->rgba + 4 * k;
                raw[k] = p[0] - p[2] <= rb && p[0] + p[1] + p[2] >= sum;
            }
            int total = 0;
            for (int x = 0; x < W; x++)   /* parts in (x, y) order, 8-connected */
                for (int y = 0; y < H; y++) {
                    if (!raw[y * W + x] || seen[y * W + x]) continue;
                    int np = 0, nt = 0, border = 0;
                    todo[nt++] = y * W + x;
                    seen[y * W + x] = 1;
                    while (nt) {
                        int q = todo[--nt], qx = q % W, qy = q / W;
                        part[np++] = q;
                        border |= qx == 0 || qx == W - 1 || qy == 0 || qy == H - 1;
                        for (int dx = -1; dx <= 1; dx++)
                            for (int dy = -1; dy <= 1; dy++) {
                                int ax = qx + dx, ay = qy + dy;
                                if (ax < 0 || ay < 0 || ax >= W || ay >= H || !raw[ay * W + ax] || seen[ay * W + ax]) continue;
                                seen[ay * W + ax] = 1;
                                todo[nt++] = ay * W + ax;
                            }
                    }
                    if (np >= parts && !(inside && border)) {
                        for (int k = 0; k < np; k++) m[part[k]] = 1;
                        total += np;
                    }
                }
            free(raw), free(seen), free(todo), free(part);
            free(t->mask);
            t->mask = m;
            if (total < least) return ferr(c, l->line, "mask: only %d texels found, at least %d needed (another texture?)", total, least);
        } else if (!strcmp(l->key, "polish")) {
            /* polish = keep A, grey B, gain G, add D: masked texels (A * c + B * grey) * G + D */
            double kk = 0, gr = 0, gain = 1, add = 0;
            char cw2[4][160];
            for (int k = 0; k < nc; k++) {
                int n2 = words(cl[k], cw2, 4);
                double v;
                if (n2 != 2 || !num(cw2[1], &v)) return ferr(c, l->line, "polish = keep A, grey B, gain G, add D");
                if (!strcmp(cw2[0], "keep")) kk = v;
                else if (!strcmp(cw2[0], "grey")) gr = v;
                else if (!strcmp(cw2[0], "gain")) gain = v;
                else if (!strcmp(cw2[0], "add")) add = v;
                else return ferr(c, l->line, "polish = keep A, grey B, gain G, add D");
            }
            if (!t->mask) return ferr(c, l->line, "polish needs a mask = line before it");
            for (int k = 0; k < t->w * t->h; k++) {
                if (!t->mask[k]) continue;
                uint8_t *p = t->rgba + 4 * k;
                const double grey = (p[0] + p[1] + p[2]) / 3.0;
                for (int ch = 0; ch < 3; ch++) {
                    int iv = (int)((kk * p[ch] + gr * grey) * gain + add);
                    p[ch] = (uint8_t)(iv > 255 ? 255 : iv < 0 ? 0 : iv);
                }
                p[3] = 255;
            }
        } else if (!strcmp(l->key, "fill")) {
            /* fill = velvet R G B, noise N, seed S[, shadow]: the texels outside the mask */
            int col[3];
            double noise = 0;
            int seed = 0, shadow = 0;
            char cw2[4][160];
            if (nw != 4 || strcmp(w[0], "velvet") || !rgb_args(w, nw, 1, col)) return ferr(c, l->line, "fill = velvet R G B, noise N, seed S[, shadow]");
            for (int k = 1; k < nc; k++) {
                int n2 = words(cl[k], cw2, 4);
                if (n2 == 2 && !strcmp(cw2[0], "noise") && num(cw2[1], &noise)) {}
                else if (n2 == 2 && !strcmp(cw2[0], "seed") && inum(cw2[1], &seed)) {}
                else if (n2 == 1 && !strcmp(cw2[0], "shadow")) shadow = 1;
                else return ferr(c, l->line, "fill = velvet R G B, noise N, seed S[, shadow]");
            }
            PyRand r;
            pyrand_seed(&r, seed);
            const uint8_t *m = t->mask;
            const int W = t->w, H = t->h;
#define IN(xx, yy) ((xx) >= 0 && (yy) >= 0 && (xx) < W && (yy) < H && m && m[(yy) * W + (xx)])
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    if (IN(x, y)) continue;
                    double k = 1.0 + pyrand_uniform(&r, -noise, noise);
                    if (shadow) {
                        if (IN(x - 1, y - 1) || IN(x - 1, y - 2)) k *= 0.45;
                        else if (IN(x - 2, y - 2) || IN(x, y - 2)) k *= 0.7;
                    }
                    uint8_t *p = t->rgba + ((size_t)y * W + x) * 4;
                    for (int ch = 0; ch < 3; ch++) {
                        int iv = (int)(col[ch] * k);
                        p[ch] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
                    }
                    p[3] = 255;
                }
#undef IN
        } else if (!strcmp(l->key, "scale")) {
            int f;
            if (nw != 2 || !inum(w[0], &f) || f < 1 || f > 8 || strcmp(w[1], "bilinear")) return ferr(c, l->line, "scale = N bilinear");
            const int w0 = t->w, h0 = t->h, W = w0 * f, H = h0 * f;
            if (W > MODASSETS_TEX_MAX || H > MODASSETS_TEX_MAX) return ferr(c, l->line, "scale: %dx%d is over %d texels a side", W, H, MODASSETS_TEX_MAX);
            uint8_t *out = malloc((size_t)W * H * 4);
            if (!out) return ferr(c, l->line, "out of memory");
            const uint8_t *in = t->rgba;
            for (int Y = 0; Y < H; Y++) {
                double fy = fmin(fmax((Y + 0.5) / f - 0.5, 0.0), h0 - 1.0);
                int y0 = (int)fy, y1 = y0 + 1 < h0 - 1 ? y0 + 1 : h0 - 1;
                double ty = fy - y0;
                for (int X = 0; X < W; X++) {
                    double fx = fmin(fmax((X + 0.5) / f - 0.5, 0.0), w0 - 1.0);
                    int x0 = (int)fx, x1 = x0 + 1 < w0 - 1 ? x0 + 1 : w0 - 1;
                    double tx = fx - x0;
                    for (int ch = 0; ch < 4; ch++) {
                        double a = in[(y0 * w0 + x0) * 4 + ch] * (1 - tx) + in[(y0 * w0 + x1) * 4 + ch] * tx;
                        double b = in[(y1 * w0 + x0) * 4 + ch] * (1 - tx) + in[(y1 * w0 + x1) * 4 + ch] * tx;
                        out[((size_t)Y * W + X) * 4 + ch] = (uint8_t)(int)(a * (1 - ty) + b * ty + 0.5);
                    }
                }
            }
            free(t->rgba), free(t->mask);
            t->rgba = out, t->mask = NULL, t->w = W, t->h = H;
        } else if (!strcmp(l->key, "canvas")) {
            int cw, ch, col[3];
            char cw2[8][160];
            if (nw != 2 || !inum(w[0], &cw) || !inum(w[1], &ch) || cw < 1 || ch < 1 || cw > MODASSETS_TEX_MAX || ch > MODASSETS_TEX_MAX || nc != 2 ||
                words(cl[1], cw2, 8) != 4 || strcmp(cw2[0], "colour") || !rgb_args(cw2, 4, 1, col))
                return ferr(c, l->line, "canvas = W H, colour R G B (at most %d texels a side)", MODASSETS_TEX_MAX);
            if (tex_alloc(t, cw, ch)) return ferr(c, l->line, "out of memory");
            for (int k = 0; k < cw * ch; k++) t->rgba[4 * k] = (uint8_t)col[0], t->rgba[4 * k + 1] = (uint8_t)col[1], t->rgba[4 * k + 2] = (uint8_t)col[2], t->rgba[4 * k + 3] = 255;
        } else if (!strcmp(l->key, "place")) {
            /* place = TEXTURE, at X Y[, as NAME]  or  place = wood R G B seed S, at X Y W H[, as NAME] */
            int x = 0, y = 0, pw = 0, ph = 0, col[3], seed = 0, is_wood = nw == 6 && !strcmp(w[0], "wood");
            char as[64] = "", cw2[8][160];
            int have_at = 0;
            if (is_wood && (!rgb_args(w, nw, 1, col) || strcmp(w[4], "seed") || !inum(w[5], &seed))) is_wood = -1;
            for (int k = 1; k < nc; k++) {
                int n2 = words(cl[k], cw2, 8);
                if (!strcmp(cw2[0], "at") && n2 == 3 && !is_wood && inum(cw2[1], &x) && inum(cw2[2], &y)) have_at = 1;
                else if (!strcmp(cw2[0], "at") && n2 == 5 && is_wood == 1 && inum(cw2[1], &x) && inum(cw2[2], &y) && inum(cw2[3], &pw) && inum(cw2[4], &ph))
                    have_at = 1;
                else if (n2 == 2 && !strcmp(cw2[0], "as") && strlen(cw2[1]) < sizeof as) snprintf(as, sizeof as, "%s", cw2[1]);
                else have_at = -1;
            }
            if (is_wood < 0 || have_at != 1 || (nw != 1 && !is_wood))
                return ferr(c, l->line, "place = TEXTURE, at X Y[, as NAME]  or  place = wood R G B seed S, at X Y W H[, as NAME]");
            const uint8_t *src = NULL;
            if (!is_wood && !(src = image_ref(c, w[0], &pw, &ph, NULL, l->line))) return -1;
            if (x < 0 || y < 0 || pw < 1 || ph < 1 || x + pw > t->w || y + ph > t->h)
                return ferr(c, l->line, "place: %dx%d at %d %d does not fit the %dx%d canvas", pw, ph, x, y, t->w, t->h);
            if (is_wood) wood_fill(t->rgba + ((size_t)y * t->w + x) * 4, pw, ph, t->w, col, seed);
            else
                for (int yy = 0; yy < ph; yy++) memcpy(t->rgba + ((size_t)(y + yy) * t->w + x) * 4, src + (size_t)yy * pw * 4, (size_t)pw * 4);
            if (t->nreg == MAX_REGIONS) return ferr(c, l->line, "more than %d places", MAX_REGIONS);
            Region *g = &t->reg[t->nreg++];
            snprintf(g->name, sizeof g->name, "%s", as[0] ? as : is_wood ? "wood" : w[0]);
            g->x = x, g->y = y, g->w = pw, g->h = ph;
        } else {
            return ferr(c, l->line, "unknown key %s in [texture %s]", l->key, sec->name);
        }
    }
    if (!t->rgba) return ferr(c, sec->line, "[texture %s] has no base = or canvas = line", sec->name);
    return 0;
}

/* ---- output files ---- */

static int out_path(Ctx *c, const char *group, const char *stem, const char *ext, char *path, size_t cap, int line) {
    if (group && *group) {
        snprintf(path, cap, "%s/%s", c->out, group);
        struct stat st;
        if (stat(path, &st) && ma_mkdir(path)) return ferr(c, line, "cannot create %s", path);
        snprintf(path, cap, "%s/%s/%s%s", c->out, group, stem, ext);
    } else {
        snprintf(path, cap, "%s/%s%s", c->out, stem, ext);
    }
    return 0;
}

static int counted(Ctx *c, const char *path, int line) {
    struct stat st;
    if (stat(path, &st)) return ferr(c, line, "cannot write %s", path);
    c->files++;
    c->bytes += (uint64_t)st.st_size;
    if ((unsigned)c->files > modassets_files_max) return ferr(c, line, "more than %u output files (budget)", modassets_files_max);
    if (c->bytes > modassets_bytes_max) return ferr(c, line, "more than %u bytes of output (budget)", modassets_bytes_max);
    return 0;
}

static int write_bytes(Ctx *c, const char *path, const void *p, size_t n, int line) {
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (!ok) return ferr(c, line, "cannot write %s", path);
    return counted(c, path, line);
}

/* A WMP1 file (meshpack.h): {nv, ni, stride0, stride1}, the streams, the u16 indices. */
static int write_wmesh(Ctx *c, const char *path, const uint8_t *s0, int stride0, const uint8_t *s1, int stride1, int nv, const uint16_t *idx,
                       int ni, int line) {
    if (!ni || ni % 3 || nv < 1 || nv > MESHPACK_MAX_VERTS) return ferr(c, line, "the mesh is empty or too large (%d vertices)", nv);
    size_t n = 20 + (size_t)nv * stride0 + (size_t)nv * stride1 + (size_t)ni * 2;
    uint8_t *b = malloc(n);
    if (!b) return ferr(c, line, "out of memory");
    memcpy(b, "WMP1", 4);
    const uint32_t hdr[4] = {(uint32_t)nv, (uint32_t)ni, (uint32_t)stride0, (uint32_t)stride1};
    for (int k = 0; k < 4; k++)
        for (int q = 0; q < 4; q++) b[4 + 4 * k + q] = (uint8_t)(hdr[k] >> (8 * q));
    size_t o = 20;
    memcpy(b + o, s0, (size_t)nv * stride0), o += (size_t)nv * stride0;
    if (stride1) memcpy(b + o, s1, (size_t)nv * stride1), o += (size_t)nv * stride1;
    for (int k = 0; k < ni; k++) b[o + 2 * k] = (uint8_t)idx[k], b[o + 2 * k + 1] = (uint8_t)(idx[k] >> 8);
    int r = write_bytes(c, path, b, n, line);
    free(b);
    return r;
}

/* The host object `model` as this disc draws it: the draw hash computed from its prop chunk (the
 * vertex and index bytes as stored, src/meshover.h meshover_draw_hash), so a host follows the disc;
 * a disc without the chunk gets the hash `--mesh-log` logs on the developer's disc. */
static int host_resolve(Ctx *c, const char *model, Host *h, int line) {
    const HostKnown *k = NULL;
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++)
        if (!strcmp(hosts[i].model, model)) k = &hosts[i];
    if (!k) return ferr(c, line, "unknown host object %s (" HOST_NAMES ")", model);
    *h = (Host){k->hash, 36, 0, 12, 24, 28};
    if (disc_ready(c, line)) {   /* a disc folder without containers (a [hide] needs none) */
        if (c->err && c->cap) c->err[0] = 0;
        return 0;
    }
    const DiscChunk *ch = find_chunk(c, DISC_CLASS_MESH, k->prop, 2, 0x6D);
    if (!ch) return 0;
    uint8_t *p = disc_chunk_read(c->disc, ch);
    DiscProp pr;
    char why[128] = "read error";
    if (!p || disc_prop_parse(p, ch->size, &pr, why, sizeof why)) {
        free(p);
        return ferr(c, line, "host %s: %s", model, why);
    }
    if (pr.stride != 36 || pr.normal != 12 || pr.colour != 24 || pr.uv0 != 28 || !pr.nidx) {
        free(p);
        return ferr(c, line, "host %s: a vertex layout this engine does not write (stride %u, FVF %#x)", model, pr.stride, pr.fvf);
    }
    uint16_t *idx = malloc(sizeof *idx * pr.nidx);
    if (!idx) {
        free(p);
        return ferr(c, line, "out of memory");
    }
    uint32_t lo = 0xffff, hi = 0;
    for (uint32_t i = 0; i < pr.nidx; i++) {
        idx[i] = (uint16_t)(pr.idx[2 * i] | pr.idx[2 * i + 1] << 8);
        lo = idx[i] < lo ? idx[i] : lo, hi = idx[i] > hi ? idx[i] : hi;
    }
    h->hash = meshpack_hash(idx, pr.nidx, lo, hi, pr.vb, pr.stride, NULL, 0);
    if (pr.prim != 5) h->hash ^= (uint64_t)pr.prim * 0x9e3779b97f4a7c15ull;
    free(idx), free(p);
    return 0;
}

static int hex_colour(const char *s, uint32_t *v) {
    if (strlen(s) != 8) return 0;
    char *end;
    *v = (uint32_t)strtoul(s, &end, 16);
    return !*end;
}

static int write_png_counted(Ctx *c, const char *path, int w, int h, const uint8_t *p, int line) {
    if (ma_png_write(path, w, h, p)) return ferr(c, line, "cannot write %s", path);
    return counted(c, path, line);
}

static int yes(const MaLine *l) { return l && !strcmp(l->value, "yes"); }


/* NORMPACKED3 (tools/mesh_pack wmesh.pack_normal): x, y in 11 bits (x 1023), z in 10 (x 511). */
static uint32_t pack_normal(const float *n) {
    uint32_t r = 0;
    const int bits[3] = {11, 11, 10}, shift[3] = {0, 11, 22};
    const double scale[3] = {1023.0, 1023.0, 511.0};
    for (int k = 0; k < 3; k++) {
        double v = n[k] > 1 ? 1 : n[k] < -1 ? -1 : n[k];
        r |= ((uint32_t)(int32_t)lrint(v * scale[k]) & ((1u << bits[k]) - 1)) << shift[k];
    }
    return r;
}

/* take = all of an own mesh: the object layout (a host object; rigid = joint J, of CHAR takes it
 * from CHAR's bind pose into joint J's frame) or, with target =, the character layout (its skin's
 * joints, or rigid on joint = J), optionally after the target's own triangles (append = yes). */
static int own_take(Ctx *c, const MaSection *sec, Source *src, Mesh *m, int line) {
    const MaLine *rig = line_of(sec, c->r, "rigid"), *tgt = line_of(sec, c->r, "target"), *jl = line_of(sec, c->r, "joint"),
                 *app = line_of(sec, c->r, "append");
    const MaOwnMesh *o = &src->own;
    char cl[4][160], w[8][160];
    if (!tgt) {
        if (jl || app) return ferr(c, (jl ? jl : app)->line, "%s = goes with target = (a part of a character)", jl ? "joint" : "append");
        const float *mat = NULL;
        if (rig) {
            int nc = clauses(rig->value, cl, 4), j = nc == 2 && words(cl[0], w, 8) == 2 && !strcmp(w[0], "joint") ? ma_joint_index(w[1]) : -1;
            Source *cs = NULL;
            if (j >= 0 && words(cl[1], w, 8) == 2 && !strcmp(w[0], "of") && !(cs = source(c, w[1], rig->line))) return -1;
            if (j < 2 || !cs) return ferr(c, rig->line, "rigid = joint NAME, of CHARACTER (a bip_* joint that carries vertices; CHARACTER: a [source] char =)");
            if (cs->is_prop || cs->is_own) return ferr(c, rig->line, "rigid = ..., of %s: a character [source] (char =)", cs->name);
            mat = cs->ch.inverse_bind[j - 2];
        }
        for (int v = 0; v < o->nv; v++) {
            double p[3] = {o->pos[v][0], o->pos[v][1], o->pos[v][2]}, n[3] = {o->nrm[v][0], o->nrm[v][1], o->nrm[v][2]};
            const double uv[2] = {o->uv[v][0], o->uv[v][1]};
            if (mat) {
                double tp[3], tn[3];
                row_transform(mat, p, 1.0, tp);
                row_transform(mat, n, 0.0, tn);
                normalize(tn);
                memcpy(p, tp, sizeof p), memcpy(n, tn, sizeof n);
            }
            if (mesh_push(m, p, n, uv) < 0) return ferr(c, line, "out of memory");
        }
        for (int i = 0; i < o->ni; i++)
            if (idx_push(m, o->idx[i])) return ferr(c, line, "out of memory");
        return 0;
    }
    if (rig) return ferr(c, rig->line, "rigid = is for a host object; a part of a character follows its skin, or joint = NAME");
    if (app && !yes(app)) return ferr(c, app->line, "append = yes (the part is drawn with the character's own triangles)");
    if (words(tgt->value, w, 8) < 1) return ferr(c, tgt->line, "target = SOURCE or target = SOURCE skin N");
    Source *ts = source(c, w[0], tgt->line);
    if (!ts) return -1;
    if (ts->is_prop || ts->is_own) return ferr(c, tgt->line, "target = names a character");
    int rj = -1;
    if (jl) {
        if (o->skinned) return ferr(c, jl->line, "joint = is for a mesh without a skin: %s is skinned (its weights are used)", src->model);
        if (words(jl->value, w, 8) != 1 || (rj = ma_joint_index(w[0])) < 2)
            return ferr(c, jl->line, "joint = NAME: a bip_* joint that carries vertices (closest: %s)", ma_joint_closest(jl->value));
    } else if (!o->skinned) {
        return ferr(c, line, "%s has no skin: weight it to the game's joints (bip_*), or attach it rigidly with joint = NAME", src->model);
    }
    /* the joints the target's draw is animated with (the slots its own vertices use) */
    uint8_t used[DISC_PALETTE] = {0};
    for (uint32_t v = 0; v < ts->ch.nv; v++) {
        const uint8_t *b = ts->ch.s1 + (size_t)v * DISC_STRIDE1;
        for (int k = 0; k < 3; k++) {
            const int16_t sl = (int16_t)(b[2 * k] | b[2 * k + 1] << 8);
            if (sl >= 0 && sl / 3 < DISC_PALETTE && rdf(b + 6 + 4 * k) > 0) used[sl / 3] = 1;
        }
    }
    for (int v = 0; v < o->nv; v++)
        for (int k = 0; k < 3; k++) {
            const int sl = jl ? (k ? -1 : rj - 2) : o->slot[v][k];
            if (sl < 0 || used[sl]) continue;
            char list[300] = "";
            size_t q = 0;
            for (int u = 0; u < DISC_PALETTE && q + 24 < sizeof list; u++)
                if (used[u]) q += (size_t)snprintf(list + q, sizeof list - q, "%s%s", q ? " " : "", joints[u + 2]);
            return ferr(c, jl ? jl->line : line, "joint %s does not move in %s's draw (its joints: %s)", joints[sl + 2], ts->model, list);
        }
    const int base = app ? (int)ts->ch.nv : 0, nv = base + o->nv, ni = (app ? (int)ts->ch.ni : 0) + o->ni;
    if (nv > MESHPACK_MAX_VERTS) return ferr(c, line, "%d vertices with %s's own (at most %d)", nv, ts->model, MESHPACK_MAX_VERTS);
    m->s0 = malloc((size_t)nv * DISC_STRIDE0);
    m->s1 = malloc((size_t)nv * DISC_STRIDE1);
    m->idx = malloc(sizeof(uint16_t) * (size_t)ni);
    if (!m->s0 || !m->s1 || !m->idx) return ferr(c, line, "out of memory");
    if (app) {
        memcpy(m->s0, ts->ch.s0, (size_t)base * DISC_STRIDE0);
        memcpy(m->s1, ts->ch.s1, (size_t)base * DISC_STRIDE1);
        for (uint32_t i = 0; i < ts->ch.ni; i++) m->idx[m->ni++] = (uint16_t)vidx(ts, (int)i);
    }
    for (int v = 0; v < o->nv; v++) {
        uint8_t *a = m->s0 + (size_t)(base + v) * DISC_STRIDE0, *b = m->s1 + (size_t)(base + v) * DISC_STRIDE1;
        for (int k = 0; k < 3; k++) wrf(a + 4 * k, o->pos[v][k]);
        const uint32_t pn = pack_normal(o->nrm[v]);
        for (int k = 0; k < 4; k++) a[12 + k] = (uint8_t)(pn >> (8 * k));
        wrf(a + 16, o->uv[v][0]), wrf(a + 20, o->uv[v][1]), wrf(a + 24, o->uv[v][0]), wrf(a + 28, o->uv[v][1]);
        for (int k = 0; k < 3; k++) {
            const int sl = jl ? (k ? -1 : rj - 2) : o->slot[v][k];
            const int16_t off = (int16_t)(sl >= 0 ? sl * 3 : -3);
            b[2 * k] = (uint8_t)off, b[2 * k + 1] = (uint8_t)((uint16_t)off >> 8);
            wrf(b + 6 + 4 * k, jl ? (k ? 0.0 : 1.0) : o->w[v][k]);
        }
    }
    for (int i = 0; i < o->ni; i++) m->idx[m->ni++] = (uint16_t)(base + o->idx[i]);
    m->nv = nv;
    return 0;
}

static const char *const mesh_keys[] = {"from",   "head_swap", "take",  "drop",   "rigid", "shape", "uv",   "paint",  "break", "host",
                                        "colour", "texture",   "crack", "target", "group", "worn",  "placed", "joint", "append", NULL};

/* The mesh of a [mesh] section: m (character layout for head_swap / drop, object layout else);
 * paint/break/crack state for the texture. */
typedef struct {
    Mesh m;
    uint8_t *paint;   /* paint =: the texture made here (size x size) */
    int psize;
    double (*kept)[3][2];   /* break =: the kept lens's UV triangles */
    int nkept;
} Built;

static int mesh_build(Ctx *c, const MaSection *sec, Source *src, Built *bd) {
    const MaLine *swap = line_of(sec, c->r, "head_swap"), *take = line_of(sec, c->r, "take"), *drop = line_of(sec, c->r, "drop"),
                 *rig = line_of(sec, c->r, "rigid"), *shape = line_of(sec, c->r, "shape"), *uvl = line_of(sec, c->r, "uv"),
                 *pnt = line_of(sec, c->r, "paint"), *brk = line_of(sec, c->r, "break");
    const int L = sec->line;
    Mesh *m = &bd->m;
    char cl[8][160], w[8][160];
    if (rig && !take) return ferr(c, rig->line, "rigid = goes with take =");
    if ((pnt || brk) && !take) return ferr(c, (pnt ? pnt : brk)->line, "%s = works on a take = mesh", pnt ? "paint" : "break");
    if (swap) {
        int nc = clauses(swap->value, cl, 8);
        double z;
        Source *b = nc >= 2 ? source(c, cl[0], swap->line) : NULL;
        if (nc >= 2 && !b) return -1;
        int need = nc == 3 && words(cl[2], w, 8) == 2 && !strcmp(w[0], "closed_seam") && !strcmp(w[1], "required");
        if (!b || words(cl[1], w, 8) != 2 || strcmp(w[0], "above") || !num(w[1], &z) || (nc == 3 && !need) || nc > 3)
            return ferr(c, swap->line, "head_swap = OTHER, above Z[, closed_seam required]");
        if (src->is_prop || b->is_prop) return ferr(c, swap->line, "head_swap needs two characters");
        return head_swap(c, src, b, z, need, m, swap->line);
    }
    if (drop) {
        /* dirty_fights.without: the character's vertices unchanged, the selected triangles (by their
         * index triples) left out of the index list */
        Tris t = {0};
        if (src->is_prop) return ferr(c, drop->line, "drop = works on a character");
        if (selector(c, drop->value, src, &t, drop->line)) return tris_free(&t), -1;
        if (t.s != src) return tris_free(&t), ferr(c, drop->line, "drop: the triangles must be from's");
        const int nt = (int)src->ch.ni / 3;
        uint64_t *gone = malloc(sizeof *gone * (size_t)(t.n + 1));
        m->s0 = malloc((size_t)src->ch.nv * DISC_STRIDE0);
        m->s1 = malloc((size_t)src->ch.nv * DISC_STRIDE1);
        m->idx = malloc(sizeof(uint16_t) * (size_t)(src->ch.ni + 1));
        if (!gone || !m->s0 || !m->s1 || !m->idx) return free(gone), tris_free(&t), ferr(c, drop->line, "out of memory");
        for (int i = 0; i < t.n; i++)
            gone[i] = (uint64_t)vidx(src, 3 * t.t[i]) << 32 | (uint64_t)vidx(src, 3 * t.t[i] + 1) << 16 | vidx(src, 3 * t.t[i] + 2);
        for (int i = 0; i < nt; i++) {
            const uint64_t k = (uint64_t)vidx(src, 3 * i) << 32 | (uint64_t)vidx(src, 3 * i + 1) << 16 | vidx(src, 3 * i + 2);
            int hit = 0;
            for (int j = 0; j < t.n && !hit; j++) hit = gone[j] == k;
            if (!hit)
                for (int j = 0; j < 3; j++) m->idx[m->ni++] = (uint16_t)vidx(src, 3 * i + j);
        }
        free(gone), tris_free(&t);
        memcpy(m->s0, src->ch.s0, (size_t)src->ch.nv * DISC_STRIDE0);
        memcpy(m->s1, src->ch.s1, (size_t)src->ch.nv * DISC_STRIDE1);
        m->nv = (int)src->ch.nv;
        return 0;
    }
    if (take && src->is_own) {
        if (strcmp(take->value, "all")) return ferr(c, take->line, "your own mesh is taken whole: take = all");
        if (pnt || brk) return ferr(c, (pnt ? pnt : brk)->line, "%s = works on the game's meshes, not your own", pnt ? "paint" : "break");
        return own_take(c, sec, src, m, take->line);
    }
    if (take) {
        if (src->is_prop) {
            if (strcmp(take->value, "all")) return ferr(c, take->line, "a prop is taken whole: take = all");
            if (rig) return ferr(c, rig->line, "a prop's vertices are in its own frame: no rigid =");
            if (prop_mesh(c, src, m, take->line)) return -1;
        } else {
            Tris t = {0};
            if (selector(c, take->value, src, &t, take->line)) return tris_free(&t), -1;
            if (!t.n) return tris_free(&t), ferr(c, take->line, "take: no triangles selected");
            if (!rig) return tris_free(&t), ferr(c, L, "[mesh %s]: take = needs rigid = joint NAME (the object's frame)", sec->name);
            int nj = words(rig->value, w, 8), j = 0;
            while (nj == 2 && j < 34 && strcmp(joints[j], w[1])) j++;
            if (nj != 2 || strcmp(w[0], "joint") || j < 2 || j >= 34)
                return tris_free(&t), ferr(c, rig->line, "rigid = joint NAME (a bip_* joint of the rig that carries vertices)");
            int r = rigid(c, &t, src->ch.inverse_bind[j - 2], m, take->line);
            tris_free(&t);
            if (r) return -1;
        }
        Break bk = {28.0, 0.035, 35.0, 9, 1};
        if (brk) {
            /* break = turn DEG, gap M, lens DEG, shards N, seed S */
            int nc = clauses(brk->value, cl, 8), ok = nc >= 1;
            for (int k = 0; ok && k < nc; k++) {
                ok = words(cl[k], w, 8) == 2;
                if (!ok) break;
                if (!strcmp(w[0], "turn")) ok = num(w[1], &bk.turn);
                else if (!strcmp(w[0], "gap")) ok = num(w[1], &bk.gap);
                else if (!strcmp(w[0], "lens")) ok = num(w[1], &bk.lens) && bk.lens > 0 && bk.lens < 90;
                else if (!strcmp(w[0], "shards")) ok = inum(w[1], &bk.shards) && bk.shards >= 0 && bk.shards <= 64;
                else if (!strcmp(w[0], "seed")) ok = inum(w[1], &bk.seed);
                else ok = 0;
            }
            if (!ok) return ferr(c, brk->line, "break = turn DEG, gap M, lens DEG, shards N, seed S");
        }
        if (pnt) {
            /* paint = frame R G B, lens R G B, size N */
            int frame[3], lensc[3], size = 0, have = 0, nc = clauses(pnt->value, cl, 8);
            for (int k = 0; k < nc; k++) {
                int n2 = words(cl[k], w, 8);
                if (n2 == 4 && !strcmp(w[0], "frame") && rgb_args(w, 4, 1, frame)) have |= 1;
                else if (n2 == 4 && !strcmp(w[0], "lens") && rgb_args(w, 4, 1, lensc)) have |= 2;
                else if (n2 == 2 && !strcmp(w[0], "size") && inum(w[1], &size) && size >= 2 && size <= MODASSETS_TEX_MAX) have |= 4;
                else have = -8;
            }
            if (have != 7) return ferr(c, pnt->line, "paint = frame R G B, lens R G B, size N");
            if (paint(m, bk.lens, frame, lensc, size, &bd->paint)) return ferr(c, pnt->line, "out of memory");
            bd->psize = size;
        }
        if (brk) {
            Mesh b = {0};
            if (break_glasses(m, &bk, &b, &bd->kept, &bd->nkept)) {
                mesh_free(&b);
                return ferr(c, brk->line, "break: out of memory or more than %d vertices", MESHPACK_MAX_VERTS);
            }
            mesh_free(m);
            *m = b;
        }
        return 0;
    }
    /* shape = shadow_box, board W H texels M, bar B, depth D, board_depth E; uv = board TEX.REGION, wood TEX.REGION */
    int nc = clauses(shape->value, cl, 8);
    double bw = 0, bh = 0, mpt = 0, bar = -1, depth = -1, bdp = -1, uvb[4], uvw[4];
    int ok = nc == 5 && !strcmp(cl[0], "shadow_box");
    for (int k = 1; ok && k < nc; k++) {
        int n2 = words(cl[k], w, 8);
        if (n2 == 5 && !strcmp(w[0], "board") && num(w[1], &bw) && num(w[2], &bh) && !strcmp(w[3], "texels") && num(w[4], &mpt)) {}
        else if (n2 == 2 && !strcmp(w[0], "bar") && num(w[1], &bar)) {}
        else if (n2 == 2 && !strcmp(w[0], "depth") && num(w[1], &depth)) {}
        else if (n2 == 2 && !strcmp(w[0], "board_depth") && num(w[1], &bdp)) {}
        else ok = 0;
    }
    if (!ok || bw <= 0 || bh <= 0 || mpt <= 0 || bar <= 0 || depth <= 0 || bdp <= 0)
        return ferr(c, shape->line, "shape = shadow_box, board W H texels M, bar B, depth D, board_depth E");
    int have = 0;
    nc = uvl ? clauses(uvl->value, cl, 8) : 0;
    for (int k = 0; k < nc; k++) {
        if (words(cl[k], w, 8) != 2 || (strcmp(w[0], "board") && strcmp(w[0], "wood"))) break;
        char tn[160];
        snprintf(tn, sizeof tn, "%s", w[1]);
        char *dot = strchr(tn, '.');
        if (!dot) break;
        *dot = 0;
        Tex *t = tex_find(c, tn);
        const Region *g = NULL;
        for (int q = 0; t && q < t->nreg; q++)
            if (!strcmp(t->reg[q].name, dot + 1)) g = &t->reg[q];
        if (!g) return ferr(c, uvl->line, "no place %s in an earlier [texture %s]", dot + 1, tn);
        double *o = !strcmp(w[0], "board") ? uvb : uvw;
        o[0] = (g->x + 0.5) / t->w, o[1] = (g->y + 0.5) / t->h, o[2] = (g->x + g->w - 0.5) / t->w, o[3] = (g->y + g->h - 0.5) / t->h;
        have |= o == uvb ? 1 : 2;
    }
    if (have != 3) return ferr(c, uvl ? uvl->line : L, "uv = board TEXTURE.PLACE, wood TEXTURE.PLACE");
    if (shadow_box(m, bw * mpt, bh * mpt, bar, depth, bdp, uvb, uvw)) return ferr(c, shape->line, "out of memory");
    return 0;
}

static int mesh_section(Ctx *c, const MaSection *sec) {
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        int k = 0;
        while (mesh_keys[k] && strcmp(mesh_keys[k], l->key)) k++;
        if (!mesh_keys[k]) return ferr(c, l->line, "unknown key %s in [mesh %s]", l->key, sec->name);
        for (int j = 0; j < i; j++)
            if (!strcmp(c->r->lines[sec->first + j].key, l->key)) return ferr(c, l->line, "a second %s = line", l->key);
    }
    const MaLine *from = line_of(sec, c->r, "from"), *swap = line_of(sec, c->r, "head_swap"), *take = line_of(sec, c->r, "take"),
                 *drop = line_of(sec, c->r, "drop"), *shape = line_of(sec, c->r, "shape"), *host = line_of(sec, c->r, "host"),
                 *colour = line_of(sec, c->r, "colour"), *texl = line_of(sec, c->r, "texture"), *target = line_of(sec, c->r, "target"),
                 *group = line_of(sec, c->r, "group"), *pnt = line_of(sec, c->r, "paint"), *crk = line_of(sec, c->r, "crack"),
                 *brk = line_of(sec, c->r, "break"), *worn = line_of(sec, c->r, "worn"), *placed = line_of(sec, c->r, "placed");
    const int L = sec->line;
    if (!!swap + !!take + !!drop + !!shape != 1) return ferr(c, L, "[mesh %s] needs exactly one of head_swap =, take =, drop =, shape =", sec->name);
    if (!!target + !!host != 1) return ferr(c, L, "[mesh %s] needs exactly one of target = (a character's draw) or host = (an object)", sec->name);
    if ((swap || drop) && host) return ferr(c, host->line, "a %s is a character draw: target =, not host =", swap ? "head swap" : "drop");
    if (group && !strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", group->value[0]))
        return ferr(c, group->line, "a group is a folder name: letters, digits, - and _");
    for (const char *g = group ? group->value : ""; *g; g++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_", *g)) return ferr(c, group->line, "a group is a folder name: letters, digits, - and _");
    if (worn && (!yes(worn) || !host || !group)) return ferr(c, worn->line, "worn = yes (a host mesh in a group: the group's instance marker)");
    if (placed && (!yes(placed) || !group)) return ferr(c, placed->line, "placed = yes (a mesh in a group: drawn where the mod's Lua puts points)");
    if (crk && !brk) return ferr(c, crk->line, "crack = needs break = (it cracks the kept lens)");
    if (pnt && texl) return ferr(c, texl->line, "paint = makes the texture: no texture =");
    Source *src = NULL;
    if (from && !(src = source(c, from->value, from->line))) return -1;
    if ((swap || take || drop) && !src) return ferr(c, L, "[mesh %s] needs from = SOURCE", sec->name);
    if ((shape || (take && !src->is_own)) && target) return ferr(c, target->line, "an object mesh replaces a host object: host =, not target =");
    const MaLine *jl = line_of(sec, c->r, "joint"), *app = line_of(sec, c->r, "append");
    if ((jl || app) && !(src && src->is_own && take)) return ferr(c, (jl ? jl : app)->line, "%s = is for your own mesh (from = a [source] file =)", jl ? "joint" : "append");
    /* target = SOURCE (every skin of its draw: no texture) or target = SOURCE skin N */
    char w[8][160];
    int tw = target ? words(target->value, w, 8) : 0, skin_n = -1;
    Source *ts = NULL;
    if (target) {
        if (!(tw == 1 || (tw == 3 && !strcmp(w[1], "skin") && inum(w[2], &skin_n)))) return ferr(c, target->line, "target = SOURCE or target = SOURCE skin N");
        if (!(ts = source(c, w[0], target->line))) return -1;
        if (ts->is_prop) return ferr(c, target->line, "target = names a character");
        if (tw == 1 && texl) return ferr(c, texl->line, "target = %s replaces the draw of every skin: no texture =", w[0]);
    }
    if (!texl && !pnt && tw != 1) return ferr(c, L, "[mesh %s] needs texture = (SOURCE.skinN or a [texture] name)", sec->name);
    Host h = {0};
    if (host && host_resolve(c, host->value, &h, host->line)) return -1;
    uint32_t argb = 0;
    if (colour && !hex_colour(colour->value, &argb)) return ferr(c, colour->line, "colour = AARRGGBB (8 hex digits)");
    int crack_lines = 7, crack_seed = 1;
    if (crk) {
        char cl[4][160], cw[4][160];
        int nc = clauses(crk->value, cl, 4), ok = nc >= 1;
        for (int k = 0; ok && k < nc; k++) {
            ok = words(cl[k], cw, 4) == 2;
            if (ok && !strcmp(cw[0], "lines")) ok = inum(cw[1], &crack_lines) && crack_lines >= 1 && crack_lines <= 64;
            else if (ok && !strcmp(cw[0], "seed")) ok = inum(cw[1], &crack_seed);
            else ok = 0;
        }
        if (!ok) return ferr(c, crk->line, "crack = lines N, seed S");
    }
    Built bd = {0};
    int r = mesh_build(c, sec, src, &bd);
    Mesh *m = &bd.m;
    char stem[64], path[1400];
    const char *grp = group ? group->value : NULL;
    if (!r && target) {
        Skin *k = skin_n >= 0 ? skin(c, ts, skin_n, target->line) : NULL;
        if (skin_n >= 0 && !k) r = -1;
        else if (!m->s0) r = ferr(c, target->line, "target = replaces a character draw: the mesh must be a head_swap, a drop or your own mesh");
        else {
            if (k) snprintf(stem, sizeof stem, "%016llx-%016llx", (unsigned long long)ts->mesh_hash, (unsigned long long)k->hash);
            else snprintf(stem, sizeof stem, "%016llx", (unsigned long long)ts->mesh_hash);
            r = out_path(c, grp, stem, ".wmesh", path, sizeof path, L) ||
                write_wmesh(c, path, m->s0, DISC_STRIDE0, m->s1, DISC_STRIDE1, m->nv, m->idx, m->ni, L);
        }
    } else if (!r) {
        uint8_t *s0 = calloc((size_t)(m->no ? m->no : 1), (size_t)h.stride);
        if (!s0) r = ferr(c, L, "out of memory");
        for (int v = 0; s0 && v < m->no; v++) {
            uint8_t *b = s0 + (size_t)v * h.stride;
            for (int q = 0; q < 4; q++) b[h.colour + q] = (uint8_t)(argb >> (8 * q));
            for (int q = 0; q < 3; q++) wrf(b + h.pos + 4 * q, m->pos[v][q]), wrf(b + h.normal + 4 * q, m->nrm[v][q]);
            wrf(b + h.uv, m->uv[v][0]), wrf(b + h.uv + 4, m->uv[v][1]);
        }
        snprintf(stem, sizeof stem, "%016llx", (unsigned long long)h.hash);
        if (s0) r = out_path(c, grp, stem, ".wmesh", path, sizeof path, L) || write_wmesh(c, path, s0, h.stride, NULL, 0, m->no, m->idx, m->ni, L);
        free(s0);
    }
    /* the texture: a [texture] / SOURCE.skinN, or paint's; a copy when cracked */
    if (!r && (texl || pnt)) {
        int iw = bd.psize, ih = bd.psize;
        const uint8_t *img = pnt ? bd.paint : image_ref(c, texl->value, &iw, &ih, NULL, texl->line);
        uint8_t *copy = img && crk ? malloc((size_t)iw * ih * 4) : NULL;
        const int line = texl ? texl->line : pnt->line;
        if (!img) r = -1;
        else if (crk && !copy) r = ferr(c, crk->line, "out of memory");
        else {
            if (copy) {
                memcpy(copy, img, (size_t)iw * ih * 4);
                if (crack(copy, iw, ih, bd.kept, bd.nkept, crack_lines, crack_seed)) r = ferr(c, crk->line, "out of memory");
            }
            if (!r) r = out_path(c, grp, stem, ".png", path, sizeof path, line) || write_png_counted(c, path, iw, ih, copy ? copy : img, line);
        }
        free(copy);
    }
    if (!r && worn) r = out_path(c, grp, stem, ".worn", path, sizeof path, worn->line) || write_bytes(c, path, "", 0, worn->line);
    if (!r && placed) r = out_path(c, grp, "placed", "", path, sizeof path, placed->line) || write_bytes(c, path, "", 0, placed->line);
    mesh_free(m);
    free(bd.paint), free(bd.kept);
    return r ? -1 : 0;
}

static int hide_section(Ctx *c, const MaSection *sec) {
    const MaLine *host = NULL, *group = NULL;
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        if (!strcmp(l->key, "host")) host = l;
        else if (!strcmp(l->key, "group")) group = l;
        else return ferr(c, l->line, "unknown key %s in [hide %s]", l->key, sec->name);
    }
    if (!host) return ferr(c, sec->line, "[hide %s] needs host = OBJECT", sec->name);
    Host h;
    if (host_resolve(c, host->value, &h, host->line)) return -1;
    char stem[32], path[1400];
    snprintf(stem, sizeof stem, "%016llx", (unsigned long long)h.hash);
    if (out_path(c, group ? group->value : NULL, stem, ".hide", path, sizeof path, sec->line)) return -1;
    return write_bytes(c, path, "", 0, sec->line);
}

/* ---- [set] and [layer]: surface effects painted over a character's clothes ----
 *
 * The reference is the original Python recipe (1.1), ported operation by operation:
 * the same texel order (Python dicts keep insertion order; the order decides which colour a grown
 * texel takes), the same sums (Python's sum() of floats is Neumaier-compensated and math.dist is
 * CPython's vector_norm), the same xorshift numbers and integer hashes. */

static int set_section(Ctx *c, const MaSection *sec) {
    if (c->nset == MAX_SETS) return ferr(c, sec->line, "more than %d sets", MAX_SETS);
    Set *st = &c->set[c->nset];
    memset(st, 0, sizeof *st);
    snprintf(st->name, sizeof st->name, "%s", sec->name);
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        if (st->n == SET_MAX) return ferr(c, l->line, "more than %d entries in [set %s]", SET_MAX, sec->name);
        for (const char *p = l->key; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
                return ferr(c, l->line, "a set entry's code is a-z A-Z 0-9 - _ (CODE = MODEL[, MODEL...])");
        for (int k = 0; k < st->n; k++)
            if (!strcmp(st->e[k].code, l->key)) return ferr(c, l->line, "a second entry %s in [set %s]", l->key, sec->name);
        SetEntry *e = &st->e[st->n];
        char cl[SET_MODELS + 1][160];
        const int n = clauses(l->value, cl, SET_MODELS + 1);
        if (n > SET_MODELS) return ferr(c, l->line, "at most %d models per entry", SET_MODELS);
        for (int k = 0; k < n; k++) {
            if (!cl[k][0] || strlen(cl[k]) >= 64 || strchr(cl[k], ' ')) return ferr(c, l->line, "CODE = MODEL[, MODEL...] (CfgChar model keys)");
            snprintf(e->model[k], sizeof e->model[k], "%s", cl[k]);
        }
        snprintf(e->code, sizeof e->code, "%s", l->key);
        e->nm = n, e->line = l->line;
        st->n++;
    }
    if (!st->n) return ferr(c, sec->line, "[set %s] has no entries (lines CODE = MODEL[, MODEL...])", sec->name);
    c->nset++;
    return 0;
}

/* The parameters of a [layer] (defaults: recipe 1.1). Per-stage values are indexed stage - 1. */
typedef struct {
    double mz0, mz1, abs_x, legs_below, skin_island, head_above;   /* region = cloth(...) */
    double near, share;                                             /* protect = face_colour ... */
    double red_share, patch_bright, edge, patch_keep;               /* vest = ... */
    int grime;
    double tint[3], grime_scale, legs_bias[2], torso_bias[2], spread[3], dark[3], dark_cloth[3], desat[3];
    int dust;
    double dust_col[3], dust_scale, dust_edge[3], dust_legs, dust_below;
    int scuff;
    double scuff_amt[3];
    double stains[3], tears[3];
    int damp;
    double damp_stage, damp_above, damp_amount;
    int wear;
    double wear_amt[3];
    double grow;
    int skip_ghost;
    double min_painted;
    int stage[3], nstage;
    double seed_a, seed_b;
    int all_skins;
    char suffix[8][16];
    int nsuffix;
    int neffects;
} LayerP;

static void layer_defaults(LayerP *P) {
    static const LayerP d = {
        0.06, 1.66, 0.3, 1.2, 0.3, 1.55, 3, 0.25, 0.4, 1.45, 6, 0.12,
        0, {0.48, 0.43, 0.36}, 7, {0.45, 0.9}, {1.3, 0.8}, {0.35, 0.65, 1.0}, {0.25, 0.42, 0.55}, {0.30, 0.45, 0.55}, {0.0, 0.10, 0.18},
        0, {128, 118, 100}, 11, {0.16, 0.22, 0.28}, 0.55, 0.7,
        0, {0.30, 0.40, 0.50},
        {0, 3, 6}, {0, 2, 7},
        0, 3, 1.38, 0.3,
        0, {0.0, 0.03, 0.07},
        2, 0, 0.0, {1, 2, 3}, 3, 1000, 37, 0, {{0}}, 0, 0};
    *P = d;
}

typedef struct {
    const char *name;
    double *dst;
    int n;
} Field;

/* Clauses "NAME n1 [n2 n3]" of value v (from clause `from`) into the fields. */
static int fields(Ctx *c, int line, const char *what, char cl[][160], int ncl, int from, const Field *f) {
    for (int i = from; i < ncl; i++) {
        char w[6][160];
        const int nw = words(cl[i], w, 6);
        if (!nw) return ferr(c, line, "empty clause in %s", what);
        int k = 0;
        while (f[k].name && strcmp(f[k].name, w[0])) k++;
        if (!f[k].name) return ferr(c, line, "unknown %s value %s", what, w[0]);
        if (nw - 1 != f[k].n) return ferr(c, line, "%s %s takes %d number%s", what, w[0], f[k].n, f[k].n == 1 ? "" : "s");
        for (int j = 0; j < f[k].n; j++)
            if (!num(w[1 + j], &f[k].dst[j])) return ferr(c, line, "%s %s: %s is not a number", what, w[0], w[1 + j]);
    }
    return 0;
}

static int range2(const char *s, double *a, double *b) {
    const char *dd = strstr(s, "..");
    if (!dd || dd == s || (size_t)(dd - s) >= 60) return 0;
    char x[64];
    memcpy(x, s, (size_t)(dd - s));
    x[dd - s] = 0;
    return num(x, a) && num(dd + 2, b);
}

static int effect_line(Ctx *c, const MaLine *l, LayerP *P) {
    char cl[16][160], w[8][160];
    const int ncl = clauses(l->value, cl, 16);
    const int nw = words(cl[0], w, 8);
    if (!nw) return ferr(c, l->line, "effect = KIND ...");
    const char *k = w[0];
    /* the rest of the first clause is a clause of its own */
    const char *rest = cl[0] + strlen(k);
    while (*rest == ' ' || *rest == '\t') rest++;
    snprintf(cl[0], sizeof cl[0], "%s", rest);
    const int from = cl[0][0] ? 0 : 1;
    double *three = NULL;
    if (!strcmp(k, "scuff")) three = P->scuff_amt, P->scuff = 1;
    else if (!strcmp(k, "stains")) three = P->stains;
    else if (!strcmp(k, "tears")) three = P->tears;
    else if (!strcmp(k, "wear")) three = P->wear_amt, P->wear = 1;
    P->neffects++;
    if (three) {
        if (ncl != 1 || nw != 4) return ferr(c, l->line, "effect = %s A B C (stages 1, 2, 3)", k);
        for (int j = 0; j < 3; j++)
            if (!num(w[1 + j], &three[j]) || three[j] < 0) return ferr(c, l->line, "effect %s: %s is not a number >= 0", k, w[1 + j]);
        if (!strcmp(k, "stains") || !strcmp(k, "tears"))
            for (int j = 0; j < 3; j++)
                if (three[j] != floor(three[j]) || three[j] > (k[0] == 's' ? 16 : 7))
                    return ferr(c, l->line, "effect %s: whole counts, at most %d", k, k[0] == 's' ? 16 : 7);
        return 0;
    }
    if (!strcmp(k, "grime")) {
        P->grime = 1;
        const Field f[] = {{"tint", P->tint, 3},       {"scale", &P->grime_scale, 1}, {"legs_bias", P->legs_bias, 2},
                           {"torso_bias", P->torso_bias, 2}, {"spread", P->spread, 3},  {"dark", P->dark, 3},
                           {"dark_cloth", P->dark_cloth, 3}, {"desat", P->desat, 3},    {NULL, NULL, 0}};
        if (fields(c, l->line, "grime", cl, ncl, from, f)) return -1;
        if (!(P->tint[0] > 0 || P->tint[1] > 0 || P->tint[2] > 0)) return ferr(c, l->line, "grime tint: one channel above 0");
        return 0;
    }
    if (!strcmp(k, "dust")) {
        P->dust = 1;
        const Field f[] = {{"colour", P->dust_col, 3}, {"scale", &P->dust_scale, 1}, {"edge", P->dust_edge, 3},
                           {"legs", &P->dust_legs, 1}, {"below", &P->dust_below, 1}, {NULL, NULL, 0}};
        return fields(c, l->line, "dust", cl, ncl, from, f);
    }
    if (!strcmp(k, "damp")) {
        P->damp = 1;
        const Field f[] = {{"stage", &P->damp_stage, 1}, {"above", &P->damp_above, 1}, {"amount", &P->damp_amount, 1}, {NULL, NULL, 0}};
        return fields(c, l->line, "damp", cl, ncl, from, f);
    }
    return ferr(c, l->line, "unknown effect %s (grime, dust, scuff, stains, tears, damp, wear)", k);
}

static const char *const layer_keys[] = {"for", "skins", "skip", "seed", "stages", "region", "protect", "vest", "effect", "grow", NULL};

static int layer_params(Ctx *c, const MaSection *sec, LayerP *P, const Set **set) {
    layer_defaults(P);
    *set = NULL;
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        int k = 0;
        while (layer_keys[k] && strcmp(layer_keys[k], l->key)) k++;
        if (!layer_keys[k]) return ferr(c, l->line, "unknown key %s in [layer %s]", l->key, sec->name);
        char cl[16][160], w[12][160];
        const int ncl = clauses(l->value, cl, 16);
        if (!strcmp(l->key, "for")) {
            for (int s = 0; s < c->nset && !*set; s++)
                if (!strcmp(c->set[s].name, l->value)) *set = &c->set[s];
            if (!*set) return ferr(c, l->line, "no [set %s] before this line", l->value);
        } else if (!strcmp(l->key, "skins")) {
            const int nw = words(l->value, w, 10);
            if (nw == 1 && !strcmp(w[0], "all")) {
                P->all_skins = 1;
                continue;
            }
            if (!nw || strcmp(w[0], "claimed")) return ferr(c, l->line, "skins = all, or skins = claimed [SUFFIX...]");
            for (int j = 1; j < nw; j++) {
                if (P->nsuffix == 8 || strlen(w[j]) >= 16) return ferr(c, l->line, "at most 8 suffixes of 15 characters");
                snprintf(P->suffix[P->nsuffix++], sizeof P->suffix[0], "%s", w[j]);
            }
        } else if (!strcmp(l->key, "skip")) {
            for (int j = 0; j < ncl; j++) {
                const int nw = words(cl[j], w, 4);
                if (nw == 1 && !strcmp(w[0], "ghost")) P->skip_ghost = 1;
                else if (nw == 3 && !strcmp(w[0], "painted") && !strcmp(w[1], "<") && num(w[2], &P->min_painted)) continue;
                else return ferr(c, l->line, "skip = ghost, painted < SHARE");
            }
        } else if (!strcmp(l->key, "seed")) {
            const int nw = words(l->value, w, 8);
            if (nw == 1 && num(w[0], &P->seed_a)) P->seed_b = 0;
            else if (!(nw == 5 && num(w[0], &P->seed_a) && !strcmp(w[1], "+") && num(w[2], &P->seed_b) && !strcmp(w[3], "*") &&
                       !strcmp(w[4], "{index}")))
                return ferr(c, l->line, "seed = N, or seed = A + B * {index}");
            if (P->seed_a != floor(P->seed_a) || P->seed_b != floor(P->seed_b) || fabs(P->seed_a) > 1e9 || fabs(P->seed_b) > 1e6)
                return ferr(c, l->line, "seed: whole numbers");
        } else if (!strcmp(l->key, "stages")) {
            P->nstage = 0;
            for (int j = 0; j < ncl; j++) {
                int st;
                if (!inum(cl[j], &st) || st < 1 || st > 3) return ferr(c, l->line, "stages are 1, 2 or 3");
                for (int q = 0; q < P->nstage; q++)
                    if (P->stage[q] == st) return ferr(c, l->line, "stage %d twice", st);
                if (P->nstage == 3) return ferr(c, l->line, "stages are 1, 2 or 3");
                P->stage[P->nstage++] = st;
            }
        } else if (!strcmp(l->key, "region")) {
            char in[160];
            const size_t L = strlen(l->value);
            if (strncmp(l->value, "cloth(", 6) || L < 7 || l->value[L - 1] != ')' || L - 7 >= sizeof in)
                return ferr(c, l->line, "region = cloth(mean_z A..B, abs_x <= X, legs below Z, skin_island S, head above H)");
            memcpy(in, l->value + 6, L - 7);
            in[L - 7] = 0;
            const int n = clauses(in, cl, 16);
            for (int j = 0; j < n; j++) {
                const int nw = words(cl[j], w, 4);
                int ok = 0;
                if (nw == 2 && !strcmp(w[0], "mean_z")) ok = range2(w[1], &P->mz0, &P->mz1);
                else if (nw == 3 && !strcmp(w[0], "abs_x") && !strcmp(w[1], "<=")) ok = num(w[2], &P->abs_x);
                else if (nw == 3 && !strcmp(w[0], "legs") && !strcmp(w[1], "below")) ok = num(w[2], &P->legs_below);
                else if (nw == 2 && !strcmp(w[0], "skin_island")) ok = num(w[1], &P->skin_island);
                else if (nw == 3 && !strcmp(w[0], "head") && !strcmp(w[1], "above")) ok = num(w[2], &P->head_above);
                if (!ok) return ferr(c, l->line, "region: unknown or bad clause \"%s\"", cl[j]);
            }
        } else if (!strcmp(l->key, "protect")) {
            const int nw = words(l->value, w, 8);
            if (!(nw == 5 && !strcmp(w[0], "face_colour") && !strcmp(w[1], "near") && num(w[2], &P->near) && !strcmp(w[3], "share") &&
                  num(w[4], &P->share)) ||
                P->near != floor(P->near) || P->near < 0 || P->near > 16)
                return ferr(c, l->line, "protect = face_colour near N share S (N 0-16 texels)");
        } else if (!strcmp(l->key, "vest")) {
            const Field f[] = {{"red", &P->red_share, 1}, {"patch", &P->patch_bright, 1}, {"edge", &P->edge, 1},
                               {"patch_keep", &P->patch_keep, 1}, {NULL, NULL, 0}};
            if (fields(c, l->line, "vest", cl, ncl, 0, f)) return -1;
            if (P->edge < 0 || P->edge > 64) return ferr(c, l->line, "vest edge: 0-64 texels");
        } else if (!strcmp(l->key, "effect")) {
            if (effect_line(c, l, P)) return -1;
        } else if (!strcmp(l->key, "grow")) {
            int g;
            if (!inum(l->value, &g) || g < 0 || g > 8) return ferr(c, l->line, "grow = N (0-8 texels)");
            P->grow = g;
        }
    }
    if (!*set) return ferr(c, sec->line, "[layer %s] needs for = SET", sec->name);
    if (!P->neffects) return ferr(c, sec->line, "[layer %s] needs at least one effect =", sec->name);
    if (P->nstage > 1 && !strstr(sec->name, "{stage}")) return ferr(c, sec->line, "the layer name needs {stage} (more than one stage)");
    if ((*set)->n > 1 && !strstr(sec->name, "{code}")) return ferr(c, sec->line, "the layer name needs {code} (more than one entry)");
    return 0;
}

/* ---- the arithmetic of dirty_clothes.py ---- */

static double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

static double hash3(int64_t x, int64_t y, int64_t z, int64_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u + (uint32_t)seed * 144665u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
}

static double value_noise(double x, double y, double z, int64_t seed) {
    const double fxi = floor(x), fyi = floor(y), fzi = floor(z);
    const int64_t xi = (int64_t)fxi, yi = (int64_t)fyi, zi = (int64_t)fzi;
    const double fx = x - fxi, fy = y - fyi, fz = z - fzi;
    const double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy), uz = fz * fz * (3 - 2 * fz);
    double cc[2][2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++) cc[i][j][k] = hash3(xi + i, yi + j, zi + k, seed);
    const double x00 = cc[0][0][0] + (cc[1][0][0] - cc[0][0][0]) * ux;
    const double x10 = cc[0][1][0] + (cc[1][1][0] - cc[0][1][0]) * ux;
    const double x01 = cc[0][0][1] + (cc[1][0][1] - cc[0][0][1]) * ux;
    const double x11 = cc[0][1][1] + (cc[1][1][1] - cc[0][1][1]) * ux;
    const double y0 = x00 + (x10 - x00) * uy;
    const double y1 = x01 + (x11 - x01) * uy;
    return y0 + (y1 - y0) * uz;
}

static double fbm(const double *p, int64_t seed, int octaves, double freq) {
    double s = 0.0, a = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; o++) {
        s += a * value_noise(p[0] * freq, p[1] * freq, p[2] * freq, seed + o * 17);
        norm += a;
        a *= 0.5;
        freq *= 2.03;
    }
    return s / norm;
}

typedef struct {
    uint32_t x;
} XorShift;
static double xs_next(XorShift *r) {
    uint32_t x = r->x;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->x = x;
    return x / 4294967296.0;
}

/* Python's sum() of floats (3.12+: Neumaier compensation; the int start 0 adds the first exactly). */
static double py_sum(const double *v, int n) {
    if (n <= 0) return 0.0;
    double f = v[0], comp = 0.0;
    for (int i = 1; i < n; i++) {
        const double x = v[i], t = f + x;
        if (fabs(f) >= fabs(x)) comp += (f - t) + x;
        else comp += (x - t) + f;
        f = t;
    }
    if (comp != 0.0 && isfinite(comp)) f += comp;
    return f;
}

/* Exact product (Dekker with Veltkamp's split): hi + lo = x * y. */
static void two_prod(double x, double y, double *hi, double *lo) {
    const double sp = 134217729.0;   /* 2^27 + 1 */
    const double z = x * y;
    double t = sp * x;
    const double xh = t - (t - x), xl = x - xh;
    t = sp * y;
    const double yh = t - (t - y), yl = y - yh;
    *hi = z;
    *lo = ((xh * yh - z) + xh * yl + xl * yh) + xl * yl;
}

/* math.dist of two 3D points: CPython's vector_norm (an accurate hypot). */
static double py_dist(const double *p, const double *q) {
    double v[3], mx = 0.0;
    for (int i = 0; i < 3; i++) {
        v[i] = fabs(p[i] - q[i]);
        if (v[i] > mx) mx = v[i];
    }
    if (mx == 0.0) return mx;
    int e;
    frexp(mx, &e);
    if (e < -1000) return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const double scale = ldexp(1.0, -e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0, hi, lo;
    for (int i = 0; i < 3; i++) {
        const double x = v[i] * scale;
        two_prod(x, x, &hi, &lo);
        const double s = csum + hi, sl = (csum - s) + hi;
        csum = s;
        frac1 += lo;
        frac2 += sl;
    }
    double h = sqrt(csum - 1.0 + (frac1 + frac2));
    two_prod(-h, h, &hi, &lo);
    const double s = csum + hi, sl = (csum - s) + hi;
    csum = s;
    frac1 += lo;
    frac2 += sl;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);
    return h / scale;
}

static double to_lin(double v) {
    v = clamp01(v / 255.0);
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static double to_srgb(double v) {
    v = clamp01(v);
    return 255.0 * (v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055);
}
static double darken(double f) {
    f = clamp01(f);
    return f == 0.0 ? 1.0 : pow(1.0 - f, 2.2);   /* pow(1, y) is 1 */
}
static void to_grey(double *lin, double t, double lift) {
    const double y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
    const double g = y * (1.0 + lift);
    for (int i = 0; i < 3; i++) lin[i] = lin[i] + (g - lin[i]) * t;
}
static double smooth(double e0, double e1, double v) {
    const double t = clamp01((v - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
static double py_mod(double x, double m) {
    double r = fmod(x, m);
    if (r != 0.0) {
        if ((m < 0) != (r < 0)) r += m;
    } else {
        r = copysign(0.0, m);
    }
    return r;
}
static void hsv(const uint8_t *c, double *hh, double *s, double *v) {
    const double r = c[0] / 255.0, g = c[1] / 255.0, b = c[2] / 255.0;
    const double mx = fmax(fmax(r, g), b), mn = fmin(fmin(r, g), b);
    if (mx <= 0 || mx == mn) {
        *hh = 0.0, *s = 0.0, *v = mx;
        return;
    }
    const double d = mx - mn;
    double h;
    if (mx == r) h = py_mod((g - b) / d, 6);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    *hh = h / 6.0, *s = d / mx, *v = mx;
}
static double red_weight(const uint8_t *c) {
    double hh, s, v;
    hsv(c, &hh, &s, &v);
    const double dist = fmin(hh, 1.0 - hh);
    return smooth(0.10, 0.06, dist) * smooth(0.10, 0.22, s) * smooth(0.06, 0.14, v);
}

static int skin_like(const uint8_t *c, const int *face) {
    const int s = c[0] + c[1] + c[2];
    int fs = face[0] + face[1] + face[2];
    if (!fs) fs = 1;
    if (s < 30) return 0;
    const double q = (double)s / fs;
    return fabs((double)c[0] / s - (double)face[0] / fs) < 0.025 && fabs((double)c[1] / s - (double)face[1] / fs) < 0.02 && 0.6 < q && q < 1.6;
}

/* The median (s[n // 2] of the sorted values) of 0..255 counts. */
static int median_hist(const int *hist, int nb, int n) {
    if (!n) return 0;
    int k = n / 2, acc = 0;
    for (int b = 0; b < nb; b++) {
        acc += hist[b];
        if (acc > k) return b;
    }
    return nb - 1;
}

/* Dicts {(x, y): position} in insertion order: at[texel] is the entry or -1. */
typedef struct {
    int *at, *key;
    double (*pos)[3];
    uint8_t *leg;
    int n;
} TexelMap;

static int tm_alloc(TexelMap *m, int N) {
    memset(m, 0, sizeof *m);
    m->at = malloc(sizeof(int) * (size_t)N), m->key = malloc(sizeof(int) * (size_t)N), m->pos = malloc(sizeof *m->pos * (size_t)N);
    m->leg = malloc((size_t)N);
    if (!m->at || !m->key || !m->pos || !m->leg) return -1;
    for (int i = 0; i < N; i++) m->at[i] = -1;
    return 0;
}
static void tm_free(TexelMap *m) { free(m->at), free(m->key), free(m->pos), free(m->leg); }
static void tm_clear(TexelMap *m) {
    for (int i = 0; i < m->n; i++) m->at[m->key[i]] = -1;
    m->n = 0;
}
static void tm_set(TexelMap *m, int k, const double *p, int leg) {
    int i = m->at[k];
    if (i < 0) i = m->at[k] = m->n++, m->key[i] = k;
    memcpy(m->pos[i], p, sizeof m->pos[i]);
    m->leg[i] = (uint8_t)leg;
}

/* raster_pos: the texels one island covers in UV (REPEAT), each with its bind-pose position. */
static void raster_pos(const Source *s, const Island *is, int w, int h, TexelMap *out) {
    for (int ti = 0; ti < is->n; ti++) {
        double u[3], v[3], P[3][3];
        for (int j = 0; j < 3; j++) {
            const uint8_t *p = vtx(s, vidx(s, 3 * is->tris[ti] + j));
            u[j] = rdf(p + 16), v[j] = rdf(p + 20);
            for (int k = 0; k < 3; k++) P[j][k] = rdf(p + 4 * k);
        }
        const double fu = floor(fmin(fmin(u[0], u[1]), u[2])), fv = floor(fmin(fmin(v[0], v[1]), v[2]));
        const double ax = (u[0] - fu) * w - 0.5, ay = (v[0] - fv) * h - 0.5, bx = (u[1] - fu) * w - 0.5, by = (v[1] - fv) * h - 0.5;
        const double cx = (u[2] - fu) * w - 0.5, cy = (v[2] - fv) * h - 0.5;
        const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (fabs(area) < 1e-12) continue;
        int x0 = (int)floor(fmin(fmin(ax, bx), cx)), x1 = (int)ceil(fmax(fmax(ax, bx), cx));
        int y0 = (int)floor(fmin(fmin(ay, by), cy)), y1 = (int)ceil(fmax(fmax(ay, by), cy));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        if (y1 > h - 1) y1 = h - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const double w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
                const double w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
                const double w2 = 1 - w0 - w1;
                if (fmin(fmin(w0, w1), w2) < -1e-6) continue;
                double p[3];
                for (int k = 0; k < 3; k++) p[k] = w0 * P[0][k] + w1 * P[1][k] + w2 * P[2][k];
                tm_set(out, y * w + x, p, 0);
            }
    }
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct {
    double p[3];
    int leg;
} Mark;
static int cmp_mark(const void *a, const void *b) {
    const Mark *x = a, *y = b;
    const int k = cmp3(x->p, y->p);
    return k ? k : x->leg - y->leg;
}

typedef struct {
    double c[3], rad, strength;
} Stain;
typedef struct {
    double c[3], d[3], half, width;
} Tear;

/* One skin: its cloth texels and their layers (build_skin). Returns 1 (layers in out[stage - 1]),
 * 0 (skipped: why), -1 (out of memory). */
static int cloth_skin(const Source *s, const Skin *k, const LayerP *P, int64_t seed, uint8_t *out[3], int *ncloth, const char **why) {
    const int w = k->w, h = k->h, N = w * h;
    const uint8_t *rgba = k->rgba;
    int rc = -1;
    TexelMap cov, legs, torso, cloth;
    uint8_t *skinm = NULL, *covm = NULL, *patch = NULL, *cand = NULL, *state = NULL;
    int *vs = NULL, *dist = NULL, *front = NULL, *next = NULL, *order = NULL;
    double *red = NULL, *edge = NULL, *vals = NULL, (*pre)[6] = NULL;
    Mark *marks = NULL;
    uint32_t *col = NULL;
    if (tm_alloc(&cov, N) || tm_alloc(&legs, N) || tm_alloc(&torso, N) || tm_alloc(&cloth, N)) goto done;
    skinm = calloc((size_t)N, 1), covm = malloc((size_t)N), patch = calloc((size_t)N, 1), cand = calloc((size_t)N, 1);
    dist = malloc(sizeof(int) * (size_t)N), front = malloc(sizeof(int) * (size_t)N), next = malloc(sizeof(int) * (size_t)N);
    if (!skinm || !covm || !patch || !cand || !dist || !front || !next) goto done;
    /* face_colour: the median colour of the largest head island */
    int face[3] = {150, 120, 100}, best = -1, best_n = -1;
    for (int i = 0; i < s->nisl; i++) {
        if (!(s->isl[i].zmin > P->head_above)) continue;
        Tris t = {(Source *)s, s->isl[i].tris, s->isl[i].n};
        raster(&t, w, h, 0, covm);
        int n = 0;
        for (int q = 0; q < N; q++) n += covm[q];
        if (n > best_n) best = i, best_n = n;
    }
    if (best >= 0) {
        Tris t = {(Source *)s, s->isl[best].tris, s->isl[best].n};
        raster(&t, w, h, 0, covm);
        for (int ch = 0; ch < 3; ch++) {
            int hist[256] = {0};
            for (int q = 0; q < N; q++)
                if (covm[q]) hist[rgba[4 * q + ch]]++;
            face[ch] = median_hist(hist, 256, best_n);
        }
    }
    /* cloth_texels */
    int maxv = 0;
    for (int i = 0; i < s->nisl; i++) maxv = s->isl[i].n * 3 > maxv ? s->isl[i].n * 3 : maxv;
    vs = malloc(sizeof(int) * (size_t)(maxv ? maxv : 1));
    vals = malloc(sizeof(double) * (size_t)(maxv ? maxv : 1));
    if (!vs || !vals) goto done;
    for (int i = 0; i < s->nisl; i++) {
        const Island *is = &s->isl[i];
        int nv = 0;
        for (int t = 0; t < is->n; t++)
            for (int j = 0; j < 3; j++) vs[nv++] = (int)vidx(s, 3 * is->tris[t] + j);
        qsort(vs, (size_t)nv, sizeof *vs, cmp_int);
        int u = 0;
        for (int q = 0; q < nv; q++)
            if (!u || vs[q] != vs[u - 1]) vs[u++] = vs[q];
        if (!u) continue;
        for (int q = 0; q < u; q++) vals[q] = rdf(vtx(s, (uint32_t)vs[q]) + 8);
        const double mz = py_sum(vals, u) / u;
        for (int q = 0; q < u; q++) vals[q] = fabs((double)rdf(vtx(s, (uint32_t)vs[q])));
        const double mx = py_sum(vals, u) / u;
        if (!(P->mz0 < mz && mz < P->mz1) || mx > P->abs_x) continue;
        raster_pos(s, is, w, h, &cov);
        if (mz >= P->legs_below && cov.n) {
            int sk = 0;
            for (int q = 0; q < cov.n; q++) sk += skin_like(rgba + 4 * cov.key[q], face);
            if (sk >= P->skin_island * cov.n) {
                tm_clear(&cov);
                continue;
            }
        }
        TexelMap *dst = mz < P->legs_below ? &legs : &torso;
        for (int q = 0; q < cov.n; q++) tm_set(dst, cov.key[q], cov.pos[q], 0);
        tm_clear(&cov);
    }
    for (int q = 0; q < torso.n; q++) skinm[torso.key[q]] = (uint8_t)skin_like(rgba + 4 * torso.key[q], face);
    for (int q = 0; q < legs.n; q++) tm_set(&cloth, legs.key[q], legs.pos[q], 1);
    const int nr = (int)P->near;
    for (int q = 0; q < torso.n; q++) {
        const int key = torso.key[q], x = key % w, y = key / w;
        if (skinm[key] || cloth.at[key] >= 0) continue;
        int near = 0, n = 0;
        for (int dy = -nr; dy <= nr; dy++)
            for (int dx = -nr; dx <= nr; dx++) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= w || qy >= h || torso.at[qy * w + qx] < 0) continue;
                n++;
                near += skinm[qy * w + qx];
            }
        if (near <= P->share * n) tm_set(&cloth, key, torso.pos[q], 0);
    }
    *ncloth = cloth.n;
    if (!cloth.n) {
        *why = "no clothing texels";
        rc = 0;
        goto done;
    }
    /* is_ghost, matches */
    {
        int64_t r = 0, b = 0;
        int lit = 0;
        for (int q = 0; q < cloth.n; q++) {
            const uint8_t *c = rgba + 4 * cloth.key[q];
            r += c[0], b += c[2];
            lit += c[0] + c[1] + c[2] > 40;
        }
        if (P->skip_ghost && (double)b / cloth.n > (double)r / cloth.n + 15) {
            *why = "ghost skin";
            rc = 0;
            goto done;
        }
        if ((double)lit / cloth.n < P->min_painted) {
            *why = "layout does not match";
            rc = 0;
            goto done;
        }
    }
    /* masks: red (hue protection), the vest's back patch, the garment's edge */
    red = malloc(sizeof(double) * (size_t)cloth.n), edge = calloc((size_t)cloth.n, sizeof(double));
    if (!red || !edge) goto done;
    {
        int ntorso = 0, nreds = 0;
        int hist[766] = {0};
        for (int q = 0; q < cloth.n; q++) {
            const uint8_t *c = rgba + 4 * cloth.key[q];
            red[q] = cloth.leg[q] ? 0.0 : red_weight(c);
            if (cloth.leg[q]) continue;
            ntorso++;
            if (red[q] > 0.5) nreds++, hist[c[0] + c[1] + c[2]]++;
        }
        if (ntorso && nreds >= P->red_share * ntorso) {
            const int msum = median_hist(hist, 766, nreds);
            for (int q = 0; q < cloth.n; q++) {
                if (cloth.leg[q]) continue;
                const uint8_t *c = rgba + 4 * cloth.key[q];
                double hh, sat, val;
                hsv(c, &hh, &sat, &val);
                if (((c[0] + c[1] + c[2]) > P->patch_bright * msum && red[q] < 0.5) || (0.06 < hh && hh < 0.19 && sat > 0.45 && val > 0.4))
                    cand[cloth.key[q]] = 1;
            }
            const int r = (int)fmax(1.0, nearbyint(2.0 * w / 512));
            const double need = 0.4 * ((2 * r + 1) * (2 * r + 1));
            for (int key = 0; key < N; key++) {
                if (!cand[key]) continue;
                const int x = key % w, y = key / w;
                int n = 0;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        const int qx = x + dx, qy = y + dy;
                        n += qx >= 0 && qy >= 0 && qx < w && qy < h && cand[qy * w + qx];
                    }
                if (n >= need) patch[key] = 1;
            }
            for (int g = 0; g < r; g++) {
                memcpy(covm, patch, (size_t)N);
                for (int key = 0; key < N; key++) {
                    if (!covm[key]) continue;
                    const int x = key % w, y = key / w;
                    const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                    for (int j = 0; j < 4; j++)
                        if (nb[j][0] >= 0 && nb[j][1] >= 0 && nb[j][0] < w && nb[j][1] < h && cloth.at[nb[j][1] * w + nb[j][0]] >= 0)
                            patch[nb[j][1] * w + nb[j][0]] = 1;
                }
            }
        }
        const int e = (int)fmax(2.0, nearbyint(P->edge * w / 512));
        int nf = 0;
        for (int i = 0; i < N; i++) dist[i] = -1;
        for (int q = 0; q < cloth.n; q++) {
            const int key = cloth.key[q], x = key % w, y = key / w;
            const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            int open = 0;
            for (int j = 0; j < 4 && !open; j++)
                open = nb[j][0] < 0 || nb[j][1] < 0 || nb[j][0] >= w || nb[j][1] >= h || cloth.at[nb[j][1] * w + nb[j][0]] < 0;
            if (open) front[nf++] = key, dist[key] = 0;
        }
        for (int step = 1; step <= e; step++) {
            int nn = 0;
            for (int f = 0; f < nf; f++) {
                const int x = front[f] % w, y = front[f] / w;
                const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                for (int j = 0; j < 4; j++) {
                    if (nb[j][0] < 0 || nb[j][1] < 0 || nb[j][0] >= w || nb[j][1] >= h) continue;
                    const int qk = nb[j][1] * w + nb[j][0];
                    if (cloth.at[qk] >= 0 && dist[qk] < 0) dist[qk] = step, next[nn++] = qk;
                }
            }
            memcpy(front, next, sizeof(int) * (size_t)nn);
            nf = nn;
        }
        for (int q = 0; q < cloth.n; q++)
            if (dist[cloth.key[q]] >= 0) edge[q] = 1.0 - dist[cloth.key[q]] / (e + 1.0);
    }
    /* plan_marks: stains and tears on clothing texels, placed in order */
    Stain stains[16];
    Tear tears[7];
    const int nst = (int)fmax(fmax(P->stains[0], P->stains[1]), P->stains[2]), ntr = (int)fmax(fmax(P->tears[0], P->tears[1]), P->tears[2]);
    marks = malloc(sizeof *marks * (size_t)cloth.n);
    if (!marks) goto done;
    {
        int nm = 0;
        for (int q = 0; q < cloth.n; q++)
            if (!patch[cloth.key[q]]) memcpy(marks[nm].p, cloth.pos[q], sizeof marks[nm].p), marks[nm++].leg = cloth.leg[q];
        if (!nm)
            for (int q = 0; q < cloth.n; q++) memcpy(marks[nm].p, cloth.pos[q], sizeof marks[nm].p), marks[nm++].leg = cloth.leg[q];
        qsort(marks, (size_t)nm, sizeof *marks, cmp_mark);
        /* legs first, then the torso, each sorted (stable partition of the sorted list) */
        Mark *lg = malloc(sizeof *lg * (size_t)nm), *to = malloc(sizeof *to * (size_t)nm);
        if (!lg || !to) {
            free(lg), free(to);
            goto done;
        }
        int nl = 0, nt = 0;
        for (int q = 0; q < nm; q++)
            if (marks[q].leg) lg[nl++] = marks[q];
            else to[nt++] = marks[q];
        XorShift rnd = {(uint32_t)(seed * 2654435761ll + 7)};
        if (!rnd.x) rnd.x = 1;
        for (int i = 0; i < nst; i++) {
            const Mark *pool = (i % 2 == 0 && nt) ? to : (nl ? lg : to);
            const int np = (i % 2 == 0 && nt) ? nt : (nl ? nl : nt);
            const Mark *cm = &pool[(int)(xs_next(&rnd) * np)];
            memcpy(stains[i].c, cm->p, sizeof stains[i].c);
            stains[i].rad = 0.04 + 0.05 * xs_next(&rnd);
            stains[i].strength = 0.6 + 0.4 * xs_next(&rnd);
        }
        Mark *cnd = malloc(sizeof *cnd * (size_t)nm);
        if (!cnd) {
            free(lg), free(to);
            goto done;
        }
        for (int i = 0; i < ntr; i++) {
            const int knee = i == 0 || i == 2 || i == 4 || i == 5;
            const int is_legs = (knee && nl) || (!(knee && nl) && !nt);
            const Mark *pool = is_legs ? lg : to;
            const int np = is_legs ? nl : nt;
            int nc = 0;
            for (int q = 0; q < np; q++) {
                const double z = pool[q].p[2];
                const int ok = is_legs ? (i == 0 || i == 2 ? (0.35 < z && z < 0.65) : z < 0.35) : (i == 1 || i == 3 ? z < 1.25 : 1);
                if (ok) cnd[nc++] = pool[q];
            }
            const Mark *from = nc ? cnd : pool;
            const int nf = nc ? nc : np;
            const Mark *cm = &from[(int)(xs_next(&rnd) * nf)];
            const double ang = xs_next(&rnd) * 3.141592653589793;
            double d[3] = {cos(ang), 0.35 * (xs_next(&rnd) - 0.5), sin(ang)};
            const double sq[3] = {d[0] * d[0], d[1] * d[1], d[2] * d[2]};
            const double n = sqrt(py_sum(sq, 3));
            memcpy(tears[i].c, cm->p, sizeof tears[i].c);
            for (int j = 0; j < 3; j++) tears[i].d[j] = d[j] / n;
            tears[i].half = 0.025 + 0.03 * xs_next(&rnd);
            tears[i].width = 0.006 + 0.006 * xs_next(&rnd);
        }
        free(cnd), free(lg), free(to);
    }
    /* the noise of each texel (the same at every stage) */
    pre = malloc(sizeof *pre * (size_t)cloth.n);
    col = malloc(sizeof(uint32_t) * (size_t)N);
    state = malloc((size_t)N);
    order = malloc(sizeof(int) * (size_t)N);
    if (!pre || !col || !state || !order) goto done;
    for (int q = 0; q < cloth.n; q++) {
        const double *p = cloth.pos[q];
        pre[q][0] = fbm(p, seed, 4, P->grime_scale);
        pre[q][1] = value_noise(p[0] * 90, p[1] * 90, p[2] * 90, seed + 5);
        pre[q][2] = fbm(p, seed + 99, 3, P->dust_scale);
        pre[q][3] = value_noise(p[0] * 55, p[1] * 55, p[2] * 55, seed + 23);
        pre[q][4] = ntr ? value_noise(p[0] * 140, p[1] * 140, p[2] * 140, seed + 11) : 0.0;
    }
    double under[3], dl[3], lut[256];   /* _LIN */
    for (int i = 0; i < 256; i++) lut[i] = to_lin(i);
    for (int i = 0; i < 3; i++) under[i] = to_lin(face[i] * 0.55), dl[i] = to_lin(P->dust_col[i]);
    const double tmax = fmax(fmax(P->tint[0], P->tint[1]), P->tint[2]);
    for (int si = 0; si < P->nstage; si++) {
        const int stage = P->stage[si], S = stage - 1;
        const double kk = P->spread[S], wear_f = darken(P->wear_amt[S]);
        uint8_t *o = out[si];
        memset(o, 0, (size_t)N * 4);
        memset(state, 0, (size_t)N);
        for (int q = 0; q < cloth.n; q++) {
            const int key = cloth.key[q], leg = cloth.leg[q];
            const double *p = cloth.pos[q];
            const uint8_t *c0 = rgba + 4 * key;
            double lin[3] = {lut[c0[0]], lut[c0[1]], lut[c0[2]]};
            const double rw = red[q], pk = patch[key] ? P->patch_keep : 1.0, ed = edge[q];
            const double n = pre[q][0], fine = pre[q][1];
            if (P->grime) {
                const double bias = leg ? (P->legs_bias[0] - p[2]) * P->legs_bias[1] : (P->torso_bias[0] - p[2]) * P->torso_bias[1];
                double d = clamp01((n + 0.3 * bias - (1.0 - 0.75 * kk)) * 3.0) * (0.6 + 0.4 * fine) * fmin(1.0, 1.1 * kk);
                if (stage == 1) d *= !leg ? ed : 0.6;
                const double e2 = ed * clamp01((n - 0.35) * 2.5) * (0.5 + 0.5 * fine) * (0.5 + 0.25 * stage);
                d = (e2 > d ? e2 : d) * pk;
                const double dark = darken(d * (P->dark[S] * rw + P->dark_cloth[S] * (1.0 - rw)));
                for (int i = 0; i < 3; i++) lin[i] = lin[i] * dark * (1.0 - (1.0 - P->tint[i] / tmax) * d * (1.0 - rw));
                to_grey(lin, P->desat[S] * d * rw, 0.0);
            }
            if (P->dust) {
                const double fd = pre[q][2];
                double dust = ed * clamp01((fd - 0.4) * 2.0) * P->dust_edge[S] * pk;
                if (leg) {
                    const double dl2 = clamp01((fd - 0.55) * 2.2) * clamp01(P->dust_below - p[2]) * kk * P->dust_legs;
                    if (dl2 > dust) dust = dl2;
                    for (int i = 0; i < 3; i++) lin[i] = lin[i] + (dl[i] - lin[i]) * dust;
                } else {
                    to_grey(lin, dust, 0.35);
                }
            }
            if (P->scuff) {
                const double scuff = clamp01((pre[q][3] - 0.78) * 6.0) * P->scuff_amt[S] * (0.4 + 0.6 * ed) * pk;
                to_grey(lin, scuff * 0.6, 0.25 * scuff);
            }
            if (!patch[key]) {
                for (int i = 0; i < (int)P->stains[S]; i++) {
                    const double dd = py_dist(p, stains[i].c) / (stains[i].rad * (0.8 + 0.4 * fine));
                    if (dd < 1.0) {
                        const double f = (0.25 + 0.25 * clamp01((dd - 0.7) * 4)) * stains[i].strength;
                        lin[0] = lin[0] * darken(f);
                        lin[1] = lin[1] * darken(f * (1 + 0.05 * (1 - rw)));
                        lin[2] = lin[2] * darken(f * (1 + 0.15 * (1 - rw)));
                    }
                }
                for (int i = 0; i < (int)P->tears[S]; i++) {
                    const Tear *t = &tears[i];
                    const double v[3] = {p[0] - t->c[0], p[1] - t->c[1], p[2] - t->c[2]};
                    double tt = v[0] * t->d[0] + v[1] * t->d[1] + v[2] * t->d[2];
                    tt = tt < -t->half ? -t->half : tt > t->half ? t->half : tt;
                    const double qv[3] = {v[0] - t->d[0] * tt, v[1] - t->d[1] * tt, v[2] - t->d[2] * tt};
                    const double dd = sqrt(qv[0] * qv[0] + qv[1] * qv[1] + qv[2] * qv[2]);
                    const double a = fabs(tt) / t->half;
                    const double taper = 1.0 - a * a;
                    const double wob = t->width * taper * (0.7 + 0.6 * pre[q][4]);
                    if (dd < wob) memcpy(lin, under, sizeof lin);
                    else if (dd < wob + 0.008) to_grey(lin, (0.35 + 0.45 * fine) * 0.6, 0.6);
                }
            }
            if (P->damp && stage == (int)P->damp_stage && !leg) {
                const double damp = clamp01((p[2] - P->damp_above) * 6) * clamp01((n - 0.35) * 2) * P->damp_amount * pk;
                const double f = darken(damp);
                for (int i = 0; i < 3; i++) lin[i] = lin[i] * f;
            }
            if (P->wear) {
                const double wr = P->wear_amt[S], f = wear_f;
                for (int i = 0; i < 3; i++) lin[i] = lin[i] * f;
                to_grey(lin, wr * 1.5 * (1 - rw), 0.0);
            }
            uint8_t px[3];
            for (int i = 0; i < 3; i++) px[i] = (uint8_t)(int)(to_srgb(lin[i]) + 0.5);
            col[key] = (uint32_t)px[0] | (uint32_t)px[1] << 8 | (uint32_t)px[2] << 16;
            state[key] = 1;
            order[q] = key;
        }
        /* layer_rgba: grown by `grow` texels in painting order (the first neighbour wins) */
        int ncur = cloth.n;
        for (int g = 0; g < (int)P->grow; g++) {
            const int start = ncur;
            for (int i = 0; i < start; i++) {
                const int key = order[i], x = key % w, y = key / w;
                const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                for (int j = 0; j < 4; j++) {
                    if (nb[j][0] < 0 || nb[j][1] < 0 || nb[j][0] >= w || nb[j][1] >= h) continue;
                    const int qk = nb[j][1] * w + nb[j][0];
                    if (state[qk]) continue;
                    state[qk] = 2, col[qk] = col[key], order[ncur++] = qk;
                }
            }
            for (int i = start; i < ncur; i++) state[order[i]] = 1;
        }
        for (int i = 0; i < ncur; i++) {
            const int key = order[i];
            o[4 * key] = (uint8_t)col[key], o[4 * key + 1] = (uint8_t)(col[key] >> 8), o[4 * key + 2] = (uint8_t)(col[key] >> 16);
            o[4 * key + 3] = 255;
        }
    }
    rc = 1;
done:
    tm_free(&cov), tm_free(&legs), tm_free(&torso), tm_free(&cloth);
    free(skinm), free(covm), free(patch), free(cand), free(vs), free(dist), free(front), free(next), free(order);
    free(red), free(edge), free(vals), free(pre), free(marks), free(col), free(state);
    return rc;
}

/* "{code}-{stage}" -> "cl-3". */
static void layer_label(const char *tmpl, const char *code, int stage, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = tmpl; *p && n + 1 < cap;) {
        if (!strncmp(p, "{code}", 6)) {
            n += (size_t)snprintf(out + n, cap - n, "%s", code), p += 6;
        } else if (!strncmp(p, "{stage}", 7)) {
            n += (size_t)snprintf(out + n, cap - n, "%d", stage), p += 7;
        } else {
            out[n++] = *p++;
        }
        if (n >= cap) n = cap - 1;
    }
    out[n] = 0;
}

typedef struct {
    uint32_t tex;
    int entry;
} Claim;

static int layer_section(Ctx *c, const MaSection *sec) {
    LayerP P;
    const Set *set;
    if (layer_params(c, sec, &P, &set)) return -1;
    if (disc_ready(c, sec->line)) return -1;
    for (const char *p = sec->name; *p; p++)
        if ((*p == '{' && strncmp(p, "{code}", 6) && strncmp(p, "{stage}", 7))) return ferr(c, sec->line, "a layer name holds only {code} and {stage}");
    /* claims: a skin texture belongs to the entry whose model keys (+ suffixes) name it, the first
     * in the set's order */
    Claim *claims = malloc(sizeof *claims * (size_t)(c->nrec ? c->nrec : 1));
    uint32_t *done = malloc(sizeof(uint32_t) * (size_t)(c->nrec + SET_MAX * SET_MODELS * MAX_SKINS + 1));
    Source *s = calloc(1, sizeof *s);
    uint8_t *lay[3] = {NULL, NULL, NULL};
    int nclaim = 0, ndone = 0, rc = 0, written = 0;
    if (!claims || !done || !s) {
        rc = ferr(c, sec->line, "out of memory");
        goto out;
    }
    for (int e = 0; !P.all_skins && e < set->n; e++)
        for (int m = 0; m < set->e[e].nm; m++)
            for (int x = -1; x < P.nsuffix; x++) {
                char key[96];
                snprintf(key, sizeof key, "%s%s", set->e[e].model[m], x < 0 ? "" : P.suffix[x]);
                const uint32_t hh = disc_model_hash(key);
                for (int r = 0; r < c->nrec; r++) {
                    if (c->recs[r].id != hh) continue;
                    int seen = 0;
                    for (int q = 0; q < nclaim && !seen; q++) seen = claims[q].tex == c->recs[r].tex;
                    if (!seen) claims[nclaim].tex = c->recs[r].tex, claims[nclaim++].entry = e;
                }
            }
    for (int e = 0; e < set->n && !rc; e++) {
        const SetEntry *en = &set->e[e];
        const int64_t seed = (int64_t)P.seed_a + (int64_t)P.seed_b * e;
        for (int m = 0; m < en->nm && !rc; m++) {
            source_free(s);
            memset(s, 0, sizeof *s);
            snprintf(s->model, sizeof s->model, "%s", en->model[m]);
            s->line = en->line;
            if (!source_load(c, s, en->line) || islands(c, s, en->line)) {
                rc = -1;
                break;
            }
            if (s->nskins == MAX_SKINS) {
                rc = ferr(c, en->line, "%s has more than %d skins", s->model, MAX_SKINS - 1);
                break;
            }
            for (int k = 0; k < s->nskins && !rc; k++) {
                const uint32_t tex = s->skins[k].tex;
                if (!find_chunk(c, DISC_CLASS_TEXTURE, tex, 0, 0)) continue;
                int seen = 0, owner = e;
                for (int q = 0; q < ndone && !seen; q++) seen = done[q] == tex;
                for (int q = 0; q < nclaim; q++)
                    if (claims[q].tex == tex) {
                        owner = claims[q].entry;
                        break;
                    }
                if (seen || owner != e) continue;
                done[ndone++] = tex;
                if (over_time(c, en->line)) {
                    rc = -1;
                    break;
                }
                Skin *sk = skin(c, s, k, en->line);
                if (!sk) {
                    rc = -1;
                    break;
                }
                for (int i = 0; i < P.nstage; i++) {
                    free(lay[i]);
                    lay[i] = malloc((size_t)sk->w * sk->h * 4);
                    if (!lay[i]) {
                        rc = ferr(c, en->line, "out of memory");
                        break;
                    }
                }
                if (rc) break;
                int ncloth = 0;
                const char *why = "";
                const int got = cloth_skin(s, sk, &P, seed, lay, &ncloth, &why);
                if (got < 0) {
                    rc = ferr(c, en->line, "out of memory");
                    break;
                }
                for (int i = 0; got && i < P.nstage && !rc; i++) {
                    char label[96], stem[128], path[1400];
                    layer_label(sec->name, en->code, P.stage[i], label, sizeof label);
                    snprintf(stem, sizeof stem, "%016llx@%s", (unsigned long long)sk->hash, label);
                    if (out_path(c, NULL, stem, ".png", path, sizeof path, sec->line) || write_png_counted(c, path, sk->w, sk->h, lay[i], sec->line))
                        rc = -1;
                    else written++;
                }
                free(sk->rgba);   /* one skin at a time in memory */
                sk->rgba = NULL;
            }
        }
    }
    if (!rc && !written) rc = ferr(c, sec->line, "no skin of set %s got a layer", set->name);
out:
    if (s) source_free(s);
    free(s), free(claims), free(done);
    for (int i = 0; i < 3; i++) free(lay[i]);
    return rc ? -1 : 0;
}

static int source_section(Ctx *c, const MaSection *sec) {
    if (c->nsrc == MAX_SOURCES) return ferr(c, sec->line, "more than %d sources", MAX_SOURCES);
    const MaLine *ch = NULL;
    int is_prop = 0, is_own = 0;
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        if ((!strcmp(l->key, "char") || !strcmp(l->key, "prop") || !strcmp(l->key, "file")) && !ch)
            ch = l, is_prop = l->key[0] == 'p', is_own = l->key[0] == 'f';
        else return ferr(c, l->line, "unknown key %s in [source %s] (one char = MODEL, prop = 0xHASH or file = own/NAME.glb)", l->key, sec->name);
    }
    uint32_t h;
    if (!ch || strlen(ch->value) >= 64)
        return ferr(c, sec->line, "[source %s] needs char = MODEL (a CfgChar model key or 0xHASH), prop = 0xHASH or file = own/NAME.glb", sec->name);
    if (is_prop && !hex_hash(ch->value, &h)) return ferr(c, ch->line, "prop = 0xHASH (the prop chunk's hash, 8 hex digits)");
    char why[200];
    if (is_own && ma_own_path_check(ch->value, why, sizeof why)) return ferr(c, ch->line, "%s", why);
    if (is_own && !ext_is(ch->value, ".glb")) return ferr(c, ch->line, "file = own/NAME.glb (a glTF 2.0 binary mesh)");
    Source *s = &c->src[c->nsrc++];
    memset(s, 0, sizeof *s);
    s->is_prop = is_prop, s->is_own = is_own;
    snprintf(s->name, sizeof s->name, "%s", sec->name);
    snprintf(s->model, sizeof s->model, "%s", ch->value);
    s->line = ch->line;
    return 0;
}

/* ---- [sound NAME]: the mod's own WAV / OGG, or a procedural sound, as <NAME>.wav ---- */

typedef struct {
    double f0, f1, seconds, attack, decay, noise, volume;
    int64_t seed;
} Synth;

/* synth = sweep F0 [F1] | tone F, seconds S, attack A, decay D, noise N, seed K, volume V */
static int synth_parse(Ctx *c, const MaLine *l, Synth *y) {
    char cl[10][160], w[4][160];
    const int nc = clauses(l->value, cl, 10);
    *y = (Synth){0, 0, 0.5, 0.003, 0.15, 0, 0.8, 1};
    int ok = nc >= 1, nw = ok ? words(cl[0], w, 4) : 0;
    if (ok && nw == 2 && !strcmp(w[0], "tone")) ok = num(w[1], &y->f0), y->f1 = y->f0;
    else if (ok && (nw == 2 || nw == 3) && !strcmp(w[0], "sweep")) ok = num(w[1], &y->f0) && (nw == 2 ? (y->f1 = y->f0, 1) : num(w[2], &y->f1));
    else ok = 0;
    for (int k = 1; ok && k < nc; k++) {
        ok = words(cl[k], w, 4) == 2;
        if (!ok) break;
        int sd = 0;
        if (!strcmp(w[0], "seconds")) ok = num(w[1], &y->seconds) && y->seconds > 0 && y->seconds <= 10;
        else if (!strcmp(w[0], "attack")) ok = num(w[1], &y->attack) && y->attack >= 0 && y->attack <= 10;
        else if (!strcmp(w[0], "decay")) ok = num(w[1], &y->decay) && y->decay >= 0;
        else if (!strcmp(w[0], "noise")) ok = num(w[1], &y->noise) && y->noise >= 0 && y->noise <= 1;
        else if (!strcmp(w[0], "volume")) ok = num(w[1], &y->volume) && y->volume >= 0 && y->volume <= 1;
        else if (!strcmp(w[0], "seed")) ok = inum(w[1], &sd), y->seed = sd;
        else ok = 0;
    }
    if (ok && (y->f0 < 20 || y->f0 > 12000 || y->f1 < 20 || y->f1 > 12000)) ok = 0;
    if (!ok)
        return ferr(c, l->line, "synth = sweep F0 F1 | tone F (20-12000 Hz), seconds S (at most 10), attack A, decay D, noise N (0-1), seed K, volume V (0-1)");
    return 0;
}

#define SYNTH_RATE 22050
/* One synth line added into out (frames at SYNTH_RATE): a sine gliding exponentially from f0 to
 * f1 plus low-passed white noise, under a linear attack and an exponential decay. */
static void synth_add(const Synth *y, float *out, int frames) {
    const int n = (int)(y->seconds * SYNTH_RATE);
    uint32_t x = (uint32_t)(y->seed * 2654435761u) | 1u;
    double ph = 0, lp = 0;
    const double a = 1.0 - exp(-2.0 * M_PI * 3000.0 / SYNTH_RATE);
    for (int i = 0; i < n && i < frames; i++) {
        const double t = (double)i / SYNTH_RATE, f = y->f0 * pow(y->f1 / y->f0, t / y->seconds);
        ph += 2.0 * M_PI * f / SYNTH_RATE;
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        lp += a * ((double)x / 2147483648.0 - 1.0 - lp);
        double env = y->attack > 0 && t < y->attack ? t / y->attack : 1.0;
        if (y->decay > 0) env *= exp(-t / y->decay);
        const double tail = (double)(n - i) / (0.005 * SYNTH_RATE);
        if (tail < 1) env *= tail;
        out[i] += (float)(y->volume * env * ((1.0 - y->noise) * sin(ph) + y->noise * 2.0 * lp));
    }
}

static int sound_section(Ctx *c, const MaSection *sec) {
    const MaLine *file = NULL;
    int nsynth = 0;
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        if (!strcmp(l->key, "file") && !file) file = l;
        else if (!strcmp(l->key, "synth")) nsynth++;
        else return ferr(c, l->line, "unknown key %s in [sound %s] (file = own/NAME.wav|ogg, or synth = lines)", l->key, sec->name);
    }
    if (!!file == !!nsynth) return ferr(c, sec->line, "[sound %s] needs file = own/NAME.wav|ogg or synth = ... (not both)", sec->name);
    int16_t *pcm = NULL;
    int ch = 1, rate = SYNTH_RATE, frames = 0;
    if (file) {
        if (!ext_is(file->value, ".wav") && !ext_is(file->value, ".ogg")) return ferr(c, file->line, "file = own/NAME.wav or own/NAME.ogg");
        size_t n;
        uint8_t *b = own_read(c, file->value, &n, file->line);
        if (!b) return -1;
        char why[300];
        int r = ma_sound_decode(b, n, &pcm, &ch, &rate, &frames, why, sizeof why);
        free(b);
        if (r) return ferr(c, file->line, "%s: %s", file->value, why);
    } else {
        Synth ys[8];
        int ny = 0;
        double secs = 0;
        for (int i = 0; i < sec->n; i++) {
            const MaLine *l = &c->r->lines[sec->first + i];
            if (ny == 8) return ferr(c, l->line, "at most 8 synth = lines");
            if (synth_parse(c, l, &ys[ny])) return -1;
            secs = fmax(secs, ys[ny++].seconds);
        }
        frames = (int)(secs * SYNTH_RATE);
        float *mix = calloc((size_t)frames, sizeof *mix);
        pcm = malloc(sizeof *pcm * (size_t)frames);
        if (!mix || !pcm) return free(mix), free(pcm), ferr(c, sec->line, "out of memory");
        for (int k = 0; k < ny; k++) synth_add(&ys[k], mix, frames);
        for (int i = 0; i < frames; i++) {
            const double v = mix[i] > 1 ? 1 : mix[i] < -1 ? -1 : mix[i];
            pcm[i] = (int16_t)lrint(v * 32767.0);
        }
        free(mix);
    }
    c->sound_bytes += (uint64_t)frames * (uint64_t)ch * 2;
    if (c->sound_bytes > MA_SOUND_BYTES_MAX) {
        free(pcm);
        return ferr(c, sec->line, "the mod's sounds take more than %u MiB decoded (budget)", MA_SOUND_BYTES_MAX >> 20);
    }
    char path[1400];
    int r = out_path(c, NULL, sec->name, ".wav", path, sizeof path, sec->line);
    if (!r && ma_wav_write(path, pcm, ch, rate, frames)) r = ferr(c, sec->line, "cannot write %s", path);
    free(pcm);
    return r ? -1 : counted(c, path, sec->line);
}

/* ---- [collision NAME]: an own mesh's collision as a patch of a level (<NAME>.col, src/colpatch.h) ---- */

static int collision_section(Ctx *c, const MaSection *sec) {
    const MaLine *from = NULL, *level = NULL, *at = NULL, *heading = NULL, *mat = NULL, *use = NULL;
    for (int i = 0; i < sec->n; i++) {
        const MaLine *l = &c->r->lines[sec->first + i];
        const MaLine **slot = !strcmp(l->key, "from") ? &from : !strcmp(l->key, "level") ? &level : !strcmp(l->key, "at") ? &at
                            : !strcmp(l->key, "heading") ? &heading : !strcmp(l->key, "material") ? &mat : !strcmp(l->key, "use") ? &use : NULL;
        if (!slot) return ferr(c, l->line, "unknown key %s in [collision %s]", l->key, sec->name);
        if (*slot) return ferr(c, l->line, "a second %s = line", l->key);
        *slot = l;
    }
    if (!from || !level || !at) return ferr(c, sec->line, "[collision %s] needs from = SOURCE, level = levelNN and at = X Y Z", sec->name);
    Source *s = source(c, from->value, from->line);
    if (!s) return -1;
    if (!s->is_own) return ferr(c, from->line, "from = your own mesh (a [source] file = own/NAME.glb)");
    int ok = !strncmp(level->value, "level", 5) && level->value[5] && strlen(level->value) < 16;
    for (const char *p = level->value + 5; ok && *p; p++) ok = *p >= '0' && *p <= '9';
    if (!ok) return ferr(c, level->line, "level = levelNN (the level's script name, e.g. level95 for the hideout)");
    char w[4][160];
    double pos[3], hd = 0;
    if (words(at->value, w, 4) != 3 || !num(w[0], &pos[0]) || !num(w[1], &pos[1]) || !num(w[2], &pos[2]))
        return ferr(c, at->line, "at = X Y Z (world metres, z up)");
    if (heading && !num(heading->value, &hd)) return ferr(c, heading->line, "heading = DEGREES (about the up axis, counter-clockwise seen from above)");
    int material = 1;
    if (mat && (!inum(mat->value, &material) || material < 0 || material > 255)) return ferr(c, mat->line, "material = N (0-255, the Lua MATERIAL id)");
    int whole = 0;
    if (use && strcmp(use->value, "collision") && !(whole = !strcmp(use->value, "mesh"))) return ferr(c, use->line, "use = collision or use = mesh");
    const MaOwnMesh *o = &s->own;
    if (!whole && !o->ncol) whole = 1;   /* no collision mesh: the visible one */
    const int nt = whole ? o->ni / 3 : o->ncol;
    if (nt > MA_COLLISION_TRIS_MAX)
        return ferr(c, sec->line, "%s: %d triangles of collision (at most %d): add a simpler mesh named collision", s->model, nt, MA_COLLISION_TRIS_MAX);
    char path[1400];
    if (out_path(c, NULL, sec->name, ".col", path, sizeof path, sec->line)) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return ferr(c, sec->line, "cannot write %s", path);
    const double ca = cos(hd * M_PI / 180.0), sa = sin(hd * M_PI / 180.0);
    fprintf(f, "; [collision %s] of %s (assets.ini), %s at %.4f %.4f %.4f heading %g\nlevel = %s\nmaterial = %d\nflags = 0x23\n", sec->name, s->model,
            whole ? "its mesh" : "its collision mesh", pos[0], pos[1], pos[2], hd, level->value, material);
    for (int t = 0; t < nt; t++) {
        fprintf(f, "tri");
        for (int k = 0; k < 3; k++) {
            const float *p = whole ? o->pos[o->idx[3 * t + k]] : o->col[t][k];
            fprintf(f, " %.4f %.4f %.4f", pos[0] + ca * p[0] - sa * p[1], pos[1] + sa * p[0] + ca * p[1], pos[2] + p[2]);
        }
        fprintf(f, "\n");
    }
    if (fclose(f)) return ferr(c, sec->line, "cannot write %s", path);
    return counted(c, path, sec->line);
}

int ma_build(MaRecipe *r, const char *disc, const char *out_dir, int *files, uint64_t *bytes, char *err, size_t cap) {
    Ctx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    r->warn[0] = 0;
    c->r = r, c->disc_dir = disc, c->out = out_dir, c->err = err, c->cap = cap, c->t0 = now_ms();
    int outs = 0, rc = 0;
    for (int i = 0; i < r->nsec && !rc; i++) {
        const MaSection *s = &r->sec[i];
        if (over_time(c, s->line)) rc = -1;
        else if (!strcmp(s->kind, "source")) rc = source_section(c, s);
        else if (!strcmp(s->kind, "texture")) rc = tex_section(c, s);
        else if (!strcmp(s->kind, "mesh")) rc = mesh_section(c, s), outs++;
        else if (!strcmp(s->kind, "hide")) rc = hide_section(c, s), outs++;
        else if (!strcmp(s->kind, "set")) rc = set_section(c, s);
        else if (!strcmp(s->kind, "layer")) rc = layer_section(c, s), outs++;
        else if (!strcmp(s->kind, "sound")) rc = sound_section(c, s), outs++;
        else if (!strcmp(s->kind, "collision")) rc = collision_section(c, s), outs++;
    }
    *files = c->files;
    *bytes = c->bytes;
    for (int i = 0; i < c->nsrc; i++) source_free(&c->src[i]);
    for (int i = 0; i < c->ntex; i++) free(c->tex[i].rgba), free(c->tex[i].mask);
    if (c->disc_state == 1)   /* the scan stays for the next mod; its open WAD files do not */
        for (int k = 0; k < 8; k++) {
            if (shared.d.fp[k]) fclose(shared.d.fp[k]);
            shared.d.fp[k] = NULL, shared.d.fpw[k] = -1;
        }
    free(c);
    return rc ? -1 : outs;
}
