/* Texture dumps and replacement packs: see texpack.h. */
#include "texpack.h"
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_image_write.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t texpack_hash(const XTexInfo *t, const uint8_t *data) {
    uint32_t hdr[4] = {t->code, t->w, t->h, 0};
    uint64_t h = fnv(0xcbf29ce484222325ull, (const uint8_t *)hdr, sizeof hdr);
    if (!t->linear) return fnv(h, data, xtex_level_bytes(t, 0));
    uint32_t row = t->w * (uint32_t)xtex_fmt_bpp(t->code) / 8;
    for (uint32_t y = 0; y < t->h; y++) h = fnv(h, data + (size_t)y * t->pitch, row);
    return h;
}

/* 0xAARRGGBB <-> RGBA bytes: swaps the R and B bytes of each texel. */
static void swap_rb(uint32_t *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = px[i];
        px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
}

int texpack_png_write(const char *path, uint32_t w, uint32_t h, const uint32_t *bgra) {
    size_t n = (size_t)w * h;
    uint32_t *rgba = malloc(n * 4 + 4);
    if (!rgba) return -1;
    memcpy(rgba, bgra, n * 4);
    swap_rb(rgba, n);
    int ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)w * 4);
    free(rgba);
    return ok ? 0 : -1;
}

uint32_t *texpack_png_read(const char *path, uint32_t *w, uint32_t *h) {
    int iw, ih, comp;
    uint8_t *p = stbi_load(path, &iw, &ih, &comp, 4);
    if (!p) return NULL;
    size_t n = (size_t)iw * ih;
    uint32_t *out = malloc(n * 4 + 4);
    if (out) {
        memcpy(out, p, n * 4);
        swap_rb(out, n);
        *w = (uint32_t)iw;
        *h = (uint32_t)ih;
    }
    stbi_image_free(p);
    return out;
}

uint32_t texpack_mip_count(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        n++;
    }
    return n;
}

uint32_t *texpack_mips(const uint32_t *bgra, uint32_t w, uint32_t h, uint32_t *levels) {
    uint32_t n = texpack_mip_count(w, h);
    size_t total = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < n; l++, lw = lw > 1 ? lw >> 1 : 1, lh = lh > 1 ? lh >> 1 : 1)
        total += (size_t)lw * lh;
    uint32_t *out = malloc(total * 4);
    if (!out) return NULL;
    memcpy(out, bgra, (size_t)w * h * 4);
    const uint32_t *src = out;
    uint32_t *dst = out + (size_t)w * h, sw = w, sh = h;
    for (uint32_t l = 1; l < n; l++) {
        uint32_t dw = sw > 1 ? sw >> 1 : 1, dh = sh > 1 ? sh >> 1 : 1;
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t x0 = 2 * x < sw ? 2 * x : sw - 1, x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
                uint32_t y0 = 2 * y < sh ? 2 * y : sh - 1, y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
                uint32_t a = src[y0 * sw + x0], b = src[y0 * sw + x1], c = src[y1 * sw + x0], d = src[y1 * sw + x1], o = 0;
                for (int s = 0; s < 32; s += 8)
                    o |= ((((a >> s) & 0xFF) + ((b >> s) & 0xFF) + ((c >> s) & 0xFF) + ((d >> s) & 0xFF) + 2) / 4) << s;
                dst[y * dw + x] = o;
            }
        src = dst;
        dst += (size_t)dw * dh;
        sw = dw;
        sh = dh;
    }
    *levels = n;
    return out;
}

/* ---- open-addressing map uint64 -> uint64 (key 0 = empty slot) ---- */
typedef struct {
    uint64_t *k, *v;
    size_t cap, n;
} Map;

static uint64_t *map_slot(const Map *m, uint64_t key) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (m->cap - 1);
    while (m->k[i] && m->k[i] != key) i = (i + 1) & (m->cap - 1);
    return &m->k[i];
}
static int map_get(const Map *m, uint64_t key, uint64_t *val) {
    if (!m->cap || !key) return 0;
    uint64_t *s = map_slot(m, key);
    if (!*s) return 0;
    *val = m->v[s - m->k];
    return 1;
}
static void map_put(Map *m, uint64_t key, uint64_t val) {
    if (!key) return;
    if ((m->n + 1) * 2 > m->cap) {
        Map g = {calloc(m->cap ? m->cap * 2 : 1024, 8), calloc(m->cap ? m->cap * 2 : 1024, 8), m->cap ? m->cap * 2 : 1024, 0};
        if (!g.k || !g.v) { free(g.k); free(g.v); return; }
        for (size_t i = 0; i < m->cap; i++)
            if (m->k[i]) { uint64_t *s = map_slot(&g, m->k[i]); *s = m->k[i]; g.v[s - g.k] = m->v[i]; g.n++; }
        free(m->k);
        free(m->v);
        *m = g;
    }
    uint64_t *s = map_slot(m, key);
    if (!*s) { *s = key; m->n++; }
    m->v[s - m->k] = val;
}
static void map_free(Map *m) {
    free(m->k);
    free(m->v);
    memset(m, 0, sizeof *m);
}

