/* --materials: options, the level's collision mesh and its top-down material map (materials.h). */
#include "materials.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int materials_arg(MaterialsConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--materials") || !strcmp(opt, "--no-materials")) {
        c->on = opt[2] == 'm';
        return 1;
    }
    if (!strcmp(opt, "--materials-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= MATERIALS_STRENGTH_MIN && f <= MATERIALS_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

int materials_wet_build(const MaterialsConfig *c, int has_map, const float view[16], const float proj[16], RRainWet *w) {
    if (!c->on || !has_map || !(c->strength > 0) || !(proj[15] == 0 && fabsf(proj[11]) == 1)) return 0;
    const float back = -proj[11];   /* view z towards the viewer (as rain_wet_build) */
    for (int j = 0; j < 3; j++) {   /* as rain_puddle_build */
        w->right[j] = view[4 * j];
        w->up_v[j] = view[4 * j + 1];
        w->back[j] = back * view[4 * j + 2];
        w->eye[j] = -(view[12] * view[4 * j] + view[13] * view[4 * j + 1] + view[14] * view[4 * j + 2]);
    }
    w->materials = c->strength;
    return 1;
}

int mat_class(uint8_t id) {
    switch (id) {
    case 6:    /* ASHPHALT */
    case 105:  /* ROOFTOP (tar) */
        return MAT_ASPHALT;
    case 5:    /* CONCRETE */
    case 41:   /* SKID (loading-dock trim) */
        return MAT_CONCRETE;
    case 36:   /* DIRT */
    case 106:  /* GRAVEL */
    case 24:   /* RUBBLE */
    case 115:  /* GARBAGE */
        return MAT_DIRT;
    case 35:   /* GRASS */
    case 114:  /* VEGETATION */
        return MAT_GRASS;
    case 16:   /* WOOD */
    case 47:   /* WOODFURNITURE */
    case 153:  /* CHAINWOOD */
    case 179:  /* CLIMB_WOOD */
        return MAT_WOOD;
    case 3:    /* STEEL */
    case 4:    /* ALUMINIUM */
    case 7:    /* LAMPOST */
    case 12:   /* DUMPSTER */
    case 13:   /* CAR_HOOD */
    case 18:   /* TRAINRAIL */
    case 28:   /* DRUM */
    case 107:  /* CHAINLINK_NOCLIMB */
    case 118:  /* STEELWALL */
    case 122:  /* RAILING */
    case 152:  /* CHAINMETAL */
        return MAT_METAL;
    case 15:   /* DRYWALL */
    case 116:  /* BRICKWALL */
        return MAT_BRICK;
    case 2:    /* GLASS */
        return MAT_GLASS;
    default:
        return MAT_UNKNOWN;
    }
}

const char *mat_class_name(int cls) {
    static const char *const n[MAT_CLASSES] = {"unknown", "asphalt", "concrete", "dirt", "grass", "wood", "metal", "brick", "glass"};
    return cls >= 0 && cls < MAT_CLASSES ? n[cls] : "unknown";
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) {
    const uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

#define PACK_MAGIC 0xDE686795u
#define CLASS_TRIS 4u
#define CLASS_VERTS 7u

/* The item at p (end: the buffer's end): its chunks into m when it holds both collision chunks.
 * Returns the offset after the item, 0 when it does not fit; *found 1 with the mesh. */
static size_t item_at(const uint8_t *b, size_t n, size_t p, MatMesh *m, int *found) {
    *found = 0;
    if (p + 16 > n || rd32(b + p + 8)) return 0;
    const uint32_t total = rd32(b + p + 4);
    p += 16;
    uint32_t acc = 0;
    const uint8_t *tri = NULL, *vert = NULL;
    uint32_t ts = 0, vs = 0;
    int chunks = 0;
    while (acc < total) {
        if (p + 16 > n) return 0;
        const uint32_t c = rd32(b + p), s = rd32(b + p + 4);
        if (rd32(b + p + 8) || s > total - acc || s > n - p - 16) return 0;
        if (c == CLASS_TRIS) tri = b + p + 16, ts = s;
        if (c == CLASS_VERTS) vert = b + p + 16, vs = s;
        acc += s;
        p += 16 + s;
        chunks++;
    }
    if (!chunks) return 0;
    if (tri && vert && ts >= 10 && vs >= 16) {
        m->tri = tri, m->ntri = ts / 10;
        m->vert = vert, m->nvert = vs / 16;
        *found = 1;
    }
    return p;
}

int mat_mesh_find(const uint8_t *buf, size_t n, MatMesh *m) {
    memset(m, 0, sizeof *m);
    if (!buf || n < 32) return -1;
    int found = 0;
    if (!rd32(buf + 8) && rd32(buf + 12) == PACK_MAGIC) {
        const uint32_t count = rd32(buf);
        size_t p = 16;
        for (uint32_t i = 0; i < count; i++) {
            p = item_at(buf, n, p, m, &found);
            if (!p) break;
            if (found) return 0;
        }
        memset(m, 0, sizeof *m);
        return -1;
    }
    if (item_at(buf, n, 0, m, &found) && found) return 0;
    memset(m, 0, sizeof *m);
    return -1;
}

/* Vertex i (x, y, z). */
static void vtx(const MatMesh *m, uint32_t i, float v[3]) {
    for (int k = 0; k < 3; k++) v[k] = rdf(m->vert + 16 * i + 4 * k);
}
/* Triangle t's corners and material: 0 with an out-of-range index or a non-finite or far corner;
 * else 1 when it faces up (normal z above MAT_UP of its length), 2 otherwise (a wall), and 3 when it
 * is degenerate (no normal). */
static int read_tri(const MatMesh *m, uint32_t t, float p[3][3], uint8_t *mat) {
    const uint8_t *r = m->tri + 10 * t;
    *mat = r[8];
    for (int k = 0; k < 3; k++) {
        const uint16_t i = rd16(r + 2 * k);
        if (i >= m->nvert) return 0;
        vtx(m, i, p[k]);
        if (!isfinite(p[k][0]) || !isfinite(p[k][1]) || !isfinite(p[k][2]) || fabsf(p[k][0]) > 1e5f || fabsf(p[k][1]) > 1e5f || fabsf(p[k][2]) > 1e5f)
            return 0;
    }
    const float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
    const float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
    const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    const float len = sqrtf(nx * nx + ny * ny + nz * nz);
    return !(len > 1e-8f) ? 3 : nz > MAT_UP * len ? 1 : 2;
}
int mat_tri_read(const MatMesh *m, uint32_t t, float p[3][3], uint8_t *mat) { return t < m->ntri ? read_tri(m, t, p, mat) : 0; }
/* Triangle t's corners and material when it faces up, has valid indices and finite corners and is
 * not a camera ghost: 1, else 0. */
static int up_tri(const MatMesh *m, uint32_t t, float p[3][3], uint8_t *mat) {
    return read_tri(m, t, p, mat) == 1 && *mat != MAT_ID_CAMERA_GHOST;
}

/* The two layers per cell while building. */
typedef struct {
    float z[2];
    uint8_t cls[2], has[2];
} Cell;
/* A surface at height z of class cls in cell c: the top one, or the highest at least MAT_LAYER_GAP
 * below it. */
static void cell_put(Cell *c, float z, uint8_t cls) {
    if (!c->has[0]) {
        c->z[0] = z, c->cls[0] = cls, c->has[0] = 1;
    } else if (z > c->z[0]) {
        if (z - c->z[0] >= MAT_LAYER_GAP) c->z[1] = c->z[0], c->cls[1] = c->cls[0], c->has[1] = 1;
        c->z[0] = z, c->cls[0] = cls;
    } else if (c->z[0] - z >= MAT_LAYER_GAP && (!c->has[1] || z > c->z[1])) {
        c->z[1] = z, c->cls[1] = cls, c->has[1] = 1;
    }
}

void mat_map_free(MatMap *map) {
    free(map->texels);
    memset(map, 0, sizeof *map);
}

int mat_map_build(const MatMesh *m, MatMap *map) {
    memset(map, 0, sizeof *map);
    if (!m || !m->tri || !m->vert) return -1;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, p[3][3];
    uint8_t mat;
    uint32_t ups = 0;
    for (uint32_t t = 0; t < m->ntri; t++) {
        if (!up_tri(m, t, p, &mat)) continue;
        ups++;
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++) {
                if (p[k][a] < lo[a]) lo[a] = p[k][a];
                if (p[k][a] > hi[a]) hi[a] = p[k][a];
            }
    }
    if (!ups) return -1;
    float cell = MAT_CELL;
    const float span = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    if (span / cell > MAT_MAP_MAX - 2) cell = span / (MAT_MAP_MAX - 2);
    const float x0 = floorf(lo[0] / cell) * cell, y0 = floorf(lo[1] / cell) * cell;
    int w = (int)((hi[0] - x0) / cell) + 1, h = (int)((hi[1] - y0) / cell) + 1;
    if (w > MAT_MAP_MAX) w = MAT_MAP_MAX;
    if (h > MAT_MAP_MAX) h = MAT_MAP_MAX;
    Cell *cells = calloc((size_t)w * h, sizeof *cells);
    uint16_t *tex = malloc((size_t)w * h * 2 * sizeof *tex);
    if (!cells || !tex) {
        free(cells);
        free(tex);
        return -1;
    }
    for (uint32_t t = 0; t < m->ntri; t++) {
        if (!up_tri(m, t, p, &mat)) continue;
        const uint8_t cls = (uint8_t)mat_class(mat);
        /* The plane z = p0.z + a (x - p0.x) + b (y - p0.y), barycentrics in x, y. */
        const float d = (p[1][1] - p[2][1]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][1] - p[2][1]);
        if (fabsf(d) < 1e-12f) continue;
        float bx0 = fminf(p[0][0], fminf(p[1][0], p[2][0])), bx1 = fmaxf(p[0][0], fmaxf(p[1][0], p[2][0]));
        float by0 = fminf(p[0][1], fminf(p[1][1], p[2][1])), by1 = fmaxf(p[0][1], fmaxf(p[1][1], p[2][1]));
        int cx0 = (int)floorf((bx0 - x0) / cell), cx1 = (int)floorf((bx1 - x0) / cell);
        int cy0 = (int)floorf((by0 - y0) / cell), cy1 = (int)floorf((by1 - y0) / cell);
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 >= w) cx1 = w - 1;
        if (cy1 >= h) cy1 = h - 1;
        int hits = 0;
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) {
                const float x = x0 + (cx + 0.5f) * cell, y = y0 + (cy + 0.5f) * cell;
                const float l1 = ((p[1][1] - p[2][1]) * (x - p[2][0]) + (p[2][0] - p[1][0]) * (y - p[2][1])) / d;
                const float l2 = ((p[2][1] - p[0][1]) * (x - p[2][0]) + (p[0][0] - p[2][0]) * (y - p[2][1])) / d;
                const float l3 = 1 - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                cell_put(&cells[(size_t)cy * w + cx], l1 * p[0][2] + l2 * p[1][2] + l3 * p[2][2], cls);
                hits++;
            }
        if (hits) continue;
        /* A small triangle that holds no cell centre: its centroid's cell. */
        const float gx = (p[0][0] + p[1][0] + p[2][0]) / 3, gy = (p[0][1] + p[1][1] + p[2][1]) / 3, gz = (p[0][2] + p[1][2] + p[2][2]) / 3;
        const int ix = (int)floorf((gx - x0) / cell), iy = (int)floorf((gy - y0) / cell);
        if (ix >= 0 && iy >= 0 && ix < w && iy < h) cell_put(&cells[(size_t)iy * w + ix], gz, cls);
    }
    const float zstep = fmaxf((hi[2] - lo[2]) / (MAT_HEIGHT_MAX - 1), MAT_ZSTEP_MIN);
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int l = 0; l < 2; l++) {
            const float q = roundf((cells[i].z[l] - lo[2]) / zstep);
            const int hb = q < 0 ? 1 : q > MAT_HEIGHT_MAX - 1 ? MAT_HEIGHT_MAX : (int)q + 1;
            tex[2 * i + l] = cells[i].has[l] ? (uint16_t)(cells[i].cls[l] << 12 | hb) : 0;
        }
    free(cells);
    map->w = w, map->h = h;
    map->x0 = x0, map->y0 = y0, map->cell = cell;
    map->z0 = lo[2], map->zstep = zstep;
    map->texels = tex;
    map->surfaces = ups;
    return 0;
}

