/* The disc folder reader and the asset container walk (format notes in disc.h). */
#include "disc.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t disc_crc32(uint32_t crc, const void *p, size_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    const uint8_t *s = p;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ s[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t disc_name_hash(const char *name) {
    char buf[1024];
    size_t n = 0;
    const char *pre = "ee_files\\";
    for (const char *s = pre; *s; s++) buf[n++] = *s;
    for (const char *s = name; *s && n < sizeof buf; s++) {
        char ch = *s == '/' ? '\\' : *s;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        buf[n++] = ch;
    }
    return disc_crc32(0, buf, n);
}

uint32_t disc_model_hash(const char *name) { return disc_crc32(0, name, strlen(name)); }

/* ---- XBoxWad.idx ---- */

static uint8_t *read_range(const char *path, long off, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = malloc(size ? size : 1);
    if (buf && size && (fseek(f, off, SEEK_SET) || fread(buf, 1, size, f) != size)) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static uint8_t *read_idx(const char *dir, size_t *n) {
    char path[1100];
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long len = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    if (len < 20 || len > (long)DISC_IDX_MAX) return NULL;
    *n = (size_t)len;
    return read_range(path, 0, (size_t)len);
}

int disc_index_parse(const uint8_t *idx, size_t n, Disc *d) {
    d->wads = NULL;
    d->loc = d->size = d->hash = NULL;
    d->nwads = d->nfiles = 0;
    if (n < 20 || n > DISC_IDX_MAX) return -1;
    const uint32_t un = (uint32_t)n, wads = rd32(idx), files = rd32(idx + 4), wad_table = rd32(idx + 12), file_table = rd32(idx + 16);
    if (wads > 256 || wad_table > un || (un - wad_table) / 36 < wads || file_table > un || (un - file_table) / 16 < files) return -1;
    d->wads = calloc(wads ? wads : 1, sizeof *d->wads);
    d->loc = malloc(sizeof(uint32_t) * (files ? files : 1));
    d->size = malloc(sizeof(uint32_t) * (files ? files : 1));
    d->hash = malloc(sizeof(uint32_t) * (files ? files : 1));
    if (!d->wads || !d->loc || !d->size || !d->hash) {
        free(d->wads), free(d->loc), free(d->size), free(d->hash);
        d->wads = NULL, d->loc = d->size = d->hash = NULL;
        return -1;
    }
    for (uint32_t i = 0; i < wads; i++) memcpy(d->wads[i], idx + wad_table + 36 * i, 32);
    for (uint32_t i = 0; i < files; i++) {
        const uint8_t *e = idx + file_table + 16 * i;
        d->loc[i] = rd32(e);
        d->size[i] = rd32(e + 4);
        d->hash[i] = rd32(e + 12);
    }
    d->nwads = wads;
    d->nfiles = files;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ idx[i]) * 0x100000001b3ull;
    d->fingerprint = h;
    return 0;
}

uint8_t *disc_read_hash(const char *dir, uint32_t hash, uint32_t max, size_t *n) {
    size_t len = 0;
    uint8_t *idx = read_idx(dir, &len);
    if (!idx) return NULL;
    Disc d;
    memset(&d, 0, sizeof d);
    uint8_t *out = NULL;
    if (!disc_index_parse(idx, len, &d))
        for (uint32_t i = 0; i < d.nfiles; i++) {
            if (d.hash[i] != hash || d.loc[i] >> 24 >= d.nwads || d.size[i] > max) continue;
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", dir, d.wads[d.loc[i] >> 24]);
            out = read_range(path, (long)(d.loc[i] & 0xffffff) << 10, d.size[i]);
            if (out) *n = d.size[i];
            break;
        }
    free(d.wads), free(d.loc), free(d.size), free(d.hash);
    free(idx);
    return out;
}

