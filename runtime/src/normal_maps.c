/* --normal-maps: see normal_maps.h. */
#include "normal_maps.h"
#include "texpack.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    float f = (float)strtod(v, &end);
    if (*end || !(f >= lo && f <= hi)) return -1;
    *out = f;
    return 0;
}

int normal_maps_arg(NormalMapsConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--normal-maps") || !strcmp(opt, "--no-normal-maps")) {
        c->on = opt[2] == 'n' && opt[3] == 'o' && opt[4] == 'r';
        return 1;
    }
    if (!strcmp(opt, "--normal-strength")) return parse_float(val, 0.0f, NORMAL_MAPS_STRENGTH_MAX, &c->normal_strength) ? -1 : 2;
    if (!strcmp(opt, "--specular-strength")) return parse_float(val, 0.0f, NORMAL_MAPS_STRENGTH_MAX, &c->specular_strength) ? -1 : 2;
    if (!strcmp(opt, "--normal-baked")) return parse_float(val, 0.0f, 1.0f, &c->baked) ? -1 : 2;
    if (!strcmp(opt, "--dry-highlights") || !strcmp(opt, "--no-dry-highlights")) {
        c->dry = opt[2] == 'd';
        return 1;
    }
    if (!strcmp(opt, "--dry-highlights-strength")) return parse_float(val, NORMAL_MAPS_DRY_MIN, NORMAL_MAPS_DRY_MAX, &c->dry_strength) ? -1 : 2;
    if (!strcmp(opt, "--parallax") || !strcmp(opt, "--no-parallax")) {
        c->parallax = opt[2] == 'p';
        return 1;
    }
    if (!strcmp(opt, "--parallax-depth")) return parse_float(val, NORMAL_MAPS_PARALLAX_MIN, NORMAL_MAPS_PARALLAX_MAX, &c->parallax_depth) ? -1 : 2;
    return 0;
}

/* ---- names ---- */
static const char SUFFIX[NORMAL_MAPS_KINDS] = {'n', 's'};

int normal_maps_name(uint64_t hash, int kind, int dds, char *buf, size_t n) {
    if (kind < 0 || kind >= NORMAL_MAPS_KINDS || n < 23) return -1;   /* 16 + "_n.png" + NUL */
    snprintf(buf, n, "%016llx_%c.%s", (unsigned long long)hash, SUFFIX[kind], dds ? "dds" : "png");
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int parse_hex16(const char *s, uint64_t *hash) {
    uint64_t h = 0;
    for (int i = 0; i < 16; i++) {
        int c = lower(s[i]);
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        h = h << 4 | (uint64_t)d;
    }
    *hash = h;
    return 0;
}

/* ".png" -> 0, ".dds" or ".ktx2" -> 1 (the BC file; any case, then the end), else -1. */
static int parse_ext(const char *e) {
    if (e[0] != '.' || !e[1] || !e[2] || !e[3]) return -1;
    char x[5] = {(char)lower(e[1]), (char)lower(e[2]), (char)lower(e[3]), e[4] ? (char)lower(e[4]) : 0, 0};
    if (e[4] && e[5]) return -1;
    return !strcmp(x, "png") ? 0 : !strcmp(x, "dds") ? 1 : !strcmp(x, "ktx2") ? 1 : -1;
}

/* A BC file name ending in .ktx2 (a .dds of the same map in the same pack wins over it). */
static int is_ktx2(const char *name) {
    const size_t n = strlen(name);
    return n > 5 && name[n - 5] == '.' && lower(name[n - 4]) == 'k' && lower(name[n - 3]) == 't' && lower(name[n - 2]) == 'x' &&
           name[n - 1] == '2';
}

int normal_maps_parse_name(const char *name, uint64_t *hash, int *kind, int *dds) {
    uint64_t h;
    if (parse_hex16(name, &h) || name[16] != '_') return -1;
    const int s = lower(name[17]);
    const int k = s == 'n' ? NORMAL_MAPS_RELIEF : s == 's' ? NORMAL_MAPS_SPECULAR : -1;
    const int d = k < 0 ? -1 : parse_ext(name + 18);
    if (d < 0) return -1;
    *hash = h;
    *kind = k;
    *dds = d;
    return 0;
}

int normal_maps_companion(const char *base, int kind, char *buf, size_t n) {
    if (kind < 0 || kind >= NORMAL_MAPS_KINDS) return -1;
    const char *slash = strrchr(base, '/');
    const char *file = slash ? slash + 1 : base;
    uint64_t h;
    if (strlen(file) != 20 || parse_hex16(file, &h) || parse_ext(file + 16) < 0) return -1;
    const size_t dir = (size_t)(file - base);
    if (n < dir + 23) return -1;
    memcpy(buf, base, dir + 16);
    buf[dir + 16] = '_';
    buf[dir + 17] = SUFFIX[kind];
    memcpy(buf + dir + 18, file + 16, 5);   /* the extension as given, with its NUL */
    return 0;
}

/* ---- index: entries sorted by (hash, kind) ---- */
typedef struct {
    uint64_t hash;
    int kind;
    int height;       /* relief maps: normal_maps_set_height (-1 unknown) */
    int glossy;       /* specular maps: normal_maps_set_glossy (-1 unknown) */
    int dir[2];       /* pack of the .png [0] and the .dds [1]: the first pack with one; -1 none */
    char name[2][24]; /* as found (case kept) */
} Entry;
static struct {
    char **dirs;
    int ndirs;
    Entry *e;
    size_t n, cap;
    int dds;
} ix;

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return (x->kind > y->kind) - (x->kind < y->kind);
}