int mat_map_at(const MatMap *map, float x, float y, float z, float tol, float *hz) {
    if (!map || !map->texels || !(map->cell > 0)) return MAT_UNKNOWN;
    const float fx = floorf((x - map->x0) / map->cell), fy = floorf((y - map->y0) / map->cell);
    if (!(fx >= 0 && fy >= 0 && fx < map->w && fy < map->h)) return MAT_UNKNOWN;
    const uint16_t *t = map->texels + 2 * ((size_t)fy * map->w + (size_t)fx);
    int best = MAT_UNKNOWN;
    float bd = tol;
    for (int l = 0; l < 2; l++) {
        const int hb = t[l] & 0xfff;
        if (!hb) continue;
        const float s = map->z0 + (hb - 1) * map->zstep, dz = fabsf(z - s);
        if (dz <= bd) {
            bd = dz;
            best = t[l] >> 12;
            if (hz) *hz = s;
        }
    }
    return best;
}

/* ---- The WAD archive ---- */

uint32_t mat_wad_hash(const char *name) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    const char *pre = "ee_files\\";
    for (const char *s = pre; *s; s++) c = tab[(c ^ (uint8_t)*s) & 0xff] ^ (c >> 8);
    for (const char *s = name; *s; s++) {
        char ch = *s == '/' ? '\\' : *s;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        c = tab[(c ^ (uint8_t)ch) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint8_t *read_range(const char *path, long off, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = size ? malloc(size) : NULL;
    if (buf && (fseek(f, off, SEEK_SET) || fread(buf, 1, size, f) != size)) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

uint8_t *mat_wad_read(const char *disc, const char *name, size_t *n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/XBoxWad.idx", disc);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long len = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    if (len < 20 || len > (8l << 20)) return NULL;
    uint8_t *idx = read_range(path, 0, (size_t)len);
    if (!idx) return NULL;
    const uint32_t want = mat_wad_hash(name), un = (uint32_t)len;
    const uint32_t wads = rd32(idx), files = rd32(idx + 4), wad_table = rd32(idx + 12), file_table = rd32(idx + 16);
    uint8_t *out = NULL;
    if (wads <= 256 && wad_table <= un && (un - wad_table) / 36 >= wads && file_table <= un && (un - file_table) / 16 >= files) {
        for (uint32_t i = 0; i < files; i++) {
            const uint8_t *e = idx + file_table + 16 * i;
            const uint32_t loc = rd32(e), size = rd32(e + 4);
            if (rd32(e + 12) != want || loc >> 24 >= wads || !size || size > (64u << 20)) continue;
            char wad[33];
            memcpy(wad, idx + wad_table + 36 * (loc >> 24), 32);
            wad[32] = 0;
            snprintf(path, sizeof path, "%s/%s", disc, wad);
            out = read_range(path, (long)(loc & 0xffffff) << 10, size);
            if (out) *n = size;
            break;
        }
    }
    free(idx);
    return out;
}

int mat_level_map(const char *disc, const char *level, MatMap *map) { return mat_level_maps(disc, level, map, NULL); }

int mat_level_maps(const char *disc, const char *level, MatMap *map, MatWallMap *walls) {
    if (map) memset(map, 0, sizeof *map);
    if (walls) memset(walls, 0, sizeof *walls);
    if (!disc || !level || !*level || strlen(level) > 64) return -1;
    char name[80];
    snprintf(name, sizeof name, "%s.lev", level);
    size_t n = 0;
    uint8_t *b = mat_wad_read(disc, name, &n);
    if (!b) return -1;
    MatMesh m;
    int r = -1;
    if (!mat_mesh_find(b, n, &m)) {
        r = 0;
        if (map && mat_map_build(&m, map)) r = -1;
        if (walls && mat_wall_map_build(&m, walls)) r = -1;
    }
    free(b);
    return r;
}

/* ---- The wall map (--lamp-shadows walls) ---- */

int mat_wall_skip(uint8_t mat) {
    switch (mat) {
    case MAT_ID_CAMERA_GHOST:
    case 2:     /* GLASS */
    case 107:   /* CHAINLINK_NOCLIMB */
    case 122:   /* RAILING */
    case 152:   /* CHAINMETAL */
        return 1;
    default:
        return 0;
    }
}

/* Triangle t's corners when it is a wall of the map (not facing up, not skipped): 1, else 0. */
static int wall_tri(const MatMesh *m, uint32_t t, float p[3][3]) {
    uint8_t mat;
    return read_tri(m, t, p, &mat) == 2 && !mat_wall_skip(mat);
}

/* The part of polygon in (n corners, at most 9) where sign x (its coordinate `axis` - v) >= 0, into
 * out: its corner count. */
static int clip_poly(const float in[][3], int n, int axis, float v, float sign, float out[][3]) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        const float *a = in[i], *b = in[(i + 1) % n];
        const float da = sign * (a[axis] - v), db = sign * (b[axis] - v);
        if (da >= 0) memcpy(out[k++], a, sizeof(float[3]));
        if ((da >= 0) != (db >= 0)) {
            const float f = da / (da - db);
            for (int j = 0; j < 3; j++) out[k][j] = a[j] + f * (b[j] - a[j]);
            out[k][axis] = v;
            k++;
        }
    }
    return k;
}

void mat_wall_map_free(MatWallMap *map) {
    free(map->texels);
    free(map->wtri);
    free(map->wcells);
    free(map->wid);
    free(map->wopen);
    memset(map, 0, sizeof *map);
}

/* The cells of a w x h map (origin x0, y0) that triangle p's footprint touches, a border included on
 * both sides (empty when it lies outside). */
static MatWallRect wall_cells(const float p[3][3], float x0, float y0, float cell, int w, int h) {
    const float bx0 = fminf(p[0][0], fminf(p[1][0], p[2][0])), bx1 = fmaxf(p[0][0], fmaxf(p[1][0], p[2][0]));
    const float by0 = fminf(p[0][1], fminf(p[1][1], p[2][1])), by1 = fmaxf(p[0][1], fmaxf(p[1][1], p[2][1]));
    MatWallRect r = {(int)ceilf((bx0 - x0) / cell) - 1, (int)ceilf((by0 - y0) / cell) - 1, (int)floorf((bx1 - x0) / cell) + 1,
                     (int)floorf((by1 - y0) / cell) + 1};
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) r.x0 = r.y0 = r.x1 = r.y1 = 0;
    return r;
}