int disc_open(Disc *d, const char *dir) {
    memset(d, 0, sizeof *d);
    size_t n = 0;
    uint8_t *idx = read_idx(dir, &n);
    int r = idx ? disc_index_parse(idx, n, d) : -1;
    free(idx);
    if (r) {
        memset(d, 0, sizeof *d);
        return -1;
    }
    snprintf(d->dir, sizeof d->dir, "%s", dir);
    for (int k = 0; k < 8; k++) d->fpw[k] = -1;
    return 0;
}

void disc_close(Disc *d) {
    for (int k = 0; k < 8; k++)
        if (d->fp[k]) fclose(d->fp[k]);
    free(d->wads), free(d->loc), free(d->size), free(d->hash), free(d->chunks), free(d->tables);
    memset(d, 0, sizeof *d);
}

static FILE *wad_file(Disc *d, uint32_t w) {
    int slot = (int)(w % 8);
    if (d->fpw[slot] == (int)w && d->fp[slot]) return d->fp[slot];
    if (d->fp[slot]) fclose(d->fp[slot]);
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", d->dir, d->wads[w]);
    d->fp[slot] = fopen(path, "rb");
    d->fpw[slot] = d->fp[slot] ? (int)w : -1;
    if (d->fp[slot]) setvbuf(d->fp[slot], NULL, _IOFBF, 1 << 16);
    return d->fp[slot];
}

int disc_pread(Disc *d, uint32_t i, uint32_t off, void *buf, uint32_t size) {
    if (i >= d->nfiles || d->loc[i] >> 24 >= d->nwads || off > d->size[i] || size > d->size[i] - off) return -1;
    FILE *f = wad_file(d, d->loc[i] >> 24);
    if (!f) return -1;
    long at = ((long)(d->loc[i] & 0xffffff) << 10) + (long)off;
    if (ftell(f) != at && fseek(f, at, SEEK_SET)) return -1;
    return fread(buf, 1, size, f) == size ? 0 : -1;
}

uint8_t *disc_file(Disc *d, uint32_t i, size_t *n) {
    if (i >= d->nfiles) return NULL;
    uint8_t *b = malloc(d->size[i] ? d->size[i] : 1);
    if (b && disc_pread(d, i, 0, b, d->size[i])) {
        free(b);
        return NULL;
    }
    if (b) *n = d->size[i];
    return b;
}

/* ---- container walk ---- */

typedef struct {
    DiscChunk *c;
    int n, cap;
} ChunkList;

static int push(ChunkList *l, const DiscChunk *c) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        DiscChunk *nc = realloc(l->c, sizeof *nc * (size_t)cap);
        if (!nc) return -1;
        l->c = nc;
        l->cap = cap;
    }
    l->c[l->n++] = *c;
    return 0;
}

/* container._read_item: one item at p into l; the offset after it, or 0 when it does not fit. */
static uint64_t read_item(DiscReadFn read, void *ud, uint64_t len, uint64_t p, int bare, ChunkList *l) {
    uint8_t h[16];
    if (p + 16 > len || read(ud, p, h)) return 0;
    const uint64_t total = rd32(h + 4);
    const uint32_t item = rd32(h + 12);
    if (rd32(h + 8)) return 0;
    p += 16;
    uint64_t acc = 0;
    int first = l->n;
    while (acc < total) {
        if (p + 16 > len || read(ud, p, h)) return 0;
        const uint64_t s = rd32(h + 4);
        if (rd32(h + 8) || acc + s > total || p + 16 + s > len) return 0;
        DiscChunk c = {rd32(h), rd32(h + 12), item, 0, (uint32_t)(p + 16), (uint32_t)s, (uint8_t)bare};
        if (push(l, &c)) return 0;
        acc += s;
        p += 16 + s;
    }
    return l->n > first ? p : 0;
}

