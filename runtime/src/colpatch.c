/* Collision patches for mods: see colpatch.h. */
#include "colpatch.h"
#include "materials.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HP(a) ((uint8_t *)(uintptr_t)(a))   /* address inside the world -> host pointer (identity) */
#define HA(p) ((uint32_t)(uintptr_t)(p))

static uint32_t rd32(const uint8_t *w, int off) { uint32_t v; memcpy(&v, w + off, 4); return v; }
static uint16_t rd16(const uint8_t *w, int off) { uint16_t v; memcpy(&v, w + off, 2); return v; }
static float rdf(const uint8_t *w, int off) { float v; memcpy(&v, w + off, 4); return v; }
static void wr32(uint8_t *w, int off, uint32_t v) { memcpy(w + off, &v, 4); }
static void wr16(uint8_t *w, int off, uint16_t v) { memcpy(w + off, &v, 2); }

/* ---- parsing ---- */

void colpatch_free(ColPatch *p) {
    free(p->tri);
    memset(p, 0, sizeof *p);
}

static int add_tri(ColPatch *p, const float a[3], const float b[3], const float c[3], uint16_t flags, uint8_t mat) {
    if (p->ntri >= COLPATCH_TRI_MAX) return -1;
    if (p->ntri == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 32;
        ColTri *t = realloc(p->tri, cap * sizeof *t);
        if (!t) return -1;
        p->tri = t, p->cap = cap;
    }
    ColTri *t = &p->tri[p->ntri++];
    memcpy(t->p[0], a, 12), memcpy(t->p[1], b, 12), memcpy(t->p[2], c, 12);
    t->flags = flags | COLPATCH_FLAG_SOLID;
    t->material = mat;
    return 0;
}
/* A quad a b c d (front side counter-clockwise) as two triangles. */
static int add_quad(ColPatch *p, const float a[3], const float b[3], const float c[3], const float d[3], uint16_t fl, uint8_t mat) {
    return add_tri(p, a, b, c, fl, mat) || add_tri(p, a, c, d, fl, mat) ? -1 : 0;
}
static int add_wall(ColPatch *p, float x0, float y0, float x1, float y1, float z0, float z1, uint16_t fl, uint8_t mat) {
    const float a[3] = {x0, y0, z0}, b[3] = {x1, y1, z0}, c[3] = {x1, y1, z1}, d[3] = {x0, y0, z1};
    return add_quad(p, a, b, c, d, fl | COLPATCH_FLAG_TWO_SIDED, mat);
}
static int add_flat(ColPatch *p, float x0, float y0, float x1, float y1, float z, int up, uint16_t fl, uint8_t mat) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    const float a[3] = {x0, y0, z}, b[3] = {x1, y0, z}, c[3] = {x1, y1, z}, d[3] = {x0, y1, z};
    return up ? add_quad(p, a, b, c, d, fl, mat) : add_quad(p, a, d, c, b, fl | COLPATCH_FLAG_TWO_SIDED, mat);
}

/* Reads exactly n numbers from s (nothing else after them). */
static int numbers(const char *s, float *out, int n) {
    for (int i = 0; i < n; i++) {
        char *e;
        double v = strtod(s, &e);
        if (e == s || !isfinite(v) || fabs(v) > 1e6) return -1;
        out[i] = (float)v;
        s = e;
    }
    while (*s == ' ' || *s == '\t') s++;
    return *s ? -1 : 0;
}