/* Widens the span of every cell of rect c (bot / top: c's cells, row by row) that triangle p crosses:
 * each row of cells it crosses, then each cell of that row, the triangle clipped to the cell (borders
 * included, so a wall on a cell border marks both sides) gives the exact lowest and highest point of
 * the wall over it. */
static void wall_raster(const float p[3][3], float x0, float y0, float cell, MatWallRect c, float *bot, float *top) {
    const float by0 = fminf(p[0][1], fminf(p[1][1], p[2][1])), by1 = fmaxf(p[0][1], fmaxf(p[1][1], p[2][1]));
    int cy0 = (int)ceilf((by0 - y0) / cell) - 1, cy1 = (int)floorf((by1 - y0) / cell);   /* on a border: both cells */
    if (cy0 < c.y0) cy0 = c.y0;
    if (cy1 >= c.y1) cy1 = c.y1 - 1;
    const int cw = c.x1 - c.x0;
    for (int cy = cy0; cy <= cy1; cy++) {
        float r0[9][3], r1[9][3], c0[9][3], c1[9][3];
        int n = clip_poly((const float (*)[3])p, 3, 1, y0 + cy * cell, 1, r0);
        n = clip_poly((const float (*)[3])r0, n, 1, y0 + (cy + 1) * cell, -1, r1);
        if (!n) continue;
        float bx0 = r1[0][0], bx1 = r1[0][0];
        for (int k = 1; k < n; k++) bx0 = fminf(bx0, r1[k][0]), bx1 = fmaxf(bx1, r1[k][0]);
        int cx0 = (int)ceilf((bx0 - x0) / cell) - 1, cx1 = (int)floorf((bx1 - x0) / cell);
        if (cx0 < c.x0) cx0 = c.x0;
        if (cx1 >= c.x1) cx1 = c.x1 - 1;
        for (int cx = cx0; cx <= cx1; cx++) {
            int k = clip_poly((const float (*)[3])r1, n, 0, x0 + cx * cell, 1, c0);
            k = clip_poly((const float (*)[3])c0, k, 0, x0 + (cx + 1) * cell, -1, c1);
            const size_t i = (size_t)(cy - c.y0) * cw + (cx - c.x0);
            for (int j = 0; j < k; j++) bot[i] = fminf(bot[i], c1[j][2]), top[i] = fmaxf(top[i], c1[j][2]);
        }
    }
}

