/* Texture layers of the Lua mods: see texlayer.h. */
#include "texlayer.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t tex;
    int layer;          /* index into tl.layer */
    char *path;
    uint32_t *px, w, h; /* read on first use (texlayer_compose) */
    int failed;
} LayerFile;

typedef struct {
    uint64_t tex, key;
    int n;
    uint16_t file[TEXLAYER_MAX_STACK];
} Combo;

static struct {
    struct {
        char name[96];
        int on;
    } layer[TEXLAYER_MAX_LAYERS];
    int nlayers, active;
    LayerFile *file;
    int nfiles;
    Combo combo[TEXLAYER_MAX_COMBOS];
    int ncombos;   /* published with a release store once the entry is written */
} tl;

static int hexdigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

int texlayer_parse_name(const char *name, uint64_t *tex, char *label) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int d = hexdigit(name[i]);
        if (d < 0) return -1;
        v = v << 4 | (uint64_t)d;
    }
    if (name[16] != '@') return -1;
    const char *l = name + 17, *dot = strrchr(l, '.');
    if (!dot || strcmp(dot, ".png") || dot == l || dot - l > 40) return -1;
    for (const char *p = l; p < dot; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return -1;
    memcpy(label, l, (size_t)(dot - l));
    label[dot - l] = 0;
    *tex = v;
    return 0;
}

static int layer_index(const char *name) {
    for (int i = 0; i < tl.nlayers; i++)
        if (!strcmp(tl.layer[i].name, name)) return i;
    return -1;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int texlayer_add_dir(const char *dir, const char *id) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **names = NULL;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {   /* in file-name order: the stacking order is the same everywhere */
        uint64_t tex;
        char label[41];
        if (texlayer_parse_name(e->d_name, &tex, label)) continue;
        char **nn = realloc(names, (n + 1) * sizeof *nn);
        if (!nn || !(nn[n] = strdup(e->d_name))) {
            names = nn ? nn : names;
            break;
        }
        names = nn;
        n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, by_name);
    int added = 0;
    for (size_t k = 0; k < n; k++) {
        uint64_t tex;
        char label[41], name[96], path[4096];
        texlayer_parse_name(names[k], &tex, label);
        if ((size_t)snprintf(name, sizeof name, "%s/%s", id, label) >= sizeof name) continue;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, names[k]) >= sizeof path) continue;
        int li = layer_index(name);
        if (li < 0) {
            if (tl.nlayers == TEXLAYER_MAX_LAYERS) {
                printf("[texlayer] %s: more than %d layers; skipped\n", name, TEXLAYER_MAX_LAYERS);
                continue;
            }
            li = tl.nlayers++;
            snprintf(tl.layer[li].name, sizeof tl.layer[li].name, "%s", name);
            tl.layer[li].on = 0;
        }
        int dup = 0;
        for (int i = 0; i < tl.nfiles && !dup; i++) dup = tl.file[i].tex == tex && tl.file[i].layer == li;
        if (dup || tl.nfiles == TEXLAYER_MAX_FILES) continue;
        LayerFile *nf = realloc(tl.file, (size_t)(tl.nfiles + 1) * sizeof *nf);
        if (!nf) break;
        tl.file = nf;
        tl.file[tl.nfiles++] = (LayerFile){tex, li, strdup(path), NULL, 0, 0, 0};
        added++;
    }
    for (size_t k = 0; k < n; k++) free(names[k]);
    free(names);
    return added;
}

int texlayer_set(const char *name, int on) {
    int li = name ? layer_index(name) : -1;
    if (li < 0) return -1;
    on = on != 0;
    if (tl.layer[li].on != on) tl.active += on ? 1 : -1;
    tl.layer[li].on = on;
    int n = 0;
    for (int i = 0; i < tl.nfiles; i++) n += tl.file[i].layer == li;
    return n;
}

int texlayer_on(const char *name) {
    int li = name ? layer_index(name) : -1;
    return li < 0 ? -1 : tl.layer[li].on;
}