int colpatch_parse(const char *text, size_t len, ColPatch *p, char *err, size_t cap) {
    memset(p, 0, sizeof *p);
    uint16_t flags = COLPATCH_FLAGS_DEFAULT;
    uint8_t mat = 1;
    size_t i = 0;
    for (int line = 1; i < len; line++) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        char buf[256];
        size_t n = e - i;
        if (n >= sizeof buf) { snprintf(err, cap, "line %d: longer than %d characters", line, (int)sizeof buf - 1); goto bad; }
        memcpy(buf, text + i, n);
        buf[n] = 0;
        i = e + 1;
        buf[strcspn(buf, ";#")] = 0;   /* comments, whole-line or after a value */
        char *s = buf, *t = buf + strlen(buf);
        while (*s == ' ' || *s == '\t') s++;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) *--t = 0;
        if (!*s) continue;
        char *eq = strchr(s, '=');
        char word[16];
        size_t wn = strcspn(s, " \t=");
        if (wn >= sizeof word) wn = sizeof word - 1;
        memcpy(word, s, wn), word[wn] = 0;
        if (eq && (!strcmp(word, "level") || !strcmp(word, "material") || !strcmp(word, "flags"))) {
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            if (!strcmp(word, "level")) {
                size_t vn = strlen(v);
                int ok = vn > 0 && vn < sizeof p->level;
                for (size_t k = 0; ok && k < vn; k++) ok = (v[k] >= 'a' && v[k] <= 'z') || (v[k] >= '0' && v[k] <= '9') || v[k] == '_';
                if (!ok) { snprintf(err, cap, "line %d: level: \"%s\" must be a level name like level95", line, v); goto bad; }
                memcpy(p->level, v, vn + 1);
            } else {
                char *end;
                unsigned long u = strtoul(v, &end, 0);
                if (end == v || *end || (word[0] == 'm' ? (u < 1 || u > 255) : u > 0xffff)) {
                    snprintf(err, cap, "line %d: %s: \"%s\" must be %s", line, word, v, word[0] == 'm' ? "a MATERIAL id 1-255" : "a number 0-0xffff");
                    goto bad;
                }
                if (word[0] == 'm') mat = (uint8_t)u;
                else flags = (uint16_t)u;
            }
            continue;
        }
        static const struct { const char *name; int n; } shapes[] = {
            {"floor", 5}, {"ceiling", 5}, {"wall", 6}, {"box", 6}, {"tri", 9}, {"remove", 6}};
        int k = -1;
        for (int j = 0; j < 6; j++) if (!strcmp(word, shapes[j].name)) k = j;
        if (k < 0) { snprintf(err, cap, "line %d: unknown keyword \"%s\"", line, word); goto bad; }
        if (!p->level[0]) { snprintf(err, cap, "line %d: level = NAME must come first", line); goto bad; }
        float v[9];
        if (numbers(s + strlen(shapes[k].name), v, shapes[k].n)) {
            snprintf(err, cap, "line %d: %s needs %d numbers", line, shapes[k].name, shapes[k].n);
            goto bad;
        }
        int r = 0;
        switch (k) {
        case 0: r = add_flat(p, v[0], v[1], v[2], v[3], v[4], 1, flags, mat); break;
        case 1: r = add_flat(p, v[0], v[1], v[2], v[3], v[4], 0, flags, mat); break;
        case 2:
            if (v[4] >= v[5] || (v[0] == v[2] && v[1] == v[3])) { snprintf(err, cap, "line %d: wall needs two different ends and Z0 < Z1", line); goto bad; }
            r = add_wall(p, v[0], v[1], v[2], v[3], v[4], v[5], flags, mat);
            break;
        case 3:
        case 5: {
            for (int a = 0; a < 3; a++)
                if (v[a] >= v[a + 3]) { snprintf(err, cap, "line %d: %s needs X0 < X1, Y0 < Y1, Z0 < Z1", line, shapes[k].name); goto bad; }
            if (k == 5) {
                if (p->nremove >= COLPATCH_REMOVE_MAX) { snprintf(err, cap, "line %d: more than %d removals", line, COLPATCH_REMOVE_MAX); goto bad; }
                ColBox *b = &p->remove[p->nremove++];
                memcpy(b->lo, v, 12), memcpy(b->hi, v + 3, 12);
                break;
            }
            r = add_wall(p, v[0], v[1], v[3], v[1], v[2], v[5], flags, mat) || add_wall(p, v[3], v[1], v[3], v[4], v[2], v[5], flags, mat) ||
                add_wall(p, v[3], v[4], v[0], v[4], v[2], v[5], flags, mat) || add_wall(p, v[0], v[4], v[0], v[1], v[2], v[5], flags, mat) ||
                add_flat(p, v[0], v[1], v[3], v[4], v[5], 1, flags, mat);
            break;
        }
        default: r = add_tri(p, v, v + 3, v + 6, flags, mat); break;
        }
        if (r) { snprintf(err, cap, "line %d: more than %d triangles", line, COLPATCH_TRI_MAX); goto bad; }
    }
    if (!p->level[0]) { snprintf(err, cap, "level = NAME is missing"); goto bad; }
    if (!p->ntri && !p->nremove) { snprintf(err, cap, "no shapes"); goto bad; }
    return 0;