/* A cell's span (bot <= top, else no wall) as its two texels. */
static void wall_texel(float bot, float top, float z0, float zstep, uint16_t *t) {
    if (!(bot <= top)) {
        t[0] = t[1] = 0;
        return;
    }
    const float qb = floorf((bot - z0) / zstep), qt = ceilf((top - z0) / zstep);
    t[0] = (uint16_t)(qb < 0 ? 1 : qb > MAT_WALL_HEIGHT_MAX - 1 ? MAT_WALL_HEIGHT_MAX : qb + 1);
    t[1] = (uint16_t)(qt < 0 ? 1 : qt > MAT_WALL_HEIGHT_MAX - 1 ? MAT_WALL_HEIGHT_MAX : qt + 1);
}

uint32_t mat_tri_sum(const uint8_t *tri, uint32_t ntri) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < 4; k++) h = (h ^ (ntri >> 8 * k & 0xff)) * 16777619u;
    for (uint32_t t = 0; tri && t < ntri; t++)
        for (int k = 0; k < 6; k++) h = (h ^ tri[10 * (size_t)t + k]) * 16777619u;
    return h;
}

uint32_t mat_tri_open(const uint8_t *tri, uint32_t ntri, uint16_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t t = 0; tri && t < ntri && t <= 0xffff; t++) {
        if (rd16(tri + 10 * (size_t)t + 6) & MAT_TRI_SOLID) continue;
        if (n < max) out[n] = (uint16_t)t;
        n++;
    }
    return n;
}

