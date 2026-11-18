/* Geometry-aware sound through Steam Audio: materials, the scene from the level's collision and
 * the acoustics thread (acoustics.h). */
#define _GNU_SOURCE
#include "acoustics.h"
#include "platform.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- materials (Steam Audio's standard presets, phonon.h IPLMaterial) ---- */
static const SaMaterial M_GENERIC = {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};
static const SaMaterial M_BRICK = {{0.03f, 0.04f, 0.07f}, 0.05f, {0.015f, 0.015f, 0.015f}};
static const SaMaterial M_CONCRETE = {{0.05f, 0.07f, 0.08f}, 0.05f, {0.015f, 0.002f, 0.001f}};
static const SaMaterial M_GRAVEL = {{0.60f, 0.70f, 0.80f}, 0.05f, {0.031f, 0.012f, 0.008f}};
static const SaMaterial M_GLASS = {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}};
static const SaMaterial M_PLASTER = {{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}};
static const SaMaterial M_WOOD = {{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}};
static const SaMaterial M_METAL = {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}};
/* chain-link fences and railings: mostly air (our own values) */
static const SaMaterial M_FENCE = {{0.02f, 0.02f, 0.02f}, 0.10f, {0.900f, 0.850f, 0.800f}};

void ac_material(uint8_t mat, SaMaterial *out) {
    switch (mat) {
    case 0:     /* the door panels */
        *out = M_WOOD;
        return;
    case 15:    /* DRYWALL */
        *out = M_PLASTER;
        return;
    case 107:   /* CHAINLINK_NOCLIMB */
    case 122:   /* RAILING */
    case 152:   /* CHAINMETAL */
        *out = M_FENCE;
        return;
    default:
        break;
    }
    switch (mat_class(mat)) {
    case MAT_ASPHALT:
    case MAT_CONCRETE: *out = M_CONCRETE; break;
    case MAT_DIRT:
    case MAT_GRASS: *out = M_GRAVEL; break;
    case MAT_WOOD: *out = M_WOOD; break;
    case MAT_METAL: *out = M_METAL; break;
    case MAT_BRICK: *out = M_BRICK; break;
    case MAT_GLASS: *out = M_GLASS; break;
    default: *out = M_GENERIC; break;
    }
}

int ac_tri_kept(uint8_t mat) { return mat != MAT_ID_CAMERA_GHOST; }
void ac_world_to_sa(const float w[3], float o[3]) { const float x = w[0], y = w[1], z = w[2]; o[0] = x, o[1] = z, o[2] = -y; }
void ac_audio_to_sa(const float a[3], float o[3]) { const float x = a[0], y = a[1], z = a[2]; o[0] = x, o[1] = y, o[2] = -z; }

static float one_pole(float fc) { return 1.0f - expf(-2.0f * 3.14159265f * fc / SA_RATE); }

/* The direct path's gain before its AC_GAIN_MIN floor and its response at 2.5 kHz (ac_direct_to_muffle). */
static void direct_parts(float occl, const float trans[3], float *gain, float *ratio) {
    const float o = occl < 0 ? 0 : occl > 1 ? 1 : occl;
    float e[3];
    for (int b = 0; b < 3; b++) {
        const float t = trans[b] < 0 ? 0 : trans[b] > 1 ? 1 : trans[b];
        e[b] = o + (1 - o) * t;
    }
    if (!(e[0] > 1e-6f)) e[0] = 1e-6f;
    *gain = powf(e[0], AC_GAIN_EXP);
    *ratio = powf(fminf(1.0f, e[1] / e[0]), AC_TONE_EXP) * (1.0f - (1.0f - AC_HIDDEN_TONE) * (1.0f - e[0]));
}

/* The one-pole whose response at 2.5 kHz is ratio; nearly flat: a bypass. */
static float ratio_to_lp(float ratio) {
    if (ratio > 0.98f) return 1.0f;
    /* one pole |H(f)| = 1 / sqrt(1 + (f / fc)^2) = ratio at 2.5 kHz */
    const float fc = fmaxf(AC_LP_MIN_HZ, 2500.0f * ratio / sqrtf(1.0f - ratio * ratio));
    return one_pole(fc);
}

void ac_direct_to_muffle(float occl, const float trans[3], float *gain, float *lp) {
    float g, ratio;
    direct_parts(occl, trans, &g, &ratio);
    *gain = fmaxf(AC_GAIN_MIN, g);
    *lp = ratio_to_lp(ratio);
}