int texlayer_active(void) { return tl.active; }
int texlayer_count(void) { return tl.nfiles; }

static uint64_t fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) h = (h ^ (v >> (8 * i) & 0xff)) * 0x100000001b3ull;
    return h;
}

uint16_t texlayer_select(uint64_t tex) {
    if (!tl.active) return 0;
    uint16_t f[TEXLAYER_MAX_STACK];
    int n = 0;
    for (int i = 0; i < tl.nfiles && n < TEXLAYER_MAX_STACK; i++)
        if (tl.file[i].tex == tex && tl.layer[tl.file[i].layer].on) f[n++] = (uint16_t)i;
    if (!n) return 0;
    const int nc = __atomic_load_n(&tl.ncombos, __ATOMIC_ACQUIRE);
    for (int c = 0; c < nc; c++)
        if (tl.combo[c].tex == tex && tl.combo[c].n == n && !memcmp(tl.combo[c].file, f, (size_t)n * sizeof f[0])) return (uint16_t)(c + 1);
    if (nc == TEXLAYER_MAX_COMBOS) {
        static int warned;
        if (!warned++) printf("[texlayer] more than %d layer combinations; later ones not drawn\n", TEXLAYER_MAX_COMBOS);
        return 0;
    }
    Combo *c = &tl.combo[nc];
    c->tex = tex;
    c->n = n;
    memcpy(c->file, f, (size_t)n * sizeof f[0]);
    uint64_t k = fnv(0xcbf29ce484222325ull ^ 0x746c6179ull, tex);
    for (int i = 0; i < n; i++) k = fnv(k, f[i]);
    c->key = k;
    __atomic_store_n(&tl.ncombos, nc + 1, __ATOMIC_RELEASE);
    return (uint16_t)(nc + 1);
}

static const Combo *combo(uint16_t id) {
    return id && id <= __atomic_load_n(&tl.ncombos, __ATOMIC_ACQUIRE) ? &tl.combo[id - 1] : NULL;
}

uint64_t texlayer_key(uint16_t id) {
    const Combo *c = combo(id);
    return c ? c->key : 0;
}

void texlayer_blend(uint32_t *bgra, uint32_t w, uint32_t h, const uint32_t *layer, uint32_t lw, uint32_t lh) {
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *lrow = layer + (size_t)(lh == h ? y : (uint64_t)y * lh / h) * lw;
        uint32_t *row = bgra + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t l = lrow[lw == w ? x : (uint64_t)x * lw / w], a = l >> 24;
            if (!a) continue;
            const uint32_t b = row[x];
            uint32_t out = b & 0xff000000u;
            for (int s = 0; s < 24; s += 8) {
                const uint32_t lc = l >> s & 0xff, bc = b >> s & 0xff;
                out |= ((lc * a + bc * (255 - a) + 127) / 255) << s;
            }
            row[x] = out;
        }
    }
}

int texlayer_compose(uint16_t id, uint32_t *bgra, uint32_t w, uint32_t h, TexLayerLoad load) {
    const Combo *c = combo(id);
    if (!c) return 1;
    int missing = 0;
    for (int i = 0; i < c->n; i++) {
        LayerFile *f = &tl.file[c->file[i]];
        if (!f->px && !f->failed) {
            f->px = load ? load(f->path, &f->w, &f->h) : NULL;
            if (!f->px || !f->w || !f->h) {
                free(f->px);
                f->px = NULL;
                f->failed = 1;
                printf("[texlayer] cannot read the layer %s\n", f->path);
            }
        }
        if (f->px) texlayer_blend(bgra, w, h, f->px, f->w, f->h);
        else missing++;
    }
    return missing;
}

void texlayer_close(void) {
    for (int i = 0; i < tl.nfiles; i++) {
        free(tl.file[i].path);
        free(tl.file[i].px);
    }
    free(tl.file);
    memset(&tl, 0, sizeof tl);
}