int disc_walk(DiscReadFn read, void *ud, uint64_t len, int bare_only, void (*emit)(void *ud, const DiscChunk *c)) {
    ChunkList l = {NULL, 0, 0};
    uint8_t h[16];
    int pack = 0;
    if (!bare_only && len >= 16 && !read(ud, 0, h) && !rd32(h + 8) && rd32(h + 12) == DISC_PACK_MAGIC) {
        pack = 1;
        uint64_t p = 16;
        for (uint32_t k = 0, n = rd32(h); k < n; k++)
            if (!(p = read_item(read, ud, len, p, 0, &l))) {
                pack = 0;
                break;
            }
        if (!pack) l.n = 0;
    }
    if (!pack && len >= 32 && !read_item(read, ud, len, 0, 1, &l)) l.n = 0;
    for (int i = 0; i < l.n; i++) emit(ud, &l.c[i]);
    int n = l.n;
    free(l.c);
    return n;
}

typedef struct {
    const uint8_t *buf;
    DiscChunk *out;
    int n, max;
} BufWalk;

static int buf_read(void *ud, uint64_t off, uint8_t out[16]) {
    memcpy(out, ((BufWalk *)ud)->buf + off, 16);
    return 0;
}

static void buf_emit(void *ud, const DiscChunk *c) {
    BufWalk *w = ud;
    if (w->n < w->max) w->out[w->n] = *c;
    w->n++;
}

int disc_walk_buffer(const uint8_t *buf, size_t len, int bare_only, DiscChunk *out, int max) {
    BufWalk w = {buf, out, 0, max};
    disc_walk(buf_read, &w, len, bare_only, buf_emit);
    return w.n;
}

typedef struct {
    Disc *d;
    uint32_t file;
    ChunkList *into;
    int only_skins;
} FileWalk;

static int file_read(void *ud, uint64_t off, uint8_t out[16]) {
    FileWalk *w = ud;
    return disc_pread(w->d, w->file, (uint32_t)off, out, 16);
}

static void file_emit(void *ud, const DiscChunk *c) {
    FileWalk *w = ud;
    if (w->only_skins && c->cls != DISC_CLASS_SKINS) return;
    DiscChunk k = *c;
    k.file = w->file;
    push(w->into, &k);
}

int disc_scan(Disc *d) {
    ChunkList all = {NULL, 0, 0}, tabs = {NULL, 0, 0};
    for (uint32_t i = 0; i < d->nfiles; i++) {
        FileWalk w = {d, i, &all, 0};
        disc_walk(file_read, &w, d->size[i], 0, file_emit);
        /* bindings._load_tables' quick check before parse_bare: a file of 64+ bytes whose head is
         * {1..64, total, 0, ...} and whose first chunk is not a mesh. */
        uint8_t h[32];
        if (d->size[i] < 64 || disc_pread(d, i, 0, h, 32)) continue;
        uint32_t n = rd32(h);
        if (rd32(h + 8) || n < 1 || n > 64 || rd32(h + 16) == DISC_CLASS_MESH) continue;
        FileWalk t = {d, i, &tabs, 1};
        disc_walk(file_read, &t, d->size[i], 1, file_emit);
    }
    free(d->chunks), free(d->tables);
    d->chunks = all.c, d->nchunks = (uint32_t)all.n;
    d->tables = tabs.c, d->ntables = (uint32_t)tabs.n;
    return all.n;
}

/* The index file: "DISCIDX1", u64 fingerprint, u32 nchunks, u32 ntables, then the chunks and the
 * tables as 7 u32 each {cls, hash, item, file, off, size, bare}, little endian. */
#define IDX_MAGIC "DISCIDX1"
#define IDX_MAX_CHUNKS (1u << 22)