void ac_mix_paths(float occl, const float trans[3], const SaPath *p, float d, const float dir_src[3], float *gain, float *lp, float dir[3]) {
    float gd, rd, gp = 0, rp = 1;
    direct_parts(occl, trans, &gd, &rd);
    memcpy(dir, dir_src, 3 * sizeof *dir);
    if (p && p->level > 0 && occl < AC_PATH_OCCL) {
        const float c = dir_src[0] * p->dir[0] + dir_src[1] * p->dir[1] + dir_src[2] * p->dir[2];
        const float eq0 = fminf(1.0f, fmaxf(0.0f, p->eq[0]));
        if (c < cosf(AC_PATH_SAME_DEG * 3.14159265f / 180) && eq0 > 1e-6f) {
            gp = fminf(1.0f, p->level * fmaxf(d, 1.0f)) * eq0;
            rp = fminf(1.0f, fmaxf(0.0f, p->eq[1]) / eq0);
        }
    }
    if (!(gp > 0)) {
        *gain = fmaxf(AC_GAIN_MIN, gd);
        *lp = ratio_to_lp(rd);
        return;
    }
    const float ed = gd * gd, ep = gp * gp, e = ed + ep;
    *gain = fminf(1.0f, fmaxf(AC_GAIN_MIN, sqrtf(e)));
    *lp = ratio_to_lp(sqrtf((ed * rd * rd + ep * rp * rp) / e));
    float v[3], l = 0;
    for (int k = 0; k < 3; k++) v[k] = ed * dir_src[k] + ep * p->dir[k], l += v[k] * v[k];
    l = sqrtf(l);
    if (l > 1e-9f && isfinite(l))
        for (int k = 0; k < 3; k++) dir[k] = v[k] / l;
}

int ac_reverb_to_i3dl2(const float rt60[3], float hf_ref, float *decay, float *hf_ratio) {
    if (!(rt60[0] > 0.05f) || !(rt60[1] > 0.05f) || !isfinite(rt60[0]) || !isfinite(rt60[1]) || !isfinite(rt60[2])) return -1;
    float d = 0.5f * (rt60[0] + rt60[1]);
    const float f = hf_ref > 2500 ? hf_ref : 2500, k = logf(f / 2500.0f) / logf(15000.0f / 2500.0f);
    const float hi = rt60[2] > 0 ? rt60[2] : 0, at = rt60[1] + (hi - rt60[1]) * (k > 1 ? 1 : k);
    float r = at / d;
    d = d < 0.1f ? 0.1f : d > 20 ? 20 : d;
    r = r < 0.1f ? 0.1f : r > 2 ? 2 : r;
    *decay = d;
    *hf_ratio = r;
    return 0;
}

/* ---- the shared state (lock) and the acoustics thread's own ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th;
static int started, threaded, running, reverb_on, pathing_on;
static char cache_dir[512];

typedef struct {
    uint8_t *buf;     /* tri then vert bytes */
    MatMesh m;
    uint32_t gen;
    char name[64];
} MeshCopy;
#define AC_DOORS_MAX 256
static struct {
    MeshCopy *mesh;              /* a new level's mesh, NULL: none pending */
    int mesh_new;
    uint32_t door_gen, door_n;
    uint16_t door[AC_DOORS_MAX];
    int door_new;
    struct { uint32_t key; float src[3]; double seen; int have, bent; float gain, lp, dir[3]; } slot[AC_MAX_SOURCES];
    float lis[3], ahead[3], up[3];
    int lis_have;
    float rt60[3];
    uint32_t rt60_seq;
    int has_scene;               /* the thread's scene exists: results may follow */
    /* --stats */
    uint64_t direct_ns, direct_runs, direct_src, reverb_ns, reverb_runs, path_ns, path_runs, path_src, path_bent;
    double build_ms;
    uint32_t build_tris;
    /* pathing: the bake thread's result for load baked_gen */
    SaProbes *baked;
    uint32_t baked_gen;
    int bake_done, baked_cached, probes_n, probes_cached;
} sh;

/* acoustics thread only */
static SaSim *sim;
static SaScene *scene;
static MeshCopy *mesh;
static uint16_t applied[AC_DOORS_MAX];
static uint32_t applied_n;
static double last_reverb;
static SaProbes *probes;    /* the current level's baked paths in the simulator */
static pthread_t bake_th;   /* the bake worker (threaded): started with the acoustics, waits for jobs */
static pthread_cond_t bake_cv = PTHREAD_COND_INITIALIZER;
static int bake_worker, bake_quit;   /* bake_quit, bake_job: under mu */
static struct BakeJob *bake_job;
static int bake_active;     /* a job handed over and not yet taken back (acoustics thread) */
static int bake_wanted;     /* the current mesh's bake waits for the running one (a newer load came in) */
static int bake_cancel;     /* atomic: ac_stop: skip a bake that has not started */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void mesh_free(MeshCopy *c) {
    if (c) free(c->buf);
    free(c);
}