/* Binary search over the first n entries (sorted). */
static Entry *find_n(uint64_t hash, int kind, size_t n) {
    size_t lo = 0, hi = n;
    const Entry key = {.hash = hash, .kind = kind};
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cmp_entry(&ix.e[mid], &key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && !cmp_entry(&ix.e[lo], &key) ? &ix.e[lo] : NULL;
}
static Entry *find(uint64_t hash, int kind) { return find_n(hash, kind, ix.n); }

/* The file an entry offers: 1 its .dds, 0 its .png, -1 none. The .dds while offered and from a pack
 * no later than the .png's. */
static int offered(const Entry *e) {
    if (ix.dds && e->dir[1] >= 0 && (e->dir[0] < 0 || e->dir[1] <= e->dir[0])) return 1;
    return e->dir[0] >= 0 ? 0 : -1;
}

static int push(const Entry *e) {
    if (ix.n == ix.cap) {
        size_t c = ix.cap ? ix.cap * 2 : 64;
        Entry *ne = realloc(ix.e, c * sizeof *ne);
        if (!ne) return -1;
        ix.e = ne;
        ix.cap = c;
    }
    ix.e[ix.n++] = *e;
    return 0;
}

int normal_maps_index_add(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **nd = realloc(ix.dirs, (size_t)(ix.ndirs + 1) * sizeof *nd);
    char *copy = strdup(dir);
    if (!nd || !copy) {
        if (nd) ix.dirs = nd;
        free(copy);
        closedir(d);
        return -1;
    }
    ix.dirs = nd;
    const int di = ix.ndirs++;
    ix.dirs[di] = copy;
    /* This pack's files, gathered and sorted first, then merged under the earlier packs. */
    Entry *pk = NULL;
    size_t pn = 0, pcap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        uint64_t h;
        int kind, dds;
        if (normal_maps_parse_name(de->d_name, &h, &kind, &dds)) continue;
        if (pn == pcap) {
            size_t c = pcap ? pcap * 2 : 64;
            Entry *np = realloc(pk, c * sizeof *np);
            if (!np) break;
            pk = np;
            pcap = c;
        }
        Entry *e = &pk[pn++];
        memset(e, 0, sizeof *e);
        e->hash = h;
        e->kind = kind;
        e->height = -1;
        e->glossy = -1;
        e->dir[0] = e->dir[1] = -1;
        e->dir[dds] = di;
        snprintf(e->name[dds], sizeof e->name[dds], "%s", de->d_name);
    }
    closedir(d);
    if (pn) qsort(pk, pn, sizeof *pk, cmp_entry);
    const size_t before = ix.n;
    int added = 0;
    for (size_t i = 0; i < pn; i++) {
        Entry *p = &pk[i];
        /* the other file of the same (hash, kind) in this pack: merged into the previous entry */
        if (i && !cmp_entry(p, &pk[i - 1])) {
            Entry *q = &pk[i - 1];
            for (int t = 0; t < 2; t++)
                if (p->dir[t] >= 0 && (q->dir[t] < 0 || (t == 1 && is_ktx2(q->name[1]) && !is_ktx2(p->name[1])))) {
                    q->dir[t] = p->dir[t];
                    memcpy(q->name[t], p->name[t], sizeof q->name[t]);
                }
            *p = *q;   /* the merged entry moves on, so a third duplicate merges into it too */
            q->dir[0] = q->dir[1] = -2;   /* consumed */
        }
    }
    for (size_t i = 0; i < pn; i++) {
        Entry *p = &pk[i];
        if (p->dir[0] == -2) continue;
        Entry *e = find_n(p->hash, p->kind, before);   /* earlier packs: the sorted part */
        if (e) {   /* an earlier pack has it: this pack only fills the file type it lacks */
            for (int t = 0; t < 2; t++)
                if (p->dir[t] >= 0 && e->dir[t] < 0) {
                    e->dir[t] = p->dir[t];
                    memcpy(e->name[t], p->name[t], sizeof e->name[t]);
                }
            continue;
        }
        if (push(p)) break;
        added++;
    }
    free(pk);
    qsort(ix.e, ix.n, sizeof *ix.e, cmp_entry);
    return added;
}