bad:
    colpatch_free(p);
    return -1;
}

/* ---- the world ---- */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void grid_point(const uint8_t *w, const float p[3], float g[3]) {
    for (int a = 0; a < 3; a++) {
        float v = rdf(w, COLW_MATRIX + 48 + 4 * a);   /* translation row */
        for (int r = 0; r < 3; r++) v += p[r] * rdf(w, COLW_MATRIX + 16 * r + 4 * a);
        g[a] = clampf(v, rdf(w, COLW_GMIN + 4 * a), rdf(w, COLW_GMAX + 4 * a));
    }
}
static void dims(const uint8_t *w, uint32_t n[3]) {
    n[0] = rd16(w, COLW_NX), n[1] = rd16(w, COLW_NY), n[2] = rd32(w, COLW_NZ);
}
void colpatch_cell(const uint8_t *w, const float p[3], uint32_t c[3]) {
    float g[3];
    uint32_t n[3];
    grid_point(w, p, g);
    dims(w, n);
    for (int a = 0; a < 3; a++) {
        int v = (int)g[a];
        c[a] = v < 0 ? 0 : (uint32_t)v >= n[a] ? (n[a] ? n[a] - 1 : 0) : (uint32_t)v;
    }
}
/* The cells [lo, hi] (inclusive) of a triangle's grid-space box. */
static void tri_cells(const uint8_t *w, const float t[3][3], uint32_t lo[3], uint32_t hi[3]) {
    for (int a = 0; a < 3; a++) lo[a] = UINT32_MAX, hi[a] = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t c[3];
        colpatch_cell(w, t[k], c);
        for (int a = 0; a < 3; a++) {
            if (c[a] < lo[a]) lo[a] = c[a];
            if (c[a] > hi[a]) hi[a] = c[a];
        }
    }
}
static int inside(const ColBox *b, const float *v) {
    for (int a = 0; a < 3; a++) if (v[a] < b->lo[a] || v[a] > b->hi[a]) return 0;
    return 1;
}