static int idx_load(Disc *d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[24];
    int ok = fread(h, 1, 24, f) == 24 && !memcmp(h, IDX_MAGIC, 8);
    uint64_t fp = 0;
    for (int i = 0; ok && i < 8; i++) fp |= (uint64_t)h[8 + i] << (8 * i);
    const uint32_t nc = ok ? rd32(h + 16) : 0, nt = ok ? rd32(h + 20) : 0;
    ok = ok && fp == d->fingerprint && nc && nc <= IDX_MAX_CHUNKS && nt <= IDX_MAX_CHUNKS;
    DiscChunk *c = ok ? malloc(sizeof *c * nc) : NULL, *t = ok ? malloc(sizeof *t * (nt ? nt : 1)) : NULL;
    ok = ok && c && t;
    for (uint32_t i = 0; ok && i < nc + nt; i++) {
        uint8_t r[28];
        DiscChunk *o = i < nc ? &c[i] : &t[i - nc];
        ok = fread(r, 1, 28, f) == 28;
        if (!ok) break;
        *o = (DiscChunk){rd32(r), rd32(r + 4), rd32(r + 8), rd32(r + 12), rd32(r + 16), rd32(r + 20), (uint8_t)rd32(r + 24)};
        ok = o->file < d->nfiles && o->off <= d->size[o->file] && o->size <= d->size[o->file] - o->off;   /* this disc's files */
    }
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        free(c), free(t);
        return -1;
    }
    free(d->chunks), free(d->tables);
    d->chunks = c, d->nchunks = nc, d->tables = t, d->ntables = nt;
    return 0;
}