void normal_maps_set_dds(int on) { ix.dds = on != 0; }

int normal_maps_has(uint64_t hash, int kind) {
    const Entry *e = find(hash, kind);
    return e && offered(e) >= 0;
}

int normal_maps_count(int kind) {
    int n = 0;
    for (size_t i = 0; i < ix.n; i++) n += ix.e[i].kind == kind && offered(&ix.e[i]) >= 0;
    return n;
}

char *normal_maps_path(uint64_t hash, int kind) {
    const Entry *e = find(hash, kind);
    const int t = e ? offered(e) : -1;
    if (t < 0) return NULL;
    const char *dir = ix.dirs[e->dir[t]];
    size_t len = strlen(dir) + strlen(e->name[t]) + 2;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dir, e->name[t]);
    return p;
}

void normal_maps_forget(uint64_t hash, int kind) {
    Entry *e = find(hash, kind);
    const int t = e ? offered(e) : -1;
    if (t < 0) return;
    e->dir[t] = -1;
    e->name[t][0] = 0;
}

void normal_maps_index_clear(void) {
    for (int i = 0; i < ix.ndirs; i++) free(ix.dirs[i]);
    free(ix.dirs);
    free(ix.e);
    const int dds = ix.dds;   /* a setting, not index contents */
    memset(&ix, 0, sizeof ix);
    ix.dds = dds;
}