int colpatch_apply(uint8_t *w, const ColPatch *const *patches, int npatches, void **block, uint32_t *removed) {
    *block = NULL;
    if (removed) *removed = 0;
    uint32_t n[3];
    dims(w, n);
    const uint32_t ntri = rd16(w, COLW_NTRI), nvert = rd32(w, COLW_NVERT), listlen = rd32(w, COLW_LISTLEN);
    const uint32_t ncell = n[0] * n[1] * n[2];
    const uint8_t *otri = HP(rd32(w, COLW_TRIS)), *overt = HP(rd32(w, COLW_VERTS));
    const uint32_t *ooff = (const uint32_t *)HP(rd32(w, COLW_OFFSETS));
    const uint16_t *olist = (const uint16_t *)HP(rd32(w, COLW_LISTS));
    uint32_t add = 0;
    for (int i = 0; i < npatches; i++) add += patches[i]->ntri;
    if (!ncell || ntri + add > 0xffff || nvert + 3 * add > 0xffff) return -1;
    /* removals first: they apply to the level's own triangles only */
    for (int i = 0; i < npatches; i++)
        for (uint32_t r = 0; r < patches[i]->nremove; r++)
            for (uint32_t t = 0; t < ntri; t++) {
                const uint8_t *rec = otri + 10 * (size_t)t;
                int all = 1;
                for (int k = 0; k < 3 && all; k++) {
                    uint16_t vi = rd16(rec, 2 * k);
                    all = vi < nvert && inside(&patches[i]->remove[r], (const float *)(overt + 16 * (size_t)vi));
                }
                uint16_t fl = rd16(rec, 6);
                if (all && (fl & COLPATCH_FLAG_SOLID)) {
                    wr16((uint8_t *)rec, 6, (uint16_t)(fl & ~COLPATCH_FLAG_SOLID));
                    if (removed) (*removed)++;
                }
            }
    if (!add) return 0;
    /* per cell: how many new triangles it gains */
    uint32_t *extra = calloc(ncell, sizeof *extra);
    if (!extra) return -1;
    const ColTri **all = malloc(add * sizeof *all);
    if (!all) { free(extra); return -1; }
    uint32_t k = 0;
    for (int i = 0; i < npatches; i++)
        for (uint32_t t = 0; t < patches[i]->ntri; t++) all[k++] = &patches[i]->tri[t];
    uint64_t newlen = listlen ? listlen : 1;
    for (uint32_t t = 0; t < add; t++) {
        uint32_t lo[3], hi[3];
        tri_cells(w, all[t]->p, lo, hi);
        for (uint32_t z = lo[2]; z <= hi[2]; z++)
            for (uint32_t y = lo[1]; y <= hi[1]; y++)
                for (uint32_t x = lo[0]; x <= hi[0]; x++) extra[(z * n[1] + y) * n[0] + x]++;
    }
    for (uint32_t c = 0; c < ncell; c++)
        if (extra[c]) newlen += extra[c] + (ooff[c] ? 0 : 1);
    const uint32_t nt = ntri + add, nv = nvert + 3 * add;
    const size_t vbytes = 16 * (size_t)nv, tbytes = (10 * (size_t)(nt + 1) + 15) & ~(size_t)15;
    const size_t bbytes = ((((size_t)nt + 7) / 8 + 15) & ~(size_t)15) + 16, obytes = 4 * (size_t)ncell, lbytes = 2 * (size_t)newlen;
    uint8_t *m = newlen > 0xffffffffu ? NULL : calloc(1, vbytes + tbytes + bbytes + obytes + lbytes + 16);
    if (!m) { free(all); free(extra); return -1; }
    uint8_t *vert = (uint8_t *)(((uintptr_t)m + 15) & ~(uintptr_t)15), *tri = vert + vbytes, *bits = tri + tbytes;
    uint32_t *off = (uint32_t *)(bits + bbytes);
    uint16_t *list = (uint16_t *)(off + ncell);
    memcpy(vert, overt, 16 * (size_t)nvert);
    memcpy(tri, otri, 10 * (size_t)ntri);
    for (uint32_t t = 0; t < add; t++) {
        uint8_t *rec = tri + 10 * (size_t)(ntri + t);
        for (int c = 0; c < 3; c++) {
            const uint32_t vi = nvert + 3 * t + c;
            const float v4[4] = {all[t]->p[c][0], all[t]->p[c][1], all[t]->p[c][2], 1.0f};
            memcpy(vert + 16 * (size_t)vi, v4, 16);
            wr16(rec, 2 * c, (uint16_t)vi);
        }
        wr16(rec, 6, all[t]->flags);
        rec[8] = all[t]->material;
        rec[9] = 0;
    }
    /* lists: each cell's old list, then its new triangles (indices stay increasing) */
    uint32_t pos = 1;   /* index 0 stays 0, as in the level files (offset 0 = empty) */
    uint32_t *at = calloc(ncell, sizeof *at);   /* where each cell's next new index goes */
    if (!at) { free(m); free(all); free(extra); return -1; }
    for (uint32_t c = 0; c < ncell; c++) {
        const uint32_t o = ooff[c], cnt = o ? olist[o] : 0;
        if (!cnt && !extra[c]) { off[c] = 0; continue; }
        off[c] = pos;
        list[pos] = (uint16_t)(cnt + extra[c]);
        if (cnt) memcpy(list + pos + 1, olist + o + 1, 2 * (size_t)cnt);
        at[c] = pos + 1 + cnt;
        pos += 1 + cnt + extra[c];
    }
    for (uint32_t t = 0; t < add; t++) {
        uint32_t lo[3], hi[3];
        tri_cells(w, all[t]->p, lo, hi);
        for (uint32_t z = lo[2]; z <= hi[2]; z++)
            for (uint32_t y = lo[1]; y <= hi[1]; y++)
                for (uint32_t x = lo[0]; x <= hi[0]; x++) list[at[(z * n[1] + y) * n[0] + x]++] = (uint16_t)(ntri + t);
    }
    free(at);
    free(all);
    free(extra);
    /* the header last: arrays complete before any query can see them */
    wr32(w, COLW_VERTS, HA(vert));
    wr32(w, COLW_TRIS, HA(tri));
    wr32(w, COLW_VISITED, HA(bits));
    wr32(w, COLW_OFFSETS, HA(off));
    wr32(w, COLW_LISTS, HA(list));
    wr32(w, COLW_LISTLEN, pos);
    wr32(w, COLW_NVERT, nv);
    wr16(w, COLW_NTRI, (uint16_t)nt);
    *block = m;
    return (int)add;
}

