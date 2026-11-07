/* --neon-glow: see neon_glow.h. */
#include "neon_glow.h"
#include "texpack.h"
#include <dirent.h>
#include <math.h>
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

int neon_glow_arg(NeonGlowConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--neon-glow") || !strcmp(opt, "--no-neon-glow")) {
        c->on = opt[2] == 'n' && opt[3] == 'e';
        return 1;
    }
    if (!strcmp(opt, "--neon-glow-strength")) return parse_float(val, 0.1f, 3.0f, &c->strength) ? -1 : 2;
    if (!strcmp(opt, "--neon-glow-bloom")) return parse_float(val, 0.0f, 1.0f, &c->bloom) ? -1 : 2;
    if (!strcmp(opt, "--neon-glow-flicker")) return parse_float(val, 0.0f, 0.3f, &c->flicker) ? -1 : 2;
    return 0;
}

/* ---- names ---- */
int neon_glow_mask_name(uint64_t hash, int dds, char *buf, size_t n) {
    if (n < 26) return -1;   /* 16 + ".emit.png" + NUL */
    snprintf(buf, n, "%016llx.emit.%s", (unsigned long long)hash, dds ? "dds" : "png");
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int neon_glow_parse_name(const char *name, uint64_t *hash, int *dds) {
    uint64_t h = 0;
    for (int i = 0; i < 16; i++) {
        int c = lower(name[i]);
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        h = h << 4 | (uint64_t)d;
    }
    const char *e = name + 16;
    char ext[10];
    for (int i = 0; i < 9; i++) {
        if (!e[i]) return -1;
        ext[i] = (char)lower(e[i]);
    }
    ext[9] = 0;
    if (e[9]) return -1;
    if (!strcmp(ext, ".emit.png")) *dds = 0;
    else if (!strcmp(ext, ".emit.dds")) *dds = 1;
    else return -1;
    *hash = h;
    return 0;
}

/* ---- index: entries sorted by hash ---- */
typedef struct {
    uint64_t hash;
    uint32_t dir;
    char name[28];   /* as found (case kept) */
    int dds;
} Mask;
static struct {
    char **dirs;
    uint32_t ndirs;
    Mask *m;
    size_t n, cap;
} ix;

static int cmp_mask(const void *a, const void *b) {
    uint64_t x = ((const Mask *)a)->hash, y = ((const Mask *)b)->hash;
    return x < y ? -1 : x > y;
}

static Mask *find(uint64_t hash) {
    size_t lo = 0, hi = ix.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix.m[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    return lo < ix.n && ix.m[lo].hash == hash ? &ix.m[lo] : NULL;
}

int neon_glow_index_add(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **nd = realloc(ix.dirs, (ix.ndirs + 1) * sizeof *nd);
    char *copy = strdup(dir);
    if (!nd || !copy) {
        if (nd) ix.dirs = nd;
        free(copy);
        closedir(d);
        return -1;
    }
    ix.dirs = nd;
    const uint32_t di = ix.ndirs++;
    ix.dirs[di] = copy;
    const size_t before = ix.n;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        uint64_t h;
        int dds;
        if (neon_glow_parse_name(e->d_name, &h, &dds)) continue;
        Mask *m = find(h);   /* earlier packs (sorted part) */
        if (!m)
            for (size_t i = before; i < ix.n; i++)   /* this pack (unsorted until the end) */
                if (ix.m[i].hash == h) m = &ix.m[i];
        if (m) {
            if (m->dir == di && dds && !m->dds) {   /* the .dds wins within a pack */
                snprintf(m->name, sizeof m->name, "%s", e->d_name);
                m->dds = 1;
            }
            continue;
        }
        if (ix.n == ix.cap) {
            size_t c = ix.cap ? ix.cap * 2 : 64;
            Mask *nm = realloc(ix.m, c * sizeof *nm);
            if (!nm) break;
            ix.m = nm;
            ix.cap = c;
        }
        Mask *nm = &ix.m[ix.n++];
        nm->hash = h;
        nm->dir = di;
        nm->dds = dds;
        snprintf(nm->name, sizeof nm->name, "%s", e->d_name);
        added++;
    }
    closedir(d);
    qsort(ix.m, ix.n, sizeof *ix.m, cmp_mask);
    return added;
}

int neon_glow_index_count(void) { return (int)ix.n; }
int neon_glow_has(uint64_t hash) { return find(hash) != NULL; }

char *neon_glow_mask_path(uint64_t hash) {
    const Mask *m = find(hash);
    if (!m) return NULL;
    size_t n = strlen(ix.dirs[m->dir]) + strlen(m->name) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", ix.dirs[m->dir], m->name);
    return p;
}

void neon_glow_index_forget(uint64_t hash) {
    Mask *m = find(hash);
    if (!m) return;
    memmove(m, m + 1, (size_t)(ix.m + ix.n - (m + 1)) * sizeof *m);
    ix.n--;
}

void neon_glow_index_clear(void) {
    for (uint32_t i = 0; i < ix.ndirs; i++) free(ix.dirs[i]);
    free(ix.dirs);
    free(ix.m);
    memset(&ix, 0, sizeof ix);
}

/* ---- loading ---- */
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *neon_glow_dds_parse(const uint8_t *buf, size_t n, uint32_t *w, uint32_t *h, size_t *offset) {
    if (n < 128 || memcmp(buf, "DDS ", 4) || rd32(buf + 4) != 124) return "not a DDS file";
    const uint32_t hh = rd32(buf + 12), ww = rd32(buf + 16);
    if (!ww || !hh || ww > 16384 || hh > 16384) return "bad size";
    if (!(rd32(buf + 80) & 4)) return "no FourCC (only BC4 masks)";
    size_t off = 128;
    const uint8_t *cc = buf + 84;
    if (!memcmp(cc, "DX10", 4)) {
        if (n < 148) return "truncated DX10 header";
        if (rd32(buf + 128) != 80) return "not BC4_UNORM (DXGI 80)";
        if (rd32(buf + 132) != 3 || rd32(buf + 140) > 1) return "not a single 2D texture";
        off = 148;
    } else if (memcmp(cc, "ATI1", 4) && memcmp(cc, "BC4U", 4)) {
        return "not BC4 (FourCC ATI1 / BC4U or DX10 BC4_UNORM)";
    }
    const size_t need = (size_t)((ww + 3) / 4) * ((hh + 3) / 4) * 8;
    if (n - off < need) return "truncated level 0";
    *w = ww;
    *h = hh;
    *offset = off;
    return NULL;
}

static void bc4_palette(int r0, int r1, uint8_t p[8]) {
    p[0] = (uint8_t)r0;
    p[1] = (uint8_t)r1;
    if (r0 > r1) {
        for (int k = 1; k < 7; k++) p[1 + k] = (uint8_t)(((7 - k) * r0 + k * r1) / 7);
    } else {
        for (int k = 1; k < 5; k++) p[1 + k] = (uint8_t)(((5 - k) * r0 + k * r1) / 5);
        p[6] = 0;
        p[7] = 255;
    }
}

void neon_glow_bc4_decode(const uint8_t *blocks, uint32_t w, uint32_t h, uint8_t *out) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = blocks + ((size_t)by * bw + bx) * 8;
            uint8_t p[8];
            bc4_palette(b[0], b[1], p);
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                if (x < w && y < h) out[(size_t)y * w + x] = p[bits >> (3 * i) & 7];
            }
        }
}