void ac_set_mesh(const MatMesh *m, uint32_t gen, const char *name) {
    MeshCopy *c = NULL;
    if (m && m->ntri && m->nvert) {
        c = calloc(1, sizeof *c);
        if (c && (c->buf = malloc((size_t)m->ntri * 10 + (size_t)m->nvert * 16))) {
            memcpy(c->buf, m->tri, (size_t)m->ntri * 10);
            memcpy(c->buf + (size_t)m->ntri * 10, m->vert, (size_t)m->nvert * 16);
            c->m.tri = c->buf, c->m.ntri = m->ntri;
            c->m.vert = c->buf + (size_t)m->ntri * 10, c->m.nvert = m->nvert;
            c->gen = gen;
            snprintf(c->name, sizeof c->name, "%s", name ? name : "");
        } else {
            free(c);
            c = NULL;
        }
    }
    pthread_mutex_lock(&mu);
    mesh_free(sh.mesh);
    sh.mesh = c;
    sh.mesh_new = 1;
    sh.has_scene = 0;
    for (int i = 0; i < AC_MAX_SOURCES; i++) sh.slot[i].have = 0;
    pthread_mutex_unlock(&mu);
}

void ac_level(const char *disc, const char *level, uint32_t gen) {
    if (!started) return;
    char name[80];
    snprintf(name, sizeof name, "%s.lev", level ? level : "");
    size_t n = 0;
    uint8_t *b = disc && level && *level ? mat_wad_read(disc, name, &n) : NULL;
    MatMesh m;
    if (b && !mat_mesh_find(b, n, &m)) ac_set_mesh(&m, gen, level);
    else {
        printf("[acoustics] %s: no collision mesh on the disc: the wall map's occlusion and the title's reverb\n", level ? level : "?");
        ac_set_mesh(NULL, gen, level);
    }
    free(b);
}

void ac_doors(uint32_t gen, const uint16_t *open, uint32_t n) {
    if (!started) return;
    if (n > AC_DOORS_MAX) n = AC_DOORS_MAX;
    pthread_mutex_lock(&mu);
    if (gen != sh.door_gen || n != sh.door_n || memcmp(open, sh.door, n * sizeof *open)) {
        sh.door_gen = gen;
        sh.door_n = n;
        memcpy(sh.door, open, n * sizeof *open);
        sh.door_new = 1;
    }
    pthread_mutex_unlock(&mu);
}