/* ---- run time ---- */

#define PATCH_MAX 16
#define WORLD_PTR 0x005af590u
typedef struct {
    char mod[41];
    ColPatch p;
    int level_read;            /* the level file was read (ok or not) */
    uint32_t lev_ntri;         /* the level file's records (0: unreadable) */
    uint8_t *lev_corners;      /* their corner indices (bytes 0-5 of each record), malloc'd */
} Reg;
static Reg regs[PATCH_MAX];
static int nregs;
static char disc_dir[1024];
static uint32_t seen_w, seen_tri;       /* the world and records last looked at */
static void *patched_block;             /* our arrays, while the world uses them */
static uint32_t patched_w, patched_tri, base_ntri;

int colpatch_register(const char *mod_id, const char *text, size_t len) {
    char err[160];
    if (nregs >= PATCH_MAX) { printf("[collision] %s: more than %d patches, ignored\n", mod_id, PATCH_MAX); return -1; }
    Reg *r = &regs[nregs];
    memset(r, 0, sizeof *r);
    if (colpatch_parse(text, len, &r->p, err, sizeof err)) {
        printf("[collision] %s: %s\n", mod_id, err);
        return -1;
    }
    snprintf(r->mod, sizeof r->mod, "%s", mod_id);
    printf("[collision] %s: %s, %u triangles, %u removals\n", mod_id, r->p.level, r->p.ntri, r->p.nremove);
    nregs++;
    return 0;
}
void colpatch_reset(void) {
    for (int i = 0; i < nregs; i++) colpatch_free(&regs[i].p), free(regs[i].lev_corners);
    nregs = 0;
    seen_w = seen_tri = 0;
    free(patched_block);   /* tests only: a running title never resets */
    patched_block = NULL;
    patched_w = patched_tri = base_ntri = 0;
}
int colpatch_count(void) { return nregs; }
void colpatch_set_disc(const char *disc) { snprintf(disc_dir, sizeof disc_dir, "%s", disc ? disc : ""); }
uint32_t colpatch_base_ntri(void) { return patched_block ? base_ntri : 0; }