static void put_u32(FILE *f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void idx_save(const Disc *d, const char *path) {
    char tmp[1200];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(IDX_MAGIC, 1, 8, f);
    put_u32(f, (uint32_t)d->fingerprint), put_u32(f, (uint32_t)(d->fingerprint >> 32));
    put_u32(f, d->nchunks), put_u32(f, d->ntables);
    for (uint32_t i = 0; i < d->nchunks + d->ntables; i++) {
        const DiscChunk *c = i < d->nchunks ? &d->chunks[i] : &d->tables[i - d->nchunks];
        put_u32(f, c->cls), put_u32(f, c->hash), put_u32(f, c->item), put_u32(f, c->file), put_u32(f, c->off), put_u32(f, c->size), put_u32(f, c->bare);
    }
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    remove(path);   /* Windows' rename does not replace */
    if (!ok || rename(tmp, path)) remove(tmp);
}

int disc_scan_cached(Disc *d, const char *path, int *loaded) {
    if (loaded) *loaded = 0;
    if (path && *path && !idx_load(d, path)) {
        if (loaded) *loaded = 1;
        return (int)d->nchunks;
    }
    int n = disc_scan(d);
    if (n > 0 && path && *path) idx_save(d, path);
    return n;
}

uint8_t *disc_chunk_read(Disc *d, const DiscChunk *c) {
    uint8_t *b = malloc(c->size ? c->size : 1);
    if (b && disc_pread(d, c->file, c->off, b, c->size)) {
        free(b);
        return NULL;
    }
    return b;
}

/* ---- skin bindings ---- */

int disc_skins_parse(const uint8_t *p, size_t n, DiscSkin *out, int max) {
    if (n < 16) return -1;
    const uint64_t cnt = rd32(p);
    if (16 + cnt * 32 > n || n - (16 + cnt * 32) >= 16) return -1;
    for (uint64_t i = 0; i < cnt; i++)
        if (rd32(p + 16 + i * 32 + 20) != rd32(p + 16 + i * 32 + 24)) return -1;
    for (uint64_t i = 0; i < cnt && i < (uint64_t)max; i++) {
        const uint8_t *r = p + 16 + i * 32;
        out[i] = (DiscSkin){rd32(r), rd32(r + 4), rd32(r + 8), rd32(r + 12)};
    }
    return cnt > 0x7fffffff ? -1 : (int)cnt;
}

int disc_skins_for(const DiscSkin *recs, int nrec, uint32_t chunk_hash, uint32_t item_hash, DiscSkin *out, int max) {
    int k = 0;
    for (int i = 0; i < nrec && k < max; i++)
        if (recs[i].id == chunk_hash) out[k++] = recs[i];
    int exact = k;
    for (int i = 0; i < nrec && k < max; i++) {
        if (recs[i].model != item_hash) continue;
        int dup = 0;   /* `r not in exact`: namedtuples compare by value */
        for (int j = 0; j < exact; j++)
            dup |= !memcmp(&out[j], &recs[i], sizeof recs[i]);
        if (!dup) out[k++] = recs[i];
    }
    return k;
}

/* ---- characters ---- */

#define VB_COMMON 0x00800001u
#define IB_COMMON 0x00010001u

int disc_is_char(const uint8_t *p, size_t n) { return n >= 0x80 && rd32(p) == 1 && rd32(p + 4) == 0x76; }

static int all_ee(const uint8_t *p, size_t a, size_t b) {
    for (size_t i = a; i < b; i++)
        if (p[i] != 0xEE) return 0;
    return 1;
}

static int cfail(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
    return -1;
}

int disc_char_parse(const uint8_t *p, size_t n, DiscChar *c, char *err, size_t cap) {
    memset(c, 0, sizeof *c);
    if (!disc_is_char(p, n)) return cfail(err, cap, "not a skinned character");
    const uint64_t tail = rd32(p + 12), rec = 0x80 + tail;
    if (rec + 36 > n) return cfail(err, cap, "resource record out of range");
    uint32_t r[9];
    for (int i = 0; i < 9; i++) r[i] = rd32(p + rec + 4 * i);
    if (r[0] != VB_COMMON || r[3] != VB_COMMON || r[6] != IB_COMMON || r[1] || r[2] || r[5] || r[8])
        return cfail(err, cap, "unexpected VB/VB/IB resource headers");
    const uint64_t d1 = r[4], d2 = r[7];
    if (!(d1 <= d2 && d2 <= tail)) return cfail(err, cap, "resource offsets out of order");
    const uint64_t nv = d1 / DISC_STRIDE0, s1_end = d1 + nv * DISC_STRIDE1;
    if (s1_end > d2 || !all_ee(p, 0x80 + nv * DISC_STRIDE0, 0x80 + d1) || !all_ee(p, 0x80 + s1_end, 0x80 + d2))
        return cfail(err, cap, "stream sizes don't match the vertex count");
    if ((tail - d2) % 6) return cfail(err, cap, "stream sizes don't match");
    uint64_t tag = 0;
    int found = 0;
    for (uint64_t i = rec + 36; i + 4 <= n; i++)
        if (!memcmp(p + i, "cter", 4)) {
            tag = i, found = 1;
            break;
        }
    if (!found || !all_ee(p, rec + 36, tag)) return cfail(err, cap, "no 'cter' matrix block after the resources");
    const uint64_t mats = tag + 4;
    if (mats + DISC_PALETTE * 64 + 16 > n) return cfail(err, cap, "matrix block truncated");
    const uint32_t mode = rd32(p + mats + DISC_PALETTE * 64);
    const uint64_t nidx = (tail - d2) / 2;
    if (nidx != (uint64_t)(mode & 0xFFFF) * 3) return cfail(err, cap, "index count != 3 * primitive count");
    const uint8_t *ib = p + 0x80 + d2;
    for (uint64_t i = 0; i < nidx; i++)
        if ((uint32_t)(ib[2 * i] | ib[2 * i + 1] << 8) >= nv) return cfail(err, cap, "index out of range");
    for (int k = 0; k < DISC_PALETTE; k++) memcpy(c->inverse_bind[k], p + mats + 64 * k, 64);
    c->s0 = p + 0x80;
    c->s1 = p + 0x80 + d1;
    c->idx = ib;
    c->nv = (uint32_t)nv;
    c->ni = (uint32_t)nidx;
    return 0;
}

/* ---- static props ---- */

int disc_is_prop(const uint8_t *p, size_t n) { return n >= 0x80 && rd32(p) == 2 && rd32(p + 4) == 0x6D; }

int disc_prop_parse(const uint8_t *p, size_t n, DiscProp *o, char *err, size_t cap) {
    memset(o, 0, sizeof *o);
    o->normal = o->normal_packed = o->colour = o->uv0 = -1;
    if (!disc_is_prop(p, n)) return cfail(err, cap, "not a static prop");
    const uint64_t tail = rd32(p + 12);
    uint32_t r[6];
    for (int i = 0; i < 6; i++) r[i] = rd32(p + 0x54 + 4 * i);
    if (r[0] != VB_COMMON || r[3] != IB_COMMON || r[2] || r[5]) return cfail(err, cap, "unexpected VB/IB resource headers");
    const uint32_t fvf = rd32(p + 0x70), mode = rd32(p + 0x74), count = rd32(p + 0x78);
    const uint32_t prim = mode & 0xFF, stride = (mode >> 8) & 0xFF;
    /* mesh.fvf_layout */
    if ((fvf & 0x00E) != 0x002) return cfail(err, cap, "unsupported FVF position type");
    int off = 12, normal = -1, colour = -1, uv0 = -1;
    if (fvf & 0x010) normal = off, off += 12;
    if (fvf & 0x040) colour = off, off += 4;
    if (fvf & 0x080) off += 4;
    for (uint32_t i = 0; i < ((fvf >> 8) & 0xF); i++) {
        static const int size[4] = {8, 12, 16, 4};
        if (i == 0) uv0 = off;
        off += size[(fvf >> (16 + 2 * i)) & 3];
    }
    int packed = 0;
    if ((int)stride != off) {
        if (normal < 0 || (int)stride != off - 8) return cfail(err, cap, "the stride doesn't match the FVF");
        if (colour > normal) colour -= 8;
        if (uv0 > normal) uv0 -= 8;
        packed = 1;
    }
    const uint64_t vbd = r[1], ibd = r[4];
    if (!stride || !(vbd <= ibd && ibd <= tail && 0x80 + tail <= n)) return cfail(err, cap, "resource offsets out of range");
    const uint64_t nv = (ibd - vbd) / stride;
    if (!all_ee(p, 0x80 + vbd + nv * stride, 0x80 + ibd)) return cfail(err, cap, "vertex/index gap isn't 0xEE padding");
    const uint64_t nidx = (tail - ibd) / 2;
    const uint8_t *ib = p + 0x80 + ibd;
    if (prim == 5) {
        if (nidx != (uint64_t)(count & 0xFFFF) * 3) return cfail(err, cap, "index count != 3 * primitive count");
    } else if (prim != 6) {
        return cfail(err, cap, "unsupported primitive type");
    }
    for (uint64_t i = 0; i < nidx; i++)
        if ((uint32_t)(ib[2 * i] | ib[2 * i + 1] << 8) >= nv) return cfail(err, cap, "index out of range");
    o->vb = p + 0x80 + vbd, o->idx = ib;
    o->nv = (uint32_t)nv, o->nidx = (uint32_t)nidx, o->stride = stride, o->fvf = fvf, o->prim = prim;
    o->normal = packed ? -1 : normal, o->normal_packed = packed ? normal : -1, o->colour = colour, o->uv0 = uv0;
    return 0;
}

/* ---- textures ---- */

int disc_tex_parse(const uint8_t *p, size_t n, uint32_t *format, uint32_t *size, uint32_t *data_off) {
    if (n < 0x80 + 20) return -1;
    const uint64_t ro = (uint64_t)rd32(p + 12) + 0x80;
    if (ro + 20 > n) return -1;
    if ((rd32(p + ro) & 0x00070000u) != 0x00040000u) return -1;
    *data_off = 0x80 + rd32(p + ro + 4);
    *format = rd32(p + ro + 12);
    *size = rd32(p + ro + 16);
    return 0;
}