int mat_wall_map_build(const MatMesh *m, MatWallMap *map) {
    memset(map, 0, sizeof *map);
    if (!m || !m->tri || !m->vert) return -1;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, p[3][3];
    uint32_t walls = 0;
    for (uint32_t t = 0; t < m->ntri; t++) {
        if (!wall_tri(m, t, p)) continue;
        walls++;
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++) {
                if (p[k][a] < lo[a]) lo[a] = p[k][a];
                if (p[k][a] > hi[a]) hi[a] = p[k][a];
            }
    }
    if (!walls) return -1;
    float cell = MAT_WALL_CELL;
    const float span = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    if (span / cell > MAT_WALL_MAP_MAX - 4) cell = span / (MAT_WALL_MAP_MAX - 4);
    /* One cell of margin: a wall on the bounds' cell border marks the cell outside it too. */
    const float x0 = floorf(lo[0] / cell) * cell - cell, y0 = floorf(lo[1] / cell) * cell - cell;
    int w = (int)((hi[0] - x0) / cell) + 2, h = (int)((hi[1] - y0) / cell) + 2;
    if (w > MAT_WALL_MAP_MAX) w = MAT_WALL_MAP_MAX;
    if (h > MAT_WALL_MAP_MAX) h = MAT_WALL_MAP_MAX;
    float *bot = malloc((size_t)w * h * 2 * sizeof *bot);
    uint16_t *tex = malloc((size_t)w * h * 2 * sizeof *tex);
    float (*wtri)[3][3] = malloc(walls * sizeof *wtri);
    MatWallRect *wcells = malloc(walls * sizeof *wcells);
    uint32_t *wid = malloc(walls * sizeof *wid);
    uint8_t *wopen = calloc(walls, 1);
    if (!bot || !tex || !wtri || !wcells || !wid || !wopen) {
        free(bot);
        free(tex);
        free(wtri);
        free(wcells);
        free(wid);
        free(wopen);
        return -1;
    }
    float *top = bot + (size_t)w * h;
    for (size_t i = 0; i < (size_t)w * h; i++) bot[i] = INFINITY, top[i] = -INFINITY;
    const MatWallRect all = {0, 0, w, h};
    uint32_t k = 0;
    for (uint32_t t = 0; t < m->ntri && k < walls; t++) {
        if (!wall_tri(m, t, p)) continue;
        memcpy(wtri[k], p, sizeof p);
        wcells[k] = wall_cells((const float (*)[3])p, x0, y0, cell, w, h);
        wid[k++] = t;
        wall_raster((const float (*)[3])p, x0, y0, cell, all, bot, top);
    }
    const float zstep = fmaxf((hi[2] - lo[2]) / (MAT_WALL_HEIGHT_MAX - 2), MAT_WALL_ZSTEP_MIN);
    for (size_t i = 0; i < (size_t)w * h; i++) wall_texel(bot[i], top[i], lo[2], zstep, tex + 2 * i);
    free(bot);
    map->w = w, map->h = h;
    map->x0 = x0, map->y0 = y0, map->cell = cell;
    map->z0 = lo[2], map->zstep = zstep;
    map->texels = tex;
    map->walls = walls;
    map->ntri = m->ntri;
    map->tri_sum = mat_tri_sum(m->tri, m->ntri);
    map->wtri = wtri;
    map->wcells = wcells;
    map->wid = wid;
    map->wopen = wopen;
    return 0;
}