static void read_level(Reg *r) {
    r->level_read = 1;
    char name[48];
    snprintf(name, sizeof name, "%s.lev", r->p.level);
    size_t n = 0;
    uint8_t *buf = disc_dir[0] ? mat_wad_read(disc_dir, name, &n) : NULL;
    MatMesh m;
    if (buf && !mat_mesh_find(buf, n, &m) && m.ntri && (r->lev_corners = malloc(6 * (size_t)m.ntri))) {
        for (uint32_t t = 0; t < m.ntri; t++) memcpy(r->lev_corners + 6 * (size_t)t, m.tri + 10 * (size_t)t, 6);
        r->lev_ntri = m.ntri;
    }
    free(buf);
    if (!r->lev_ntri) printf("[collision] %s: %s not found on the disc, patch unused\n", r->mod, name);
}

int colpatch_frame(int (*readable)(uint32_t addr, uint32_t len)) {
    if (!nregs || !readable(WORLD_PTR, 4)) return 0;
    const uint32_t g = *(const uint32_t *)HP(WORLD_PTR);
    if (g < 0x10000u || !readable(g + 0x10, 4)) return 0;
    const uint32_t a = *(const uint32_t *)HP(g + 0x10);
    if (a < 0x10000u || !readable(a + 4, 4)) return 0;
    const uint32_t w = *(const uint32_t *)HP(a + 4);
    if (w < 0x10000u || !readable(w, COLW_SIZE)) return 0;
    uint8_t *wp = HP(w);
    const uint32_t tri = rd32(wp, COLW_TRIS);
    if (w == seen_w && tri == seen_tri) return 0;
    seen_w = w, seen_tri = tri;
    if (patched_block && w == patched_w && tri == patched_tri) return 0;
    if (patched_block) {   /* the title loaded another world: ours is no longer referenced */
        free(patched_block);
        patched_block = NULL;
        patched_w = patched_tri = base_ntri = 0;
    }
    const uint32_t n = rd16(wp, COLW_NTRI);
    if (!n || tri < 0x10000u || !readable(tri, 10 * n)) return 0;
    const ColPatch *use[PATCH_MAX];
    int nuse = 0;
    const char *level = NULL;
    for (int i = 0; i < nregs; i++) {
        Reg *r = &regs[i];
        if (!r->level_read) read_level(r);
        /* the level's records: as many as the world counts (a padding record may follow in the file) */
        if (!r->lev_ntri || r->lev_ntri < n || r->lev_ntri > n + 1) continue;
        const uint8_t *mem = HP(tri);
        int same = 1;
        for (uint32_t t = 0; t < n && same; t++) same = !memcmp(r->lev_corners + 6 * (size_t)t, mem + 10 * (size_t)t, 6);
        if (!same) continue;
        if (level && strcmp(level, r->p.level)) continue;
        level = r->p.level;
        use[nuse++] = &r->p;
    }
    if (!nuse) return 0;
    const uint32_t nx = rd16(wp, COLW_NX), ny = rd16(wp, COLW_NY), nz = rd32(wp, COLW_NZ);
    if (!nx || !ny || !nz || nz > 64 || !readable(rd32(wp, COLW_OFFSETS), 4 * nx * ny * nz) || !readable(rd32(wp, COLW_LISTS), 2 * rd32(wp, COLW_LISTLEN)) ||
        !readable(rd32(wp, COLW_VERTS), 16 * rd32(wp, COLW_NVERT))) {
        printf("[collision] %s: the title's collision world at 0x%08x is not as expected, not patched\n", level, w);
        return 0;
    }
    void *block = NULL;
    uint32_t removed = 0;
    int added = colpatch_apply(wp, use, nuse, &block, &removed);
    if (added < 0) {
        printf("[collision] %s: the patches do not fit the world (%u triangles), not applied\n", level, n);
        return 0;
    }
    seen_tri = rd32(wp, COLW_TRIS);
    if (block) patched_block = block, patched_w = w, patched_tri = seen_tri, base_ntri = n;
    printf("[collision] %s: %d patch%s applied: %d triangles added (%u -> %u), %u made non-solid\n", level, nuse, nuse == 1 ? "" : "es",
           added, n, (uint32_t)rd16(wp, COLW_NTRI), removed);
    return 1;
}