/* Map keys must not be 0: a (theoretical) hash 0 is stored as 1. */
static uint64_t mkey(uint64_t hash) { return hash ? hash : 1; }

/* 16 hex digits + ".png" (any case): the hash, or -1. */
static int parse_name(const char *name, uint64_t *hash) {
    uint64_t h = 0;
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        h = h << 4 | (uint64_t)d;
    }
    const char *e = name + 16;
    if (e[0] != '.' || (e[1] | 32) != 'p' || (e[2] | 32) != 'n' || (e[3] | 32) != 'g' || e[4]) return -1;
    *hash = h;
    return 0;
}

/* ---- replacement pack (layers: the first pack that has a hash wins) ---- */
typedef struct {
    char name[24];        /* file name as found (case kept); "" = disabled */
    uint32_t dir;         /* index into pack.dirs */
} PackEntry;
static struct {
    char **dirs;
    uint32_t ndirs;
    Map index;            /* hash -> entry + 1 */
    PackEntry *e;
    size_t n, cap;
} pack;

int texpack_open(const char *dir) {
    texpack_close();
    return texpack_add(dir);
}

int texpack_add(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **nd = realloc(pack.dirs, (pack.ndirs + 1) * sizeof *nd);
    char *copy = strdup(dir);
    if (!nd || !copy) {
        if (nd) pack.dirs = nd;
        free(copy);
        closedir(d);
        return -1;
    }
    pack.dirs = nd;
    uint32_t di = pack.ndirs++;
    pack.dirs[di] = copy;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        uint64_t h, v;
        if (parse_name(e->d_name, &h) || map_get(&pack.index, mkey(h), &v)) continue;
        if (pack.n == pack.cap) {
            size_t c = pack.cap ? pack.cap * 2 : 256;
            PackEntry *ne = realloc(pack.e, c * sizeof *ne);
            if (!ne) break;
            pack.e = ne;
            pack.cap = c;
        }
        snprintf(pack.e[pack.n].name, sizeof pack.e[0].name, "%s", e->d_name);
        pack.e[pack.n].dir = di;
        map_put(&pack.index, mkey(h), ++pack.n);
        added++;
    }
    closedir(d);
    return added;
}

int texpack_active(void) { return pack.ndirs != 0; }
int texpack_pack_count(void) { return (int)pack.ndirs; }

int texpack_has(uint64_t hash) {
    uint64_t v;
    return map_get(&pack.index, mkey(hash), &v) && pack.e[v - 1].name[0];
}

void texpack_disable(uint64_t hash) {
    uint64_t v;
    if (map_get(&pack.index, mkey(hash), &v)) pack.e[v - 1].name[0] = 0;
}

char *texpack_path(uint64_t hash) {
    uint64_t v;
    if (!texpack_has(hash) || !map_get(&pack.index, mkey(hash), &v)) return NULL;
    const PackEntry *pe = &pack.e[v - 1];
    size_t len = strlen(pack.dirs[pe->dir]) + 32;
    char *path = malloc(len);
    if (path) snprintf(path, len, "%s/%s", pack.dirs[pe->dir], pe->name);
    return path;
}

uint32_t *texpack_load_file(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels) {
    uint32_t iw, ih, *img = texpack_png_read(path, &iw, &ih);
    if (!img) return NULL;
    uint32_t *m = texpack_mips(img, iw, ih, levels);
    free(img);
    if (m) { *w = iw; *h = ih; }
    return m;
}

uint32_t *texpack_load(uint64_t hash, uint32_t *w, uint32_t *h, uint32_t *levels) {
    char *path = texpack_path(hash);
    if (!path) return NULL;
    uint32_t *m = texpack_load_file(path, w, h, levels);
    free(path);
    return m;
}