int mat_wall_map_set_open(MatWallMap *map, const uint16_t *tri, uint32_t n, int open, MatWallRect *r) {
    MatWallRect u = {0, 0, 0, 0};
    int changed = 0;
    if (map && map->texels && map->wid && tri) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = 0, b = map->walls;   /* the wall with collision index tri[i] (wid increases) */
            while (a < b) {
                const uint32_t mid = a + (b - a) / 2;
                if (map->wid[mid] < tri[i]) a = mid + 1;
                else b = mid;
            }
            if (a >= map->walls || map->wid[a] != tri[i] || map->wopen[a] == !!open) continue;
            map->wopen[a] = (uint8_t)!!open;
            changed++;
            const MatWallRect c = map->wcells[a];
            if (c.x1 <= c.x0) continue;
            if (u.x1 <= u.x0) u = c;
            else {
                if (c.x0 < u.x0) u.x0 = c.x0;
                if (c.y0 < u.y0) u.y0 = c.y0;
                if (c.x1 > u.x1) u.x1 = c.x1;
                if (c.y1 > u.y1) u.y1 = c.y1;
            }
        }
    }
    if (u.x1 > u.x0) {
        const int cw = u.x1 - u.x0, ch = u.y1 - u.y0;
        float *bot = malloc((size_t)cw * ch * 2 * sizeof *bot);
        if (bot) {
            float *top = bot + (size_t)cw * ch;
            for (size_t i = 0; i < (size_t)cw * ch; i++) bot[i] = INFINITY, top[i] = -INFINITY;
            for (uint32_t k = 0; k < map->walls; k++) {
                if (map->wopen[k]) continue;
                const MatWallRect c = map->wcells[k];
                if (c.x1 <= u.x0 || c.x0 >= u.x1 || c.y1 <= u.y0 || c.y0 >= u.y1) continue;
                wall_raster((const float (*)[3])map->wtri[k], map->x0, map->y0, map->cell, u, bot, top);
            }
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++)
                    wall_texel(bot[(size_t)y * cw + x], top[(size_t)y * cw + x], map->z0, map->zstep,
                               map->texels + 2 * ((size_t)(u.y0 + y) * map->w + (u.x0 + x)));
            free(bot);
        }
    }
    if (r) *r = u;
    return changed;
}