uint8_t *neon_glow_l8_mips(const uint8_t *l8, uint32_t w, uint32_t h, uint32_t *levels) {
    const uint32_t n = texpack_mip_count(w, h);
    size_t total = 0;
    for (uint32_t l = 0; l < n; l++) total += (size_t)(w >> l ? w >> l : 1) * (h >> l ? h >> l : 1);
    uint8_t *c = malloc(total);
    if (!c) return NULL;
    memcpy(c, l8, (size_t)w * h);
    uint8_t *src = c;
    uint32_t sw = w, sh = h;
    for (uint32_t l = 1; l < n; l++) {
        uint8_t *dst = src + (size_t)sw * sh;
        const uint32_t dw = sw >> 1 ? sw >> 1 : 1, dh = sh >> 1 ? sh >> 1 : 1;
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                const uint32_t x0 = 2 * x < sw ? 2 * x : sw - 1, x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
                const uint32_t y0 = 2 * y < sh ? 2 * y : sh - 1, y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
                const uint32_t s = src[(size_t)y0 * sw + x0] + src[(size_t)y0 * sw + x1] + src[(size_t)y1 * sw + x0] +
                                   src[(size_t)y1 * sw + x1];
                dst[(size_t)y * dw + x] = (uint8_t)((s + 2) / 4);
            }
        src = dst;
        sw = dw;
        sh = dh;
    }
    *levels = n;
    return c;
}