int ac_query(uint32_t key, const float lis[3], const float src[3], float *gain, float *lp, float pos[3]) {
    if (!started || !key) return -1;
    float s[3] = {src[0], src[1], src[2]};
    if (s[1] < lis[1]) s[1] += fminf(AC_SOURCE_LIFT, lis[1] - s[1]);   /* footsteps on the ground: not under it */
    const double t = now_s();
    int r = -1;
    pthread_mutex_lock(&mu);
    int i = 0, old = 0;
    for (; i < AC_MAX_SOURCES; i++) {
        if (sh.slot[i].key == key) break;
        if (sh.slot[i].seen < sh.slot[old].seen) old = i;
    }
    if (i == AC_MAX_SOURCES) {   /* a new source takes the least recently queried slot */
        i = old;
        sh.slot[i].key = key;
        sh.slot[i].have = 0;
    }
    ac_audio_to_sa(s, sh.slot[i].src);
    ac_audio_to_sa(lis, sh.lis);
    sh.slot[i].seen = t;
    if (sh.slot[i].have && sh.has_scene) {
        *gain = sh.slot[i].gain;
        *lp = sh.slot[i].lp;
        if (pos && sh.slot[i].bent) {   /* the opening's direction at the source's distance */
            const float d[3] = {src[0] - lis[0], src[1] - lis[1], src[2] - lis[2]}, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float a[3];
            ac_audio_to_sa(sh.slot[i].dir, a);   /* the same flip back to audio axes */
            for (int k = 0; k < 3; k++) pos[k] = lis[k] + a[k] * l;
        } else if (pos) memcpy(pos, src, 3 * sizeof *pos);
        r = 0;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

void ac_listener(const float pos[3], const float front[3], const float top[3]) {
    if (!started) return;
    pthread_mutex_lock(&mu);
    ac_audio_to_sa(pos, sh.lis);
    ac_audio_to_sa(front, sh.ahead);
    ac_audio_to_sa(top, sh.up);
    sh.lis_have = 1;
    pthread_mutex_unlock(&mu);
}

uint32_t ac_reverb(float rt60[3]) {
    pthread_mutex_lock(&mu);
    const uint32_t q = sh.has_scene ? sh.rt60_seq : 0;
    memcpy(rt60, sh.rt60, sizeof sh.rt60);
    pthread_mutex_unlock(&mu);
    return q;
}

static int door_open(uint32_t t) {   /* binary search in the applied (increasing) list */
    uint32_t lo = 0, hi = applied_n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (applied[mid] == t) return 1;
        if (applied[mid] < t) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* A scene of mesh mc: without the doors open now (the applied list: acoustics thread only), or with
 * every door panel (MATERIAL 0) left out (all_open: the pathing bake, any thread). *ntri, *nmat; lo/hi
 * (optional) its bounds in Steam Audio's axes. */
static SaScene *scene_of(const MeshCopy *mc, int all_open, int *ntri, int *nmat_out, float lo[3], float hi[3]) {
    const uint32_t nt = mc->m.ntri;
    float (*v)[3] = malloc((size_t)nt * 3 * sizeof *v);
    int (*tri)[3] = malloc((size_t)nt * sizeof *tri), *mi = malloc((size_t)nt * sizeof *mi);
    SaMaterial mats[256];
    int mat_of[256], nmat = 0, n = 0;
    for (int i = 0; i < 256; i++) mat_of[i] = -1;
    if (lo) lo[0] = lo[1] = lo[2] = 1e30f, hi[0] = hi[1] = hi[2] = -1e30f;
    if (v && tri && mi)
        for (uint32_t t = 0; t < nt; t++) {
            float p[3][3];
            uint8_t mat;
            const int kind = mat_tri_read(&mc->m, t, p, &mat);
            if ((kind != 1 && kind != 2) || !ac_tri_kept(mat) || (all_open ? mat == 0 : door_open(t))) continue;
            if (mat_of[mat] < 0) { mat_of[mat] = nmat; ac_material(mat, &mats[nmat++]); }
            for (int k = 0; k < 3; k++) {
                ac_world_to_sa(p[k], v[3 * n + k]);
                tri[n][k] = 3 * n + k;
                if (lo)
                    for (int c = 0; c < 3; c++) lo[c] = fminf(lo[c], v[3 * n + k][c]), hi[c] = fmaxf(hi[c], v[3 * n + k][c]);
            }
            mi[n++] = mat_of[mat];
        }
    SaScene *sc = n ? sa_scene_new((const float (*)[3])v, 3 * n, (const int (*)[3])tri, mi, n, mats, nmat) : NULL;
    free(v);
    free(tri);
    free(mi);
    *ntri = n;
    *nmat_out = nmat;
    return sc;
}

/* The scene of the current mesh without the open doors (acoustics thread). */
static void scene_build(void) {
    sa_sim_set_scene(sim, NULL);
    sa_scene_free(scene);
    scene = NULL;
    if (!mesh) return;
    const double t0 = now_s();
    int n = 0, nmat = 0;
    scene = scene_of(mesh, 0, &n, &nmat, NULL, NULL);
    sa_sim_set_scene(sim, scene);
    const double ms = (now_s() - t0) * 1e3;
    pthread_mutex_lock(&mu);
    sh.has_scene = scene != NULL;
    sh.build_ms += ms;
    sh.build_tris = (uint32_t)n;
    pthread_mutex_unlock(&mu);
    printf("[acoustics] %s: scene of %d triangles (%d materials, %u doors open) built in %.1f ms%s\n", mesh->name, n, nmat, applied_n, ms,
           scene ? "" : ": failed, the wall map's occlusion stays");
}

/* ---- pathing bake (a low-priority thread per level; inline without the acoustics thread) ---- */
typedef struct BakeJob {
    MeshCopy *mesh;   /* its own copy: the acoustics thread may drop the level meanwhile */
    char path[700];   /* the cache file, "" for none */
} BakeJob;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static MeshCopy *mesh_dup(const MeshCopy *c) {
    MeshCopy *d = calloc(1, sizeof *d);
    const size_t n = (size_t)c->m.ntri * 10 + (size_t)c->m.nvert * 16;
    if (!d || !(d->buf = malloc(n))) { free(d); return NULL; }
    memcpy(d->buf, c->buf, n);
    d->m = c->m;
    d->m.tri = d->buf, d->m.vert = d->buf + (size_t)c->m.ntri * 10;
    d->gen = c->gen;
    memcpy(d->name, c->name, sizeof d->name);
    return d;
}

#define PATH_MAGIC "WRPATHS1"
static void cache_name(const MeshCopy *c, char *out, size_t n) {
    out[0] = 0;
    if (!cache_dir[0]) return;
    char cfg[160];
    snprintf(cfg, sizeof cfg, "%d %s %.2f %.2f %d %.2f %.2f %.1f %.1f", AC_PATH_CACHE_VERSION, sa_version(), AC_PATH_SPACING, AC_PATH_HEIGHT, AC_PATH_MAX_PROBES,
             AC_PATH_VIS_RADIUS, AC_PATH_VIS_THRESHOLD, AC_PATH_VIS_RANGE, AC_PATH_RANGE);
    uint64_t h = fnv(0xcbf29ce484222325ull, cfg, strlen(cfg));
    h = fnv(h, c->buf, (size_t)c->m.ntri * 10 + (size_t)c->m.nvert * 16);
    char nm[64];
    int k = 0;
    for (const char *q = c->name; *q && k < 40; q++) nm[k++] = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ? *q : '_';
    nm[k] = 0;
    snprintf(out, n, "%s/pathing-%s-%016llx.bin", cache_dir, k ? nm : "level", (unsigned long long)h);
}

static SaProbes *cache_read(const char *path) {
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    SaProbes *p = NULL;
    char magic[8];
    uint32_t n = 0;
    uint8_t *b = NULL;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, PATH_MAGIC, 8) && fread(&n, 4, 1, f) == 1 && n && n < (256u << 20) && (b = malloc(n)) &&
        fread(b, 1, n, f) == n)
        p = sa_probes_load(b, n);
    free(b);
    fclose(f);
    return p;
}

static void cache_write(const char *path, SaProbes *p) {
    if (!path[0]) return;
    uint8_t *b = NULL;
    const size_t n = sa_probes_save(p, &b);
    char tmp[720];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    plat_mkdir(cache_dir);
    FILE *f = n ? fopen(tmp, "wb") : NULL;
    const uint32_t n32 = (uint32_t)n;
    int ok = f && fwrite(PATH_MAGIC, 1, 8, f) == 8 && fwrite(&n32, 4, 1, f) == 1 && fwrite(b, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    remove(path);   /* Windows: rename does not replace */
    if (!ok || rename(tmp, path)) {
        remove(tmp);
        printf("[acoustics] cannot write the pathing cache %s: baked again next time\n", path);
    }
    free(b);
}

static void bake_run(BakeJob *j) {
    const double t0 = now_s();
    int cached = 1, n = 0, nmat = 0;
    float spacing = AC_PATH_SPACING;
    SaProbes *p = cache_read(j->path);
    if (!p) {
        cached = 0;
        float lo[3], hi[3];
        SaScene *sc = scene_of(j->mesh, 1, &n, &nmat, lo, hi);
        hi[1] += AC_PATH_HEIGHT + 1;
        if (sc) p = sa_probes_new(sc, lo, hi, spacing, AC_PATH_HEIGHT);
        while (p && sa_probes_count(p) > AC_PATH_MAX_PROBES) {   /* the bake's memory: N^2 */
            sa_probes_free(p);
            spacing *= 1.25f;
            p = sa_probes_new(sc, lo, hi, spacing, AC_PATH_HEIGHT);
        }
        const SaPathBake bk = {AC_PATH_VIS_RADIUS, AC_PATH_VIS_THRESHOLD, AC_PATH_VIS_RANGE, AC_PATH_RANGE, 1, 1};
        if (p && (__atomic_load_n(&bake_cancel, __ATOMIC_ACQUIRE) || sa_probes_bake(p, sc, &bk))) {
            sa_probes_free(p);
            p = NULL;
        }
        sa_scene_free(sc);
        if (p) cache_write(j->path, p);
    }
    const double ms = (now_s() - t0) * 1e3;
    if (p)
        printf("[acoustics] %s: sound around corners: %d probes%s, %s in %.0f ms, %zu bytes of paths\n", j->mesh->name, sa_probes_count(p),
               cached ? "" : spacing == AC_PATH_SPACING ? "" : " (spaced wider for memory)", cached ? "read from the cache" : "baked", ms, sa_probes_data_size(p));
    else if (!__atomic_load_n(&bake_cancel, __ATOMIC_ACQUIRE))
        printf("[acoustics] %s: no probes for sound around corners: occlusion only\n", j->mesh->name);
    pthread_mutex_lock(&mu);
    sa_probes_free(sh.baked);
    sh.baked = p;   /* for load baked_gen: dropped if another level came meanwhile */
    sh.baked_gen = j->mesh->gen;
    sh.baked_cached = cached;
    sh.bake_done = 1;
    pthread_mutex_unlock(&mu);
    mesh_free(j->mesh);
    free(j);
}

/* The bake worker. It is created with the acoustics thread at start-up: a bake run on a thread created
 * later, while the title plays, crashed inside Steam Audio's own worker (a job's callback overwritten;
 * not seen outside the game, nor with the same bake on the acoustics thread). A bake is never
 * cancelled either: 4.8.1's iplPathBakerCancelBake returns from the bake while its workers may still
 * run jobs on the bake's freed locals (a crash with two loads in a row), so a superseded bake finishes
 * (about 1.5 s for a large level) and its result is dropped. */
static void *bake_main(void *arg) {
    (void)arg;
    plat_thread_low_priority();
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!bake_quit && !bake_job) pthread_cond_wait(&bake_cv, &mu);
        if (bake_quit) break;
        BakeJob *j = bake_job;
        bake_job = NULL;
        pthread_mutex_unlock(&mu);
        bake_run(j);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* The current mesh's paths: from the cache or baked (acoustics thread); after the running bake when
 * one is still busy with an earlier level. */
static void bake_start(void) {
    if (!pathing_on || !mesh) return;
    if (bake_active) { bake_wanted = 1; return; }
    bake_wanted = 0;
    BakeJob *j = calloc(1, sizeof *j);
    if (!j || !(j->mesh = mesh_dup(mesh))) { free(j); return; }
    cache_name(mesh, j->path, sizeof j->path);
    if (!bake_worker) { bake_run(j); return; }
    pthread_mutex_lock(&mu);
    bake_job = j;
    pthread_cond_signal(&bake_cv);
    pthread_mutex_unlock(&mu);
    bake_active = 1;
}

/* Takes the bake's result into the simulator when it is for the current mesh (acoustics thread). */
static void bake_take(void) {
    pthread_mutex_lock(&mu);
    SaProbes *p = NULL;
    int done = sh.bake_done, cached = sh.baked_cached;
    SaProbes *stale = NULL;
    if (sh.baked && mesh && sh.baked_gen == mesh->gen) p = sh.baked;
    else stale = sh.baked;
    sh.baked = NULL;
    sh.bake_done = 0;
    pthread_mutex_unlock(&mu);
    sa_probes_free(stale);
    if (done) bake_active = 0;
    if (done && bake_wanted) bake_start();   /* the level that came in while it ran */
    if (!p) return;
    sa_sim_set_probes(sim, p);
    sa_probes_free(probes);
    probes = p;
    pthread_mutex_lock(&mu);
    sh.probes_n = sa_probes_count(p);
    sh.probes_cached = cached;
    pthread_mutex_unlock(&mu);
}

static void drop_probes(void) {
    sa_sim_set_probes(sim, NULL);
    sa_probes_free(probes);
    probes = NULL;
    pthread_mutex_lock(&mu);
    sh.probes_n = 0;
    pthread_mutex_unlock(&mu);
}

void ac_set_cache_dir(const char *dir) { snprintf(cache_dir, sizeof cache_dir, "%s", dir ? dir : ""); }

int ac_pathing_probes(int *from_cache) {
    pthread_mutex_lock(&mu);
    const int n = sh.probes_n;
    if (from_cache) *from_cache = sh.probes_cached;
    pthread_mutex_unlock(&mu);
    return n;
}

void ac_step(void) {
    if (!started || !sim) return;
    MeshCopy *newmesh = NULL;
    int took_mesh = 0, took_doors = 0;
    uint32_t dgen = 0, dn = 0;
    uint16_t dl[AC_DOORS_MAX];
    float lis[3], ahead[3], up[3], src[AC_MAX_SOURCES][3];
    int idx[AC_MAX_SOURCES], n = 0, lis_have;
    uint32_t keys[AC_MAX_SOURCES];
    const double t = now_s();
    pthread_mutex_lock(&mu);
    if (sh.mesh_new) { newmesh = sh.mesh; sh.mesh = NULL; sh.mesh_new = 0; took_mesh = 1; }
    if (sh.door_new) { dgen = sh.door_gen; dn = sh.door_n; memcpy(dl, sh.door, dn * sizeof *dl); sh.door_new = 0; took_doors = 1; }
    for (int i = 0; i < AC_MAX_SOURCES; i++)
        if (sh.slot[i].key && t - sh.slot[i].seen < AC_STALE_S) { memcpy(src[n], sh.slot[i].src, sizeof src[n]); keys[n] = sh.slot[i].key; idx[n++] = i; }
    memcpy(lis, sh.lis, sizeof lis);
    memcpy(ahead, sh.ahead, sizeof ahead);
    memcpy(up, sh.up, sizeof up);
    lis_have = sh.lis_have;
    pthread_mutex_unlock(&mu);

    if (took_mesh) {
        if (pathing_on) drop_probes();
        mesh_free(mesh);
        mesh = newmesh;
        applied_n = 0;
        if (took_doors && mesh && dgen == mesh->gen) { memcpy(applied, dl, dn * sizeof *dl); applied_n = dn; }
        scene_build();
        if (scene) bake_start();
    } else if (took_doors && mesh && dgen == mesh->gen && (dn != applied_n || memcmp(dl, applied, dn * sizeof *dl))) {
        memcpy(applied, dl, dn * sizeof *dl);
        applied_n = dn;
        scene_build();
    }
    if (pathing_on) bake_take();
    if (!scene) return;

    if (n) {
        float occl[AC_MAX_SOURCES], trans[AC_MAX_SOURCES][3], g[AC_MAX_SOURCES], lp[AC_MAX_SOURCES], dir[AC_MAX_SOURCES][3];
        int bent[AC_MAX_SOURCES] = {0}, np = 0, pk[AC_MAX_SOURCES];
        struct timespec a, b, c;
        clock_gettime(CLOCK_MONOTONIC, &a);
        sa_sim_direct(sim, lis, (const float (*)[3])src, n, AC_SOURCE_RADIUS, occl, trans);
        clock_gettime(CLOCK_MONOTONIC, &b);
        c = b;
        if (probes) {   /* the hidden ones: paths around what hides them */
            float ps[AC_MAX_SOURCES][3];
            SaPath path[AC_MAX_SOURCES];
            for (int k = 0; k < n; k++)
                if (occl[k] < AC_PATH_OCCL) { memcpy(ps[np], src[k], sizeof ps[np]); pk[np++] = k; }
            const SaPathQuery q = {AC_PATH_VIS_RADIUS, AC_PATH_VIS_THRESHOLD, AC_PATH_VIS_RANGE, 1};
            if (np) sa_sim_pathing(sim, lis, (const float (*)[3])ps, np, &q, path);
            clock_gettime(CLOCK_MONOTONIC, &c);
            for (int k = 0, j = 0; k < n; k++) {
                const float v[3] = {src[k][0] - lis[0], src[k][1] - lis[1], src[k][2] - lis[2]}, d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const float ds[3] = {d > 1e-6f ? v[0] / d : 0, d > 1e-6f ? v[1] / d : 0, d > 1e-6f ? v[2] / d : -1};
                const SaPath *pp = j < np && pk[j] == k ? &path[j++] : NULL;
                ac_mix_paths(occl[k], trans[k], pp, d, ds, &g[k], &lp[k], dir[k]);
                bent[k] = dir[k][0] != ds[0] || dir[k][1] != ds[1] || dir[k][2] != ds[2];
            }
        } else
            for (int k = 0; k < n; k++) ac_direct_to_muffle(occl[k], trans[k], &g[k], &lp[k]);
        pthread_mutex_lock(&mu);
        for (int k = 0; k < n; k++) {
            const int i = idx[k];
            if (sh.slot[i].key == keys[k]) {   /* not taken over by another source meanwhile */
                sh.slot[i].gain = g[k];
                sh.slot[i].lp = lp[k];
                sh.slot[i].bent = bent[k];
                if (bent[k]) memcpy(sh.slot[i].dir, dir[k], sizeof dir[k]);
                sh.slot[i].have = 1;
                sh.path_bent += (uint64_t)bent[k];
            }
        }
        sh.direct_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
        sh.direct_runs++;
        sh.direct_src += (uint64_t)n;
        if (np) {
            sh.path_ns += (uint64_t)((c.tv_sec - b.tv_sec) * 1000000000ll + (c.tv_nsec - b.tv_nsec));
            sh.path_runs++;
            sh.path_src += (uint64_t)np;
        }
        pthread_mutex_unlock(&mu);
    }
    if (reverb_on && lis_have && (!threaded || t - last_reverb >= AC_REVERB_PERIOD_S)) {
        last_reverb = t;
        float rt[3];
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        const int ok = !sa_sim_reverb(sim, lis, ahead, up, AC_REVERB_BOUNCES, rt);
        clock_gettime(CLOCK_MONOTONIC, &b);
        pthread_mutex_lock(&mu);
        if (ok) { memcpy(sh.rt60, rt, sizeof rt); sh.rt60_seq++; }
        sh.reverb_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
        sh.reverb_runs++;
        pthread_mutex_unlock(&mu);
    }
}

static int stats_on;
void ac_set_stats(int on) { stats_on = on != 0; }
static double cpu_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void *thread_main(void *arg) {
    (void)arg;
    double t0 = now_s(), c0 = cpu_s();
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        ac_step();
        nanosleep(&(struct timespec){0, 16 * 1000 * 1000}, NULL);   /* about one title frame */
        if (stats_on && now_s() - t0 >= 5.0) {
            char buf[512];
            const double t1 = now_s(), c1 = cpu_s();
            ac_stats(buf, sizeof buf);
            printf("%s; thread %.2f%% of a core\n", buf, 100.0 * (c1 - c0) / (t1 - t0));
            t0 = t1, c0 = c1;
        }
    }
    return NULL;
}

int ac_start(int thr, int reverb, int pathing) {
    if (started) return 0;
    if (!sa_ok()) return -1;
    pathing_on = pathing != 0;
    sim = sa_sim_new(AC_MAX_SOURCES, AC_REVERB_RAYS, pathing_on);
    if (!sim) { printf("[acoustics] Steam Audio simulator creation failed: the wall map's occlusion stays\n"); return -1; }
    memset(&sh, 0, sizeof sh);
    reverb_on = reverb != 0;
    threaded = thr != 0;
    started = 1;
    if (threaded) {
        running = 1;
        if (pthread_create(&th, NULL, thread_main, NULL)) {
            running = 0;
            started = 0;
            sa_sim_free(sim);
            sim = NULL;
            printf("[acoustics] cannot start the acoustics thread: the wall map's occlusion stays\n");
            return -1;
        }
        if (pathing_on) {
            bake_worker = !pthread_create(&bake_th, NULL, bake_main, NULL);
            if (!bake_worker) printf("[acoustics] cannot start the pathing bake thread: paths are baked on the acoustics thread\n");
        }
    }
    return 0;
}

int ac_running(void) { return started; }

void ac_stop(void) {
    if (!started) return;
    if (threaded) {
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        pthread_join(th, NULL);
    }
    started = 0;
    if (bake_worker) {   /* waits for a bake in progress; one handed over and not started is skipped */
        __atomic_store_n(&bake_cancel, 1, __ATOMIC_RELEASE);
        pthread_mutex_lock(&mu);
        bake_quit = 1;
        if (bake_job) { mesh_free(bake_job->mesh); free(bake_job); bake_job = NULL; }
        pthread_cond_signal(&bake_cv);
        pthread_mutex_unlock(&mu);
        pthread_join(bake_th, NULL);
        bake_worker = bake_quit = 0;
        __atomic_store_n(&bake_cancel, 0, __ATOMIC_RELEASE);
    }
    bake_active = bake_wanted = 0;
    pthread_mutex_lock(&mu);
    sa_probes_free(sh.baked);
    sh.baked = NULL;
    sh.bake_done = 0;
    pthread_mutex_unlock(&mu);
    drop_probes();
    sa_sim_set_scene(sim, NULL);
    sa_scene_free(scene);
    scene = NULL;
    sa_sim_free(sim);
    sim = NULL;
    mesh_free(mesh);
    mesh = NULL;
    applied_n = 0;
    pthread_mutex_lock(&mu);
    mesh_free(sh.mesh);
    sh.mesh = NULL;
    pthread_mutex_unlock(&mu);
}

void ac_stats(char *buf, int n) {
    pthread_mutex_lock(&mu);
    const double dus = sh.direct_runs ? sh.direct_ns / 1e3 / sh.direct_runs : 0, per = sh.direct_src ? sh.direct_ns / 1e3 / sh.direct_src : 0;
    const double rms = sh.reverb_runs ? sh.reverb_ns / 1e6 / sh.reverb_runs : 0;
    const int k = snprintf(buf, (size_t)n,
             "[stats] acoustics: %llu direct runs, %.1f sources each, %.1f us per run (%.2f us per source); reverb %llu runs, %.2f ms each; scene %u triangles",
             (unsigned long long)sh.direct_runs, sh.direct_runs ? (double)sh.direct_src / sh.direct_runs : 0, dus, per,
             (unsigned long long)sh.reverb_runs, rms, sh.build_tris);
    if (pathing_on && k > 0 && k < n)
        snprintf(buf + k, (size_t)(n - k), "; pathing %d probes%s, %.1f hidden sources per run, %.2f us per source, %llu results bent", sh.probes_n,
                 sh.probes_cached ? " (cache)" : "", sh.path_runs ? (double)sh.path_src / sh.path_runs : 0, sh.path_src ? sh.path_ns / 1e3 / sh.path_src : 0,
                 (unsigned long long)sh.path_bent);
    sh.direct_ns = sh.direct_runs = sh.direct_src = sh.reverb_ns = sh.reverb_runs = 0;
    sh.path_ns = sh.path_runs = sh.path_src = sh.path_bent = 0;
    pthread_mutex_unlock(&mu);
}