int mat_wall_at(const MatWallMap *map, float x, float y, float z) {
    if (!map || !map->texels || !(map->cell > 0)) return 0;
    const float fx = floorf((x - map->x0) / map->cell), fy = floorf((y - map->y0) / map->cell);
    if (!(fx >= 0 && fy >= 0 && fx < map->w && fy < map->h)) return 0;
    const uint16_t *t = map->texels + 2 * ((size_t)fy * map->w + (size_t)fx);
    if (!t[0]) return 0;
    return z >= map->z0 + (t[0] - 1) * map->zstep && z <= map->z0 + (t[1] - 1) * map->zstep;
}


/* Amanatides-Woo traversal of the cells under the segment, t in [t0, t1] of a + t (b - a). */
int mat_wall_crossings(const MatWallMap *map, const float a[3], const float b[3], float skip) {
    if (!map || !map->texels || !(map->cell > 0)) return 0;
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 2 * skip) || !(d[0] * d[0] + d[1] * d[1] > 1e-12f)) return 0;
    float t = skip > 0 ? skip / len : 0;
    const float t1 = 1 - t;
    /* clip to the map's box so a segment starting far outside it does not walk empty cells */
    const float bx[2] = {map->x0, map->x0 + map->w * map->cell}, by[2] = {map->y0, map->y0 + map->h * map->cell};
    float lo = t, hi = t1;
    for (int k = 0; k < 2; k++) {
        const float o = k ? a[1] : a[0], v = k ? d[1] : d[0], *bb = k ? by : bx;
        if (fabsf(v) < 1e-12f) { if (o < bb[0] || o >= bb[1]) return 0; continue; }
        float ta = (bb[0] - o) / v, tb = (bb[1] - o) / v;
        if (ta > tb) { const float s = ta; ta = tb; tb = s; }
        if (ta > lo) lo = ta;
        if (tb < hi) hi = tb;
    }
    if (!(lo < hi)) return 0;
    t = lo;
    const float gx = (a[0] + d[0] * t - map->x0) / map->cell, gy = (a[1] + d[1] * t - map->y0) / map->cell;
    int ix = (int)floorf(gx), iy = (int)floorf(gy);
    if (ix >= map->w) ix = map->w - 1;
    if (iy >= map->h) iy = map->h - 1;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    const int sx = d[0] > 0 ? 1 : -1, sy = d[1] > 0 ? 1 : -1;
    const float dtx = fabsf(d[0]) > 1e-12f ? map->cell / fabsf(d[0]) : INFINITY, dty = fabsf(d[1]) > 1e-12f ? map->cell / fabsf(d[1]) : INFINITY;
    float tx = fabsf(d[0]) > 1e-12f ? (map->x0 + (ix + (sx > 0)) * map->cell - a[0]) / d[0] : INFINITY;
    float ty = fabsf(d[1]) > 1e-12f ? (map->y0 + (iy + (sy > 0)) * map->cell - a[1]) / d[1] : INFINITY;
    int walls = 0, prev = 0;
    for (int guard = map->w + map->h + 2; guard-- > 0 && t < hi;) {
        const float te = fminf(fminf(tx, ty), hi);
        if (ix < 0 || iy < 0 || ix >= map->w || iy >= map->h) break;
        const uint16_t *c = map->texels + 2 * ((size_t)iy * map->w + (size_t)ix);
        int blocked = 0;
        if (c[0]) {
            const float za = a[2] + d[2] * t, zb = a[2] + d[2] * te;
            const float zlo = fminf(za, zb), zhi = fmaxf(za, zb);
            blocked = zhi >= map->z0 + (c[0] - 1) * map->zstep && zlo <= map->z0 + (c[1] - 1) * map->zstep;
        }
        if (blocked && !prev) walls++;
        prev = blocked;
        t = te;
        if (tx < ty) { ix += sx; tx += dtx; }
        else { iy += sy; ty += dty; }
    }
    return walls;
}