static int has_dds_ext(const char *path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && lower(path[n - 3]) == 'd' && lower(path[n - 2]) == 'd' && lower(path[n - 1]) == 's';
}

uint8_t *neon_glow_load(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels, const char **why) {
    uint8_t *l8 = NULL;
    uint32_t ww = 0, hh = 0;
    if (has_dds_ext(path)) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            *why = "cannot open";
            return NULL;
        }
        uint8_t *buf = NULL;
        long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
        if (n > 0 && n < (64l << 20) && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            size_t off;
            const char *bad = neon_glow_dds_parse(buf, (size_t)n, &ww, &hh, &off);
            if (bad) *why = bad;
            else if ((l8 = malloc((size_t)ww * hh))) neon_glow_bc4_decode(buf + off, ww, hh, l8);
            else *why = "out of memory";
        } else {
            *why = "cannot read";
        }
        free(buf);
        fclose(f);
    } else {
        uint32_t *px = texpack_png_read(path, &ww, &hh);
        if (!px) {
            *why = "cannot read the PNG";
            return NULL;
        }
        if ((l8 = malloc((size_t)ww * hh)))
            for (size_t i = 0; i < (size_t)ww * hh; i++) l8[i] = (uint8_t)(px[i] >> 16);   /* red */
        else *why = "out of memory";
        free(px);
    }
    if (!l8) return NULL;
    uint8_t *chain = neon_glow_l8_mips(l8, ww, hh, levels);
    free(l8);
    if (!chain) {
        *why = "out of memory";
        return NULL;
    }
    *w = ww;
    *h = hh;
    return chain;
}

/* ---- flicker ---- */
static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

float neon_glow_flicker(uint64_t hash, uint32_t frame, float amp) {
    if (amp <= 0) return 1.0f;
    const uint64_t r = mix64(hash);
    const double TAU = 6.283185307179586;
    const double t = frame / 60.0;
    const double hum_hz = 0.15 + (r & 0xFF) / 255.0 * 0.35, buzz_hz = 3.0 + (r >> 8 & 0xFF) / 255.0 * 2.0;
    const double p0 = (r >> 16 & 0xFFFF) / 65536.0 * TAU, p1 = (r >> 32 & 0xFFFF) / 65536.0 * TAU;
    double w = 0.35 * (0.5 + 0.5 * sin(TAU * hum_hz * t + p0)) + 0.15 * (0.5 + 0.5 * sin(TAU * buzz_hz * t + p1));
    /* a short dip every 6-20 s (per sign): a raised cosine over 12 frames */
    const uint32_t period = 360 + (uint32_t)(r >> 48 & 0x3FF) % 840, at = (frame + (uint32_t)(r >> 24)) % period;
    if (at < 12) w += 0.5 * (0.5 - 0.5 * cos(TAU * at / 12.0));
    if (w > 1) w = 1;
    return (float)(1.0 - amp * w);
}