void texpack_close(void) {
    for (uint32_t i = 0; i < pack.ndirs; i++) free(pack.dirs[i]);
    free(pack.dirs);
    free(pack.e);
    map_free(&pack.index);
    memset(&pack, 0, sizeof pack);
}

/* ---- key -> hash memo ---- */
static Map memo;
int texpack_key_hash(uint64_t key, uint64_t *hash) { return map_get(&memo, key, hash); }
void texpack_key_set(uint64_t key, uint64_t hash) { map_put(&memo, key, hash); }

/* ---- dump ---- */
typedef struct {
    uint64_t hash;
    uint32_t code, w, h, levels, linear, alpha, draws, first_frame;
} DumpEntry;
static struct {
    char *dir;
    DumpEntry *e;
    size_t n, cap;
    Map index;   /* hash -> entry + 1 */
} dump;

static DumpEntry *dump_find(uint64_t hash) {
    uint64_t v;
    return map_get(&dump.index, mkey(hash), &v) ? &dump.e[v - 1] : NULL;
}

static DumpEntry *dump_add(const DumpEntry *src) {
    if (dump.n == dump.cap) {
        size_t c = dump.cap ? dump.cap * 2 : 256;
        DumpEntry *ne = realloc(dump.e, c * sizeof *ne);
        if (!ne) return NULL;
        dump.e = ne;
        dump.cap = c;
    }
    dump.e[dump.n] = *src;
    map_put(&dump.index, mkey(src->hash), ++dump.n);
    return &dump.e[dump.n - 1];
}

static void dump_free(void) {
    free(dump.dir);
    free(dump.e);
    map_free(&dump.index);
    memset(&dump, 0, sizeof dump);
}

static char *dump_path(const char *name) {
    size_t len = strlen(dump.dir) + strlen(name) + 2;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dump.dir, name);
    return p;
}

int texpack_dump_open(const char *dir) {
    if (dump.dir) texpack_dump_close();
    make_dir(dir);
    struct stat sb;
    if (stat(dir, &sb) || !S_ISDIR(sb.st_mode)) return -1;
    dump.dir = strdup(dir);
    char *mp = dump_path("manifest.csv");
    FILE *f = mp ? fopen(mp, "r") : NULL;
    free(mp);
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        DumpEntry d = {0};
        unsigned long long h;
        if (sscanf(line, "%16llx,0x%x,%u,%u,%u,%u,%u,%u,%u", &h, &d.code, &d.w, &d.h, &d.levels, &d.linear, &d.alpha,
                   &d.draws, &d.first_frame) == 9 && !dump_find(h)) {
            d.hash = h;
            dump_add(&d);
        }
    }
    if (f) fclose(f);
    return 0;
}

int texpack_dump_active(void) { return dump.dir != NULL; }

int texpack_dump_texture(uint64_t hash, const XTexInfo *t, const uint32_t *bgra, uint32_t frame) {
    if (!dump.dir || dump_find(hash)) return 0;
    DumpEntry d = {hash, t->code, t->w, t->h, t->levels, (uint32_t)t->linear, 0, 0, frame};
    for (size_t i = 0, n = (size_t)t->w * t->h; i < n && !d.alpha; i++) d.alpha = bgra[i] >> 24 != 0xFF;
    char name[32];
    snprintf(name, sizeof name, "%016llx.png", (unsigned long long)hash);
    char *p = dump_path(name);
    int r = p ? texpack_png_write(p, t->w, t->h, bgra) : -1;
    free(p);
    if (r) return -1;
    return dump_add(&d) ? 1 : -1;
}

void texpack_dump_draw(uint64_t hash) {
    DumpEntry *d = dump_find(hash);
    if (d) d->draws++;
}

int texpack_dump_flush(void) {
    if (!dump.dir) return -1;
    char *mp = dump_path("manifest.csv");
    FILE *f = mp ? fopen(mp, "w") : NULL;
    free(mp);
    if (!f) return -1;
    fputs("hash,format,w,h,levels,linear,has_alpha,draws,first_frame\n", f);
    for (size_t i = 0; i < dump.n; i++) {
        const DumpEntry *d = &dump.e[i];
        fprintf(f, "%016llx,0x%02X,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)d->hash, d->code, d->w, d->h, d->levels,
                d->linear, d->alpha, d->draws, d->first_frame);
    }
    fclose(f);
    return (int)dump.n;
}

int texpack_dump_close(void) {
    int n = texpack_dump_flush();
    dump_free();
    return n;
}