/* ---- height (--parallax) ---- */
int normal_maps_image_height(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    if (!data || !w || !h || !levels || (format != TEXPACK_BGRA8 && format != TEXPACK_BC3 && format != TEXPACK_BC7)) return 0;
    const uint8_t *p = data;
    uint32_t lw = w, lh = h;
    for (uint32_t l = 0; l + 1 < levels && (lw > 64 || lh > 64); l++) {   /* the first level of at most 64 a side */
        p += texpack_level_bytes(format, lw, lh);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (format == TEXPACK_BGRA8) {
        for (size_t i = 0; i < (size_t)lw * lh; i++)
            if (p[i * 4 + 3] < NORMAL_MAPS_HEIGHT_FLAT) return 1;
        return 0;
    }
    const size_t blocks = (size_t)((lw + 3) / 4) * ((lh + 3) / 4);
    for (size_t b = 0; b < blocks; b++, p += 16) {
        if (format == TEXPACK_BC3) {   /* the alpha block's endpoints and, below them, its levels */
            const unsigned a0 = p[0], a1 = p[1];
            if ((a0 < a1 ? a0 : a1) < NORMAL_MAPS_HEIGHT_FLAT) return 1;
            continue;
        }
        uint32_t t[16];
        texpack_bc7_block(p, t);
        for (int i = 0; i < 16; i++)
            if ((t[i] >> 24) < NORMAL_MAPS_HEIGHT_FLAT) return 1;
    }
    return 0;
}

void normal_maps_set_height(uint64_t hash, int on) {
    Entry *e = find(hash, NORMAL_MAPS_RELIEF);
    if (e) e->height = on != 0;
}

int normal_maps_height(uint64_t hash) {
    const Entry *e = find(hash, NORMAL_MAPS_RELIEF);
    return e ? e->height : -1;
}

/* ---- glossy (--reflections sssr's glass) ---- */
static float gl_step(float a, float b, float x) {
    const float t = x <= a ? 0 : x >= b ? 1 : (x - a) / (b - a);
    return t * t * (3 - 2 * t);
}
static int gl_texel(uint32_t argb) {   /* B shine, A gloss */
    const float shine = (argb & 255) / 255.0f, gloss = (argb >> 24) / 255.0f;
    return gl_step(NORMAL_MAPS_GLOSSY_SHINE_FROM, NORMAL_MAPS_GLOSSY_SHINE_TO, shine) *
               gl_step(NORMAL_MAPS_GLOSSY_GLOSS_FROM, NORMAL_MAPS_GLOSSY_GLOSS_TO, gloss) >
           NORMAL_MAPS_GLOSSY_MIN;
}
int normal_maps_image_glossy(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    if (!data || !w || !h || !levels || (format != TEXPACK_BGRA8 && format != TEXPACK_BC3 && format != TEXPACK_BC7)) return 0;
    const uint8_t *p = data;
    uint32_t lw = w, lh = h;
    for (uint32_t l = 0; l + 1 < levels && (lw > 64 || lh > 64); l++) {   /* the first level of at most 64 a side */
        p += texpack_level_bytes(format, lw, lh);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (format == TEXPACK_BGRA8) {
        for (size_t i = 0; i < (size_t)lw * lh; i++)
            if (gl_texel((uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 3] << 24)) return 1;
        return 0;
    }
    const size_t blocks = (size_t)((lw + 3) / 4) * ((lh + 3) / 4);
    for (size_t b = 0; b < blocks; b++, p += 16) {
        uint32_t t[16];
        if (format == TEXPACK_BC3) {   /* gloss: the alpha block's endpoints (the larger); shine: the colour endpoints' blue */
            const unsigned a = p[0] > p[1] ? p[0] : p[1];
            const unsigned c0 = p[8] | p[9] << 8, c1 = p[10] | p[11] << 8;
            const unsigned b0 = (c0 & 31) * 255 / 31, b1 = (c1 & 31) * 255 / 31;
            if (gl_texel(a << 24 | (b0 > b1 ? b0 : b1))) return 1;
            continue;
        }
        texpack_bc7_block(p, t);
        for (int i = 0; i < 16; i++)
            if (gl_texel(t[i])) return 1;
    }
    return 0;
}

void normal_maps_set_glossy(uint64_t hash, int on) {
    Entry *e = find(hash, NORMAL_MAPS_SPECULAR);
    if (e) e->glossy = on != 0;
}

int normal_maps_glossy(uint64_t hash) {
    const Entry *e = find(hash, NORMAL_MAPS_SPECULAR);
    return e ? e->glossy : -1;
}
