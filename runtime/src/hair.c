/* --hair: see hair.h. */
#include "hair.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int hair_arg(HairConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--hair") || !strcmp(opt, "--no-hair")) {
        c->on = opt[2] == 'h';
        return 1;
    }
    if (!strcmp(opt, "--hair-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = strtof(val, &end);
        if (*end || !(f >= HAIR_STRENGTH_MIN && f <= HAIR_STRENGTH_MAX)) return -1;
        c->strength = f;
        return 2;
    }
    return 0;
}

int skin_arg(SkinConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--skin") || !strcmp(opt, "--no-skin")) {
        c->on = opt[2] == 's';
        return 1;
    }
    if (!strcmp(opt, "--skin-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = strtof(val, &end);
        if (*end || !(f >= SKIN_STRENGTH_MIN && f <= SKIN_STRENGTH_MAX)) return -1;
        c->strength = f;
        return 2;
    }
    return 0;
}

/* --eyes / --no-eyes and --eyes-strength (name: "eyes"), likewise --cloth. */
static int mat_arg(int *on, float *strength, const char *name, float lo, float hi, const char *opt, const char *val) {
    if (strncmp(opt, "--", 2)) return 0;
    const char *o = opt + 2;
    const size_t n = strlen(name);
    if (!strcmp(o, name) || (!strncmp(o, "no-", 3) && !strcmp(o + 3, name))) {
        *on = o[0] != 'n' || !strcmp(o, name);
        return 1;
    }
    if (!strncmp(o, name, n) && !strcmp(o + n, "-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = strtof(val, &end);
        if (*end || !(f >= lo && f <= hi)) return -1;
        *strength = f;
        return 2;
    }
    return 0;
}
int eyes_arg(EyesConfig *c, const char *opt, const char *val) {
    return mat_arg(&c->on, &c->strength, "eyes", EYES_STRENGTH_MIN, EYES_STRENGTH_MAX, opt, val);
}
int cloth_arg(ClothConfig *c, const char *opt, const char *val) {
    return mat_arg(&c->on, &c->strength, "cloth", CLOTH_STRENGTH_MIN, CLOTH_STRENGTH_MAX, opt, val);
}

void hair_mask_size(uint32_t w, uint32_t h, uint32_t *mw, uint32_t *mh) {
    /* halve both until the larger side fits (keeps the aspect and divides the skin's size) */
    while (w > HAIR_MASK_MAX || h > HAIR_MASK_MAX) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    *mw = w;
    *mh = h;
}

void hair_downsample(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst, uint32_t mw, uint32_t mh) {
    const uint32_t fx = w / mw, fy = h / mh, n = fx * fy;
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            uint32_t s[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < fy; j++)
                for (uint32_t i = 0; i < fx; i++) {
                    const uint32_t t = src[(size_t)(y * fy + j) * w + x * fx + i];
                    for (int c = 0; c < 4; c++) s[c] += t >> (8 * c) & 255;
                }
            uint32_t o = 0;
            for (int c = 0; c < 4; c++) o |= (s[c] + n / 2) / n << (8 * c);
            dst[(size_t)y * mw + x] = o;
        }
}

/* ---- the mask ---- */

static float smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
static void unpack(uint32_t c, float rgb[3]) {
    rgb[0] = (c >> 16 & 255) / 255.0f;
    rgb[1] = (c >> 8 & 255) / 255.0f;
    rgb[2] = (c & 255) / 255.0f;
}
/* How much a skin texel of colour c can be hair, against the face's skin colour ref: texels that
 * look like the skin (a bald head, ears, a cheek's painted shading: the skin's hue at more than about
 * half its brightness) and brightly coloured ones that no hair has (a blue or green cap, a bandana)
 * are left out; dark, grey and brown to red-blond colours stay. */
static float luma(const float *p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }
static float colour_gate(uint32_t c, const float *ref) {
    float p[3];
    unpack(c, p);
    float g = 1;
    if (ref) {
        const float sp = p[0] + p[1] + p[2], sr = ref[0] + ref[1] + ref[2], lr = luma(ref);
        if (sp > 0.03f && sr > 0.03f && lr > 0.01f) {
            float d = 0;
            for (int k = 0; k < 3; k++) d += (p[k] / sp - ref[k] / sr) * (p[k] / sp - ref[k] / sr);
            const float skin = (1 - smooth(0.025f, 0.06f, sqrtf(d))) * smooth(0.35f, 0.55f, luma(p) / lr);
            g = 1 - skin;
        }
    }
    const float mx = fmaxf(p[0], fmaxf(p[1], p[2])), mn = fminf(p[0], fminf(p[1], p[2]));
    const float sat = mx > 0.04f ? (mx - mn) / mx : 0;
    if (sat > 0.3f) {
        float hue = 0;   /* degrees */
        const float d = mx - mn;
        if (mx == p[0]) hue = 60 * fmodf((p[1] - p[2]) / d + 6, 6);
        else if (mx == p[1]) hue = 60 * ((p[2] - p[0]) / d + 2);
        else hue = 60 * ((p[0] - p[1]) / d + 4);
        const float hair_hue = smooth(2, 10, hue) * (1 - smooth(48, 60, hue));   /* red-brown to blond */
        g *= 1 - smooth(0.35f, 0.6f, sat) * (1 - hair_hue);
    }
    return g;
}

typedef struct {
    float a[2], b[2], c[2];   /* texel space, shifted to start in the first repeat */
} Tri2;
/* Calls fn for each texel of mw x mh whose centre lies inside the triangle (edges included; the
 * texture repeats: texels past an edge wrap), with its barycentric weights; a triangle that covers
 * no centre gives its centroid's texel. */
typedef void (*TexelFn)(void *ctx, uint32_t x, uint32_t y, float wa, float wb, float wc);
static void raster(const float *ua, const float *ub, const float *uc, uint32_t mw, uint32_t mh, TexelFn fn, void *ctx) {
    const float ou = floorf(fminf(ua[0], fminf(ub[0], uc[0]))), ov = floorf(fminf(ua[1], fminf(ub[1], uc[1])));
    const float ax = (ua[0] - ou) * mw, ay = (ua[1] - ov) * mh, bx = (ub[0] - ou) * mw, by = (ub[1] - ov) * mh, cx = (uc[0] - ou) * mw,
                cy = (uc[1] - ov) * mh;
    const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (!(fabsf(area) > 1e-9f)) return;
    const float x0 = fminf(ax, fminf(bx, cx)), x1 = fmaxf(ax, fmaxf(bx, cx)), y0 = fminf(ay, fminf(by, cy)), y1 = fmaxf(ay, fmaxf(by, cy));
    if (x1 - x0 > 4.0f * mw || y1 - y0 > 4.0f * mh) return;   /* a broken coordinate */
    int hit = 0;
    for (int y = (int)floorf(y0 - 0.5f); y <= (int)ceilf(y1 - 0.5f); y++)
        for (int x = (int)floorf(x0 - 0.5f); x <= (int)ceilf(x1 - 0.5f); x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float wa = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / area;
            const float wb = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / area;
            const float wc = 1 - wa - wb;
            if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) continue;
            fn(ctx, ((uint32_t)(x % (int)mw + (int)mw)) % mw, ((uint32_t)(y % (int)mh + (int)mh)) % mh, wa, wb, wc);
            hit = 1;
        }
    if (!hit) {
        const int x = (int)floorf((ax + bx + cx) / 3), y = (int)floorf((ay + by + cy) / 3);
        fn(ctx, ((uint32_t)(x % (int)mw + (int)mw)) % mw, ((uint32_t)(y % (int)mh + (int)mh)) % mh, 1.0f / 3, 1.0f / 3, 1.0f / 3);
    }
}

typedef struct {
    uint32_t mw;
    float *w, *h, *f;        /* geometry: weight, height, strand direction (2 per texel, texel space) */
    float ha, hb, hc;        /* the triangle's vertex heights */
    float dir[2];
    const uint32_t *albedo;  /* face triangles: histograms of the skin's colours */
    uint32_t (*hist)[256];
} TexCtx;
static void hair_texel(void *ctx, uint32_t x, uint32_t y, float wa, float wb, float wc) {
    TexCtx *c = ctx;
    const size_t i = (size_t)y * c->mw + x;
    c->w[i] = 1;
    c->h[i] = wa * c->ha + wb * c->hb + wc * c->hc;
    c->f[2 * i] = c->dir[0];
    c->f[2 * i + 1] = c->dir[1];
}
static void face_texel(void *ctx, uint32_t x, uint32_t y, float wa, float wb, float wc) {
    TexCtx *c = ctx;
    (void)wa, (void)wb, (void)wc;
    const uint32_t t = c->albedo[(size_t)y * c->mw + x];
    c->hist[0][t >> 16 & 255]++;
    c->hist[1][t >> 8 & 255]++;
    c->hist[2][t & 255]++;
}

static void sub3(const float *a, const float *b, float *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float norm3(float *v) {
    const float l = sqrtf(dot3(v, v));
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}
static int valid_slot(int s) { return s >= 0 && s < HAIR_SLOTS; }
static int is_face(int s) { return s >= HAIR_SLOT_FACE_FIRST && s <= HAIR_SLOT_FACE_LAST; }
static int dominant(const HairMesh *m, uint32_t v) {
    int best = -1;
    float bw = 0;
    for (int k = 0; k < 3; k++)
        if (valid_slot(m->slot[v][k]) && m->weight[v][k] > bw) bw = m->weight[v][k], best = m->slot[v][k];
    return best;
}
/* The triangle's dominant slot (summed weights of its three vertices, as regions.py). */
static int tri_slot(const HairMesh *m, const uint32_t *t) {
    float acc[HAIR_SLOTS] = {0};
    for (int j = 0; j < 3; j++)
        for (int k = 0; k < 3; k++)
            if (valid_slot(m->slot[t[j]][k]) && m->weight[t[j]][k] > 0) acc[m->slot[t[j]][k]] += m->weight[t[j]][k];
    int best = -1;
    float bw = 0;
    for (int s = 0; s < HAIR_SLOTS; s++)
        if (acc[s] > bw) bw = acc[s], best = s;
    return best;
}
/* The strand direction of a triangle in texel space: down the head (or, on the crown, back from the
 * face) in its plane, through the triangle's texture mapping. */
static void tri_flow(const HairMesh *m, const uint32_t *t, const float *fn, const float *fwd, uint32_t mw, uint32_t mh, float out[2]) {
    float d[3] = {fn[0] * fn[2], fn[1] * fn[2], -1 + fn[2] * fn[2]};   /* (0, 0, -1) minus its normal part */
    if (norm3(d) < 0.3f) {
        const float b[3] = {-fwd[0], -fwd[1], 0}, k = dot3(fn, b);
        d[0] = b[0] - k * fn[0], d[1] = b[1] - k * fn[1], d[2] = b[2] - k * fn[2];
        norm3(d);
    }
    out[0] = 0, out[1] = 1;
    float e1[3], e2[3];
    sub3(m->pos[t[1]], m->pos[t[0]], e1);
    sub3(m->pos[t[2]], m->pos[t[0]], e2);
    const float du1 = (m->uv[t[1]][0] - m->uv[t[0]][0]) * mw, dv1 = (m->uv[t[1]][1] - m->uv[t[0]][1]) * mh;
    const float du2 = (m->uv[t[2]][0] - m->uv[t[0]][0]) * mw, dv2 = (m->uv[t[2]][1] - m->uv[t[0]][1]) * mh;
    const float det = du1 * dv2 - du2 * dv1;
    if (!(fabsf(det) > 1e-9f)) return;
    float pu[3], pv[3];   /* dP/du, dP/dv per texel */
    for (int k = 0; k < 3; k++) {
        pu[k] = (e1[k] * dv2 - e2[k] * dv1) / det;
        pv[k] = (e2[k] * du1 - e1[k] * du2) / det;
    }
    /* least squares: the texel step g with pu g.x + pv g.y along d */
    const float a = dot3(pu, pu), b = dot3(pu, pv), c = dot3(pv, pv), g0 = dot3(pu, d), g1 = dot3(pv, d);
    const float gd = a * c - b * b;
    if (!(fabsf(gd) > 1e-20f)) return;
    float gx = (c * g0 - b * g1) / gd, gy = (a * g1 - b * g0) / gd;
    const float l = sqrtf(gx * gx + gy * gy);
    if (!(l > 1e-12f)) return;
    out[0] = gx / l, out[1] = gy / l;
}
/* The face's forward direction (its joints' vertices from the head's, horizontal; +y when either
 * is missing) and the head's centre (x, y). Returns 1 when the mesh has head vertices. */
static int head_frame(const HairMesh *m, float fwd[3], float centre[2]) {
    double fc[3] = {0, 0, 0}, hc[3] = {0, 0, 0};
    for (uint32_t v = 0; v < m->nv; v++) {
        const int s = dominant(m, v);
        double *a = is_face(s) ? fc : s == HAIR_SLOT_HEAD ? hc : NULL;
        if (a) a[0] += m->pos[v][0], a[1] += m->pos[v][1], a[2] += 1;
    }
    fwd[0] = 0, fwd[1] = 1, fwd[2] = 0;
    centre[0] = hc[2] > 0 ? (float)(hc[0] / hc[2]) : 0, centre[1] = hc[2] > 0 ? (float)(hc[1] / hc[2]) : 0;
    if (fc[2] > 0 && hc[2] > 0) {
        float d[3] = {(float)(fc[0] / fc[2] - hc[0] / hc[2]), (float)(fc[1] / fc[2] - hc[1] / hc[2]), 0};
        if (norm3(d) > 1e-4f) memcpy(fwd, d, sizeof d);
    }
    return hc[2] > 0;
}
/* A triangle's normal: its vertices' normals summed, or its own; 0 when degenerate. */
static int tri_normal(const HairMesh *m, const uint32_t *tv, float nr[3]) {
    float e1[3], e2[3];
    nr[0] = nr[1] = nr[2] = 0;
    if (m->nrm)
        for (int j = 0; j < 3; j++) nr[0] += m->nrm[tv[j]][0], nr[1] += m->nrm[tv[j]][1], nr[2] += m->nrm[tv[j]][2];
    else {
        sub3(m->pos[tv[1]], m->pos[tv[0]], e1);
        sub3(m->pos[tv[2]], m->pos[tv[0]], e2);
        nr[0] = e1[1] * e2[2] - e1[2] * e2[1], nr[1] = e1[2] * e2[0] - e1[0] * e2[2], nr[2] = e1[0] * e2[1] - e1[1] * e2[0];
    }
    return norm3(nr) >= 1e-12f;
}
static uint32_t median(const uint32_t *h, uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < 256; i++)
        if ((s += h[i]) * 2 >= n) return i;
    return 255;
}

/* ---- headgear modeled into a skin ---- */
static uint64_t mix64(uint64_t z) {
    z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ z >> 27) * 0x94d049bb133111ebull;
    return z ^ z >> 31;
}
static int u64_cmp(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
uint64_t hair_head_print(const HairMesh *m) {
    if (!m || !m->pos || !m->slot || !m->weight || !m->nv) return 0;
    uint64_t *h = malloc((size_t)m->nv * sizeof *h);
    if (!h) return 0;
    uint32_t n = 0;
    for (uint32_t v = 0; v < m->nv; v++) {
        if (dominant(m, v) != HAIR_SLOT_HEAD) continue;
        uint32_t q[3];
        for (int k = 0; k < 3; k++) q[k] = (uint32_t)(int32_t)floor((double)m->pos[v][k] * 10000.0 + 0.5);
        h[n++] = mix64(((uint64_t)q[0] | (uint64_t)q[1] << 32) ^ mix64(q[2]));
    }
    qsort(h, n, sizeof *h, u64_cmp);
    uint64_t p = 0, distinct = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!i || h[i] != h[i - 1]) p += h[i], distinct++;
    free(h);
    return distinct ? mix64(p ^ distinct) : 0;
}
/* The title's skins with modeled headgear (prints of the extracted characters,
 * unique among the 153): a black helmet, a white paper cap, a green helmet and a dark helmet. */
#define HEADGEAR_MAX 64
#define HAIR_HEADGEAR_BUILTIN 0x1c59be136e60919aull, 0xdeabaa12180e00d1ull, 0xe296b8d0558c04ddull, 0x84bb599ad9e56cdcull
static uint64_t headgear[HEADGEAR_MAX] = {HAIR_HEADGEAR_BUILTIN};
static int headgear_n = -1;
static pthread_mutex_t headgear_lock = PTHREAD_MUTEX_INITIALIZER;
static int headgear_count(void) {   /* under the lock */
    if (headgear_n < 0)
        for (headgear_n = 0; headgear_n < HEADGEAR_MAX && headgear[headgear_n]; headgear_n++) {}
    return headgear_n;
}
int hair_is_headgear(const HairMesh *m) {
    const uint64_t p = hair_head_print(m);
    int r = 0;
    pthread_mutex_lock(&headgear_lock);
    for (int i = 0, n = headgear_count(); i < n && p && !r; i++) r = headgear[i] == p;
    pthread_mutex_unlock(&headgear_lock);
    return r;
}
void hair_headgear_register(uint64_t print) {
    pthread_mutex_lock(&headgear_lock);
    const int n = headgear_count();
    if (print && n < HEADGEAR_MAX) headgear[headgear_n++] = print;
    pthread_mutex_unlock(&headgear_lock);
}
int hair_headgear_known(void) {
    pthread_mutex_lock(&headgear_lock);
    const int n = headgear_count();
    pthread_mutex_unlock(&headgear_lock);
    return n;
}

uint32_t hair_mask_add(uint32_t *mask, uint32_t mw, uint32_t mh, const HairMesh *m, const uint32_t *albedo, float ref_rgb[3], int *have_ref) {
    if (!mask || !m || !mw || !mh || m->ni < 3 || !m->pos || !m->uv || !m->slot || !m->weight || !m->idx) return 0;
    const size_t n = (size_t)mw * mh;
    float fwd[3], centre[2];
    head_frame(m, fwd, centre);
    const int gear = hair_is_headgear(m);
    /* classify the triangles; the hair's height range */
    const uint32_t nt = m->ni / 3;
    uint8_t *cls = calloc(nt ? nt : 1, 1);   /* 1 hair, 2 face */
    float *gw = calloc(n, sizeof *gw), *gh = calloc(n, sizeof *gh), *gf = calloc(2 * n, sizeof *gf);
    uint32_t (*hist)[256] = calloc(3, sizeof *hist);
    if (!cls || !gw || !gh || !gf || !hist) {
        free(cls), free(gw), free(gh), free(gf), free(hist);
        return 0;
    }
    float z0 = INFINITY, z1 = -INFINITY;
    for (uint32_t t = 0; t < nt; t++) {
        const uint32_t *tv = m->idx + 3 * t;
        if (tv[0] >= m->nv || tv[1] >= m->nv || tv[2] >= m->nv) continue;
        const int s = tri_slot(m, tv);
        if (is_face(s)) {
            cls[t] = 2;
            continue;
        }
        if (s != HAIR_SLOT_HEAD || gear) continue;   /* modeled headgear: no hair (the face still gives the skin colour) */
        float nr[3];
        if (!tri_normal(m, tv, nr)) continue;
        if (dot3(nr, fwd) > HAIR_FACE_FACING && nr[2] <= HAIR_TOP_FACING) continue;   /* face */
        cls[t] = 1;
        for (int j = 0; j < 3; j++) z0 = fminf(z0, m->pos[tv[j]][2]), z1 = fmaxf(z1, m->pos[tv[j]][2]);
    }
    const float zr = z1 > z0 ? z1 - z0 : 1;
    TexCtx c = {mw, gw, gh, gf, 0, 0, 0, {0, 1}, albedo, hist};
    uint32_t faces = 0;
    for (uint32_t t = 0; t < nt; t++) {
        if (!cls[t]) continue;
        const uint32_t *tv = m->idx + 3 * t;
        if (cls[t] == 2) {
            if (albedo) raster(m->uv[tv[0]], m->uv[tv[1]], m->uv[tv[2]], mw, mh, face_texel, &c), faces++;
            continue;
        }
        float e1[3], e2[3], fn[3];
        sub3(m->pos[tv[1]], m->pos[tv[0]], e1);
        sub3(m->pos[tv[2]], m->pos[tv[0]], e2);
        fn[0] = e1[1] * e2[2] - e1[2] * e2[1], fn[1] = e1[2] * e2[0] - e1[0] * e2[2], fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        if (norm3(fn) < 1e-12f) continue;
        tri_flow(m, tv, fn, fwd, mw, mh, c.dir);
        c.ha = (m->pos[tv[0]][2] - z0) / zr, c.hb = (m->pos[tv[1]][2] - z0) / zr, c.hc = (m->pos[tv[2]][2] - z0) / zr;
        raster(m->uv[tv[0]], m->uv[tv[1]], m->uv[tv[2]], mw, mh, hair_texel, &c);
    }
    free(cls);
    /* the face's skin colour */
    if (albedo && faces) {
        uint32_t tot = 0;
        for (int i = 0; i < 256; i++) tot += hist[0][i];
        if (tot) {
            for (int k = 0; k < 3; k++) ref_rgb[k] = median(hist[k], tot) / 255.0f;
            *have_ref = 1;
        }
    }
    free(hist);
    /* one ring of texels around the hair takes its neighbours' values (bilinear lookups and UV
     * seams), then the weight is softened over 3 x 3 */
    float *rw = malloc(n * sizeof *rw);
    if (!rw) {
        free(gw), free(gh), free(gf);
        return 0;
    }
    memcpy(rw, gw, n * sizeof *rw);
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            const size_t i = (size_t)y * mw + x;
            if (gw[i] > 0) continue;
            float s = 0, sh = 0, sf[2] = {0, 0};
            for (int k = 0; k < 4; k++) {
                const uint32_t xx = (x + mw + (k == 0) - (k == 1)) % mw, yy = (y + mh + (k == 2) - (k == 3)) % mh;
                const size_t j = (size_t)yy * mw + xx;
                if (gw[j] > 0) s += 1, sh += gh[j], sf[0] += gf[2 * j], sf[1] += gf[2 * j + 1];
            }
            if (s > 0) {
                rw[i] = 1;
                gh[i] = sh / s;
                const float l = sqrtf(sf[0] * sf[0] + sf[1] * sf[1]);
                gf[2 * i] = l > 1e-6f ? sf[0] / l : 0, gf[2 * i + 1] = l > 1e-6f ? sf[1] / l : 1;
            }
        }
    uint32_t wrote = 0;
    const float *ref = albedo && *have_ref ? ref_rgb : NULL;
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            const size_t i = (size_t)y * mw + x;
            if (rw[i] <= 0) continue;
            float s = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) s += rw[(size_t)((y + mh + dy) % mh) * mw + (x + mw + dx) % mw];
            float w = s / 9;
            if (albedo) {   /* the skin's colour over 3 x 3 texels: painted noise does not speckle the mask */
                uint32_t sum[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const uint32_t t = albedo[(size_t)((y + mh + dy) % mh) * mw + (x + mw + dx) % mw];
                        sum[0] += t >> 16 & 255, sum[1] += t >> 8 & 255, sum[2] += t & 255;
                    }
                w *= colour_gate((sum[0] + 4) / 9 << 16 | (sum[1] + 4) / 9 << 8 | (sum[2] + 4) / 9, ref);
            }
            const uint32_t r = (uint32_t)(w * 255 + 0.5f);
            if (r <= (mask[i] >> 16 & 255)) continue;
            const float h = gh[i] < 0 ? 0 : gh[i] > 1 ? 1 : gh[i];
            const uint32_t g = (uint32_t)(h * 255 + 0.5f), b = (uint32_t)(128 + 127 * gf[2 * i] + 0.5f), a = (uint32_t)(128 + 127 * gf[2 * i + 1] + 0.5f);
            mask[i] = a << 24 | r << 16 | g << 8 | b;
            wrote++;
        }
    free(rw), free(gw), free(gh), free(gf);
    return wrote;
}

/* ---- --skin ---- */

typedef struct {
    uint32_t mw;
    uint8_t *region, *thin;
    uint8_t r, t;
} SkinCtx;
static void skin_texel(void *ctx, uint32_t x, uint32_t y, float wa, float wb, float wc) {
    SkinCtx *c = ctx;
    (void)wa, (void)wb, (void)wc;
    const size_t i = (size_t)y * c->mw + x;
    if (c->r > c->region[i]) c->region[i] = c->r;
    if (c->t > c->thin[i]) c->thin[i] = c->t;
}

void skin_geom_add(uint8_t *region, uint8_t *thin, uint32_t mw, uint32_t mh, const HairMesh *m) {
    if (!region || !thin || !m || !mw || !mh || m->ni < 3 || !m->pos || !m->uv || !m->slot || !m->weight || !m->idx) return;
    float fwd[3], centre[2];
    const int head = head_frame(m, fwd, centre);
    /* across the head (horizontal), and how far its vertices reach out to the sides */
    const float side[2] = {fwd[1], -fwd[0]};
    float reach = 0;
    if (head)
        for (uint32_t v = 0; v < m->nv; v++) {
            const int s = dominant(m, v);
            if (s == HAIR_SLOT_HEAD || is_face(s)) reach = fmaxf(reach, fabsf((m->pos[v][0] - centre[0]) * side[0] + (m->pos[v][1] - centre[1]) * side[1]));
        }
    SkinCtx c = {mw, region, thin, 0, 0};
    for (uint32_t t = 0; t < m->ni / 3; t++) {
        const uint32_t *tv = m->idx + 3 * t;
        if (tv[0] >= m->nv || tv[1] >= m->nv || tv[2] >= m->nv) continue;
        const int s = tri_slot(m, tv);
        if (s < 0) continue;
        float nr[3];
        if (s == SKIN_SLOT_PELVIS || s >= SKIN_SLOT_LEGS_FIRST) continue;   /* trousers */
        if (is_face(s)) c.r = SKIN_REGION_FACE;
        else if (SKIN_IS_HAND(s)) c.r = SKIN_REGION_HAND;
        else if (s == HAIR_SLOT_HEAD)
            c.r = tri_normal(m, tv, nr) && dot3(nr, fwd) > HAIR_FACE_FACING && nr[2] <= HAIR_TOP_FACING ? SKIN_REGION_FACE : SKIN_REGION_HEAD;
        else c.r = s == HAIR_SLOT_NECK ? SKIN_REGION_NECK : SKIN_REGION_BODY;
        c.t = 0;
        if ((c.r == SKIN_REGION_HEAD || c.r == SKIN_REGION_FACE) && reach > 1e-6f) {   /* the ears: the head's most lateral triangles */
            float lat = 0;
            for (int j = 0; j < 3; j++) lat += fabsf((m->pos[tv[j]][0] - centre[0]) * side[0] + (m->pos[tv[j]][1] - centre[1]) * side[1]);
            c.t = (uint8_t)(smooth(0.7f, 0.92f, lat / 3 / reach) * 255 + 0.5f);
        }
        raster(m->uv[tv[0]], m->uv[tv[1]], m->uv[tv[2]], mw, mh, skin_texel, &c);
    }
}

/* How much a colour looks like the reference skin colour: its hue (chromaticity within d0 .. d1)
 * and about its brightness (from ~0.5 to ~1.8 times the reference's). */
static float skin_like(const float *p, const float *ref, float d0, float d1) {
    const float sp = p[0] + p[1] + p[2], sr = ref[0] + ref[1] + ref[2], lr = luma(ref);
    if (sp < 0.03f || sr < 0.03f || lr < 0.02f) return 0;
    float d = 0;
    for (int k = 0; k < 3; k++) d += (p[k] / sp - ref[k] / sr) * (p[k] / sp - ref[k] / sr);
    const float b = luma(p) / lr;
    return (1 - smooth(d0, d1, sqrtf(d))) * smooth(0.4f, 0.6f, b) * (1 - smooth(1.6f, 2.2f, b));
}

uint32_t skin_mask_build(uint32_t *skin, uint32_t mw, uint32_t mh, const uint8_t *region, const uint8_t *thin, const uint32_t *albedo,
                         const float ref_rgb[3], const uint32_t *hair) {
    if (!skin || !mw || !mh) return 0;
    const size_t n = (size_t)mw * mh;
    memset(skin, 0, n * sizeof *skin);
    if (!region || !thin || !albedo || !ref_rgb) return 0;
    static const float sheen[6] = {0, 0.4f, 0.5f, 0.7f, 1.0f, 0.4f}, base[6] = {0, 0.3f, 0.4f, 0.45f, 0.45f, 0.4f};
    /* below the head: the hands' median colour (always bare), else the face's */
    uint32_t (*hist)[256] = calloc(3, sizeof *hist), nh = 0;
    float body[3] = {ref_rgb[0], ref_rgb[1], ref_rgb[2]};
    if (hist) {
        for (size_t i = 0; i < n; i++)
            if (region[i] == SKIN_REGION_HAND) {
                const uint32_t t = albedo[i];
                hist[0][t >> 16 & 255]++, hist[1][t >> 8 & 255]++, hist[2][t & 255]++, nh++;
            }
        if (nh >= 16) {
            float hand[3];
            for (int k = 0; k < 3; k++) hand[k] = median(hist[k], nh) / 255.0f;
            /* a glove or a dark sleeve over the hands: keep the face's */
            if (skin_like(hand, ref_rgb, 0.04f, 0.08f) > 0.5f) memcpy(body, hand, sizeof body);
        }
        free(hist);
    }
    uint32_t count = 0;
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            const size_t i = (size_t)y * mw + x;
            const int r = region[i] <= SKIN_REGION_HAND ? region[i] : SKIN_REGION_HAND;
            if (!r) continue;
            uint32_t sum[3] = {0, 0, 0};   /* the colour over 3 x 3 texels, as the hair's */
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const uint32_t t = albedo[(size_t)((y + mh + dy) % mh) * mw + (x + mw + dx) % mw];
                    sum[0] += t >> 16 & 255, sum[1] += t >> 8 & 255, sum[2] += t & 255;
                }
            const float p[3] = {sum[0] / (9 * 255.0f), sum[1] / (9 * 255.0f), sum[2] / (9 * 255.0f)};
            const int head = r == SKIN_REGION_HEAD || r == SKIN_REGION_FACE;
            float w = head ? skin_like(p, ref_rgb, 0.03f, 0.06f) : skin_like(p, body, 0.02f, 0.04f);
            if (hair) w *= 1 - (hair[i] >> 16 & 255) / 255.0f;
            const uint32_t wr = (uint32_t)(w * 255 + 0.5f);
            if (!wr) continue;
            const float th = base[r] + (1 - base[r]) * thin[i] / 255.0f;
            skin[i] = 0xFF000000u | wr << 16 | (uint32_t)(th * 255 + 0.5f) << 8 | (uint32_t)(sheen[r] * 255 + 0.5f);
            count++;
        }
    /* one ring of texels outside the mesh's triangles takes its skin neighbours' values (bilinear
     * lookups at UV seams) */
    uint32_t *ring = malloc(n * sizeof *ring);
    if (!ring) return count;
    memcpy(ring, skin, n * sizeof *ring);
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            const size_t i = (size_t)y * mw + x;
            if (region[i]) continue;
            uint32_t s[4] = {0, 0, 0, 0}, k = 0;
            for (int d = 0; d < 4; d++) {
                const uint32_t xx = (x + mw + (d == 0) - (d == 1)) % mw, yy = (y + mh + (d == 2) - (d == 3)) % mh;
                const uint32_t t = skin[(size_t)yy * mw + xx];
                if (!(t >> 16 & 255)) continue;
                for (int c = 0; c < 4; c++) s[c] += t >> (8 * c) & 255;
                k++;
            }
            if (k) ring[i] = (s[3] + k / 2) / k << 24 | (s[2] + k / 2) / k << 16 | (s[1] + k / 2) / k << 8 | (s[0] + k / 2) / k;
        }
    memcpy(skin, ring, n * sizeof *skin);
    free(ring);
    return count;
}


/* ---- --eyes, --cloth ---- */

typedef struct {
    uint32_t mw;
    uint8_t *region, *eye;
    uint8_t r;
    uint8_t q;          /* the triangle's eye weight */
} MatCtx;
static void mat_texel(void *ctx, uint32_t x, uint32_t y, float wa, float wb, float wc) {
    (void)wa, (void)wb, (void)wc;
    MatCtx *c = ctx;
    const size_t i = (size_t)y * c->mw + x;
    if (c->r > c->region[i]) c->region[i] = c->r;
    if (c->q > c->eye[i]) c->eye[i] = c->q;
}
static int cloth_region(int s) {
    if (s == SKIN_SLOT_PELVIS || s == 26 || s == 27 || s == 29 || s == 30) return CLOTH_REGION_LEGS;   /* thighs, calves */
    if (s == 15 || s == 16 || s == 21 || s == 22) return CLOTH_REGION_ARM;                            /* upper arms, forearms */
    if (s == 1 || s == 2 || s == HAIR_SLOT_NECK || s == 14 || s == 20) return CLOTH_REGION_TORSO;     /* spine, neck, clavicles */
    return CLOTH_REGION_OTHER;                                                                         /* head, face, hands, feet */
}
static float eye_lid_weight(const HairMesh *m, uint32_t v) {
    float w = 0;
    for (int k = 0; k < 3; k++)
        if (m->slot[v][k] == HAIR_SLOT_EYE_LID) w += m->weight[v][k];
    return w;
}
static float tri_area2(const float *a, const float *b, const float *c) {
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
    return x * x + y * y + z * z;
}
/* A triangle around the eye lid (w: its vertices' eye-lid weights) is the eye's opening when a
 * blink, pulling its lid-weighted vertices down (model z), shrinks it: the opening closes under the
 * lid. One that stretches is the upper lid itself (the strip above the lid's line, drawn over the
 * whole eye while it blinks): never eye. The opening's weight is its strongest vertex's, all over
 * it (the eye down to the lower lid, whose vertices the lid leaves alone). */
static float eye_tri_weight(const float *pa, const float *pb, const float *pc, const float w[3]) {
    const float wm = fmaxf(w[0], fmaxf(w[1], w[2]));
    if (!(wm > 0)) return 0;
    const float *p[3] = {pa, pb, pc};
    float e = 0;
    for (int k = 0; k < 3; k++) {
        const float *a = p[k], *b = p[(k + 1) % 3];
        const float d2 = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
        e = fmaxf(e, d2);
    }
    const float d = 0.25f * sqrtf(e);
    float q[3][3];
    for (int k = 0; k < 3; k++) q[k][0] = p[k][0], q[k][1] = p[k][1], q[k][2] = p[k][2] - w[k] * d;
    const float before = tri_area2(pa, pb, pc), after = tri_area2(q[0], q[1], q[2]);
    return after <= before * 1.0001f ? (wm > 1 ? 1 : wm) : 0;
}

void mat_geom_add(uint8_t *region, uint8_t *eye, uint32_t mw, uint32_t mh, const HairMesh *m) {
    if (!region || !eye || !m || !mw || !mh || m->ni < 3 || !m->pos || !m->uv || !m->slot || !m->weight || !m->idx) return;
    MatCtx c = {mw, region, eye, 0, 0};
    for (uint32_t t = 0; t < m->ni / 3; t++) {
        const uint32_t *tv = m->idx + 3 * t;
        if (tv[0] >= m->nv || tv[1] >= m->nv || tv[2] >= m->nv) continue;
        const int s = tri_slot(m, tv);
        if (s < 0) continue;
        c.r = (uint8_t)cloth_region(s);
        const float w[3] = {eye_lid_weight(m, tv[0]), eye_lid_weight(m, tv[1]), eye_lid_weight(m, tv[2])};
        c.q = (uint8_t)(eye_tri_weight(m->pos[tv[0]], m->pos[tv[1]], m->pos[tv[2]], w) * 255 + 0.5f);
        raster(m->uv[tv[0]], m->uv[tv[1]], m->uv[tv[2]], mw, mh, mat_texel, &c);
    }
}

uint32_t eyes_mask_build(uint32_t *eyes, uint32_t mw, uint32_t mh, const uint8_t *eye, const uint32_t *albedo, const float ref_rgb[3]) {
    if (!eyes || !mw || !mh) return 0;
    const size_t n = (size_t)mw * mh;
    memset(eyes, 0, n * sizeof *eyes);
    if (!eye || !albedo || !ref_rgb) return 0;
    float *w = calloc(n, sizeof *w);
    int32_t *lab = malloc(n * sizeof *lab), *stack = malloc(n * sizeof *stack);
    if (!w || !lab || !stack) {
        free(w), free(lab), free(stack);
        return 0;
    }
    /* the eye-lid's texels that are not the face's skin: the white, the iris (the lids are skin;
     * one texel's own colour: an eye is a few texels wide) */
    for (size_t i = 0; i < n; i++) {
        lab[i] = -1;
        if (!eye[i]) continue;
        float p[3];
        unpack(albedo[i], p);
        const float g = (eye[i] / 255.0f) * (1 - skin_like(p, ref_rgb, 0.03f, 0.06f));
        if (g >= 0.05f) w[i] = fminf(1.6f * g, 1);   /* a texel half eye, half lid: mostly eye */
    }
    /* each eye (8-connected texels): its centre and half extents (from the weighted variance:
     * sqrt(3 var) is half a uniform run's length), then every texel's offset, one ring further */
    uint32_t count = 0;
    int32_t comps = 0;
    for (size_t i0 = 0; i0 < n; i0++) {
        if (w[i0] <= 0 || lab[i0] >= 0) continue;
        size_t sp = 0, first = 0;
        int32_t *members = stack;   /* reused as the component's list */
        members[sp++] = (int32_t)i0;
        lab[i0] = comps;
        double sw = 0, sx = 0, sy = 0, sxx = 0, syy = 0;
        while (first < sp) {
            const int32_t i = members[first++];
            const int x = i % (int)mw, y = i / (int)mw;
            const double wi = w[i], px = x + 0.5, py = y + 0.5;
            sw += wi, sx += wi * px, sy += wi * py, sxx += wi * px * px, syy += wi * py * py;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= (int)mw || yy >= (int)mh) continue;
                    const size_t j = (size_t)yy * mw + xx;
                    if (w[j] > 0 && lab[j] < 0) lab[j] = comps, members[sp++] = (int32_t)j;
                }
        }
        const double cx = sx / sw, cy = sy / sw;
        double rx = sqrt(fmax(3 * (sxx / sw - cx * cx), 0)), ry = sqrt(fmax(3 * (syy / sw - cy * cy), 0));
        rx = fmax(rx, 0.75), ry = fmax(ry, 0.75);
        for (size_t k = 0; k < sp; k++) {
            const int32_t i = members[k];
            const int x = i % (int)mw, y = i / (int)mw;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= (int)mw || yy >= (int)mh) continue;
                    const size_t j = (size_t)yy * mw + xx;
                    if (lab[j] != comps && (w[j] > 0 || eyes[j])) continue;   /* another eye's, or done */
                    double du = (xx + 0.5 - cx) / rx, dv = (yy + 0.5 - cy) / ry;
                    du = du < -1 ? -1 : du > 1 ? 1 : du, dv = dv < -1 ? -1 : dv > 1 ? 1 : dv;
                    const uint32_t r = w[j] > 0 ? (uint32_t)(w[j] * 255 + 0.5f) : 0;
                    eyes[j] = 0xFF000000u | r << 16 | (uint32_t)(128 + 127 * du + 0.5) << 8 | (uint32_t)(128 + 127 * dv + 0.5);
                }
            count++;
        }
        comps++;
    }
    free(w), free(lab), free(stack);
    return count;
}

float eyes_lid_open(const float (*c)[4], int base, int rows) {
    if (!c || base < 0 || base + 3 * HAIR_SLOTS > rows) return 1;
    /* the angle between the two rotations, whatever their (uniform) scale and the palette's
     * transposition: sum(H o L) = sh sl trace(Rh^T Rl), |H| = sh sqrt 3 */
    const float (*h)[4] = c + base + 3 * HAIR_SLOT_HEAD, (*l)[4] = c + base + 3 * HAIR_SLOT_EYE_LID;
    double hl = 0, hh = 0, ll = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) hl += (double)h[i][j] * l[i][j], hh += (double)h[i][j] * h[i][j], ll += (double)l[i][j] * l[i][j];
    if (!isfinite(hl) || !isfinite(hh) || !isfinite(ll) || hh < 1e-12 || ll < 1e-12) return 1;
    double ct = (3 * hl / sqrt(hh * ll) - 1) / 2;
    ct = ct < -1 ? -1 : ct > 1 ? 1 : ct;
    const float deg = (float)(acos(ct) * 57.29577951308232);
    if (deg <= EYES_LID_OPEN_DEG) return 1;
    if (deg >= EYES_LID_SHUT_DEG) return 0;
    const float t = (deg - EYES_LID_OPEN_DEG) / (EYES_LID_SHUT_DEG - EYES_LID_OPEN_DEG);
    return 1 - t * t * (3 - 2 * t);
}

static float hue_of(const float *p, float *sat, float *val) {
    const float mx = fmaxf(p[0], fmaxf(p[1], p[2])), mn = fminf(p[0], fminf(p[1], p[2])), d = mx - mn;
    *val = mx;
    *sat = mx > 1e-4f ? d / mx : 0;
    if (d < 1e-5f) return 0;
    if (mx == p[0]) return 60 * fmodf((p[1] - p[2]) / d + 6, 6);
    if (mx == p[1]) return 60 * ((p[2] - p[0]) / d + 2);
    return 60 * ((p[0] - p[1]) / d + 4);
}

uint32_t cloth_mask_build(uint32_t *cloth, uint32_t mw, uint32_t mh, const uint8_t *region, const uint32_t *albedo, const uint32_t *skin,
                          const uint32_t *hair) {
    if (!cloth || !mw || !mh) return 0;
    const size_t n = (size_t)mw * mh;
    memset(cloth, 0, n * sizeof *cloth);
    if (!region || !albedo) return 0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < mh; y++)
        for (uint32_t x = 0; x < mw; x++) {
            const size_t i = (size_t)y * mw + x;
            const int r = region[i];
            if (r == CLOTH_REGION_NONE || r >= CLOTH_REGION_OTHER) continue;
            float cw = 1;
            if (skin) cw *= 1 - (skin[i] >> 16 & 255) / 255.0f;
            if (hair) cw *= 1 - (hair[i] >> 16 & 255) / 255.0f;
            if (cw < 0.05f) continue;
            uint32_t sum[3] = {0, 0, 0};   /* the colour over 3 x 3 texels, as the skin's */
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const uint32_t t = albedo[(size_t)((y + mh + dy) % mh) * mw + (x + mw + dx) % mw];
                    sum[0] += t >> 16 & 255, sum[1] += t >> 8 & 255, sum[2] += t & 255;
                }
            const float p[3] = {sum[0] / (9 * 255.0f), sum[1] / (9 * 255.0f), sum[2] / (9 * 255.0f)};
            float sat, val;
            const float hue = hue_of(p, &sat, &val);
            const float lit = smooth(0.03f, 0.07f, val);
            /* denim: blue to blue-grey (worn jeans are barely saturated) */
            const float cyan = r == CLOTH_REGION_LEGS ? smooth(125, 150, hue) : smooth(150, 170, hue);   /* worn jeans drift to grey-green */
            float denim = cyan * (1 - smooth(245, 265, hue)) * smooth(0.025f, 0.07f, sat) * lit;
            /* leather: dark red to brown, saturated, on the torso or the arms (vests, jackets) */
            float leather = 0;
            if (r == CLOTH_REGION_TORSO || r == CLOTH_REGION_ARM) {
                const float red = hue < 180 ? 1 - smooth(25, 45, hue) : smooth(320, 340, hue);
                leather = red * smooth(0.18f, 0.30f, sat) * (1 - smooth(0.60f, 0.80f, val)) * lit;
            }
            if (leather > 1 - denim) leather = 1 - denim;
            const float cotton = 1 - leather - denim;
            const uint32_t lr = (uint32_t)(leather * cw * 255 + 0.5f), dg = (uint32_t)(denim * cw * 255 + 0.5f), cb = (uint32_t)(cotton * cw * 255 + 0.5f);
            if (!(lr | dg | cb)) continue;
            cloth[i] = 0xFF000000u | lr << 16 | dg << 8 | cb;
            count++;
        }
    return count;
}

/* ---- the cache ---- */

typedef struct {
    uint64_t key;
    int used;
    uint32_t nsig;
    uint64_t sig[HAIR_CACHE_MESHES];
    uint32_t mw, mh, version, hair;
    float ref[3];
    int have_ref;
    uint32_t *mask;
    /* --skin (hair_cache_set_skin): the maps, the skin mask, its texels and version */
    uint8_t *region, *thin;
    uint32_t *skin, skin_n, skin_version;
    /* --eyes, --cloth (hair_cache_set_mat): their maps, masks, texels, and the three masks stacked */
    uint8_t *creg, *eye;
    uint32_t *eyes, *cloth, *stack, eyes_n, cloth_n, mat_version;
} HairSkin;
static HairSkin cache[HAIR_CACHE_SKINS];
static uint32_t cache_n;
static int skin_on, eyes_on, cloth_on;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static HairSkin *find(uint64_t key, int add) {
    const uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) % HAIR_CACHE_SKINS;
    for (uint32_t k = 0; k < HAIR_CACHE_SKINS; k++) {
        HairSkin *e = &cache[(h + k) % HAIR_CACHE_SKINS];
        if (e->used && e->key == key) return e;
        if (!e->used) {
            if (!add || cache_n + 1 >= HAIR_CACHE_SKINS) return NULL;   /* one slot stays free: lookups end */
            memset(e, 0, sizeof *e);
            e->used = 1;
            e->key = key;
            cache_n++;
            return e;
        }
    }
    return NULL;
}
static int has_sig(const HairSkin *e, uint64_t sig) {
    for (uint32_t i = 0; i < e->nsig; i++)
        if (e->sig[i] == sig) return 1;
    return 0;
}

int hair_cache_wants(uint64_t key, uint64_t sig) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    const int want = e ? e->nsig < HAIR_CACHE_MESHES && !has_sig(e, sig) : cache_n + 1 < HAIR_CACHE_SKINS;
    pthread_mutex_unlock(&cache_lock);
    return want;
}

uint32_t hair_cache_add(uint64_t key, uint64_t sig, const HairMesh *m, const uint32_t *albedo, uint32_t w, uint32_t h) {
    uint32_t mw, mh;
    hair_mask_size(w, h, &mw, &mh);
    uint32_t *small = NULL;
    if (albedo && (mw != w || mh != h)) {
        small = malloc((size_t)mw * mh * sizeof *small);
        if (small) hair_downsample(albedo, w, h, small, mw, mh);
        albedo = small;
    }
    pthread_mutex_lock(&cache_lock);
    HairSkin *e = find(key, 1);
    uint32_t hair = 0;
    if (e && e->nsig < HAIR_CACHE_MESHES && !has_sig(e, sig)) {
        e->sig[e->nsig++] = sig;
        if (!e->mask || e->mw != mw || e->mh != mh) {
            free(e->mask);
            e->mask = calloc((size_t)mw * mh, sizeof *e->mask);
            e->mw = e->mask ? mw : 0;
            e->mh = e->mask ? mh : 0;
            e->hair = 0;
        }
        if (e->mask && hair_mask_add(e->mask, mw, mh, m, albedo, e->ref, &e->have_ref)) {
            e->version++;
            e->hair = 0;
            for (size_t i = 0; i < (size_t)mw * mh; i++) e->hair += (e->mask[i] >> 16 & 255) != 0;
        }
        const int mat = eyes_on || cloth_on;
        if ((skin_on || mat) && e->mask) {   /* the mask's size is the skin's: the maps follow it (--eyes, --cloth: skin is not cloth) */
            if (!e->region) {
                e->region = calloc((size_t)mw * mh, 1);
                e->thin = calloc((size_t)mw * mh, 1);
                e->skin = calloc((size_t)mw * mh, sizeof *e->skin);
            }
            if (e->region && e->thin && e->skin) {
                skin_geom_add(e->region, e->thin, mw, mh, m);
                if (albedo && e->have_ref) {
                    e->skin_n = skin_mask_build(e->skin, mw, mh, e->region, e->thin, albedo, e->ref, e->mask);
                    if (e->skin_n) e->skin_version++;
                }
            }
        }
        if (mat && e->mask) {
            const size_t n = (size_t)mw * mh;
            if (!e->creg) {
                e->creg = calloc(n, 1);
                e->eye = calloc(n, 1);
                e->eyes = calloc(n, sizeof *e->eyes);
                e->cloth = calloc(n, sizeof *e->cloth);
                e->stack = calloc(3 * n, sizeof *e->stack);
            }
            if (e->creg && e->eye && e->eyes && e->cloth && e->stack) {
                mat_geom_add(e->creg, e->eye, mw, mh, m);
                e->eyes_n = eyes_on && albedo ? eyes_mask_build(e->eyes, mw, mh, e->eye, albedo, e->have_ref ? e->ref : NULL) : 0;
                e->cloth_n = cloth_on && albedo ? cloth_mask_build(e->cloth, mw, mh, e->creg, albedo, e->skin_n ? e->skin : NULL, e->mask) : 0;
                if (!e->eyes_n) memset(e->eyes, 0, n * sizeof *e->eyes);
                if (!e->cloth_n) memset(e->cloth, 0, n * sizeof *e->cloth);
                if (skin_on && e->skin && e->skin_n) memcpy(e->stack, e->skin, n * sizeof *e->stack);
                else memset(e->stack, 0, n * sizeof *e->stack);
                memcpy(e->stack + n, e->eyes, n * sizeof *e->stack);
                memcpy(e->stack + 2 * n, e->cloth, n * sizeof *e->stack);
                e->mat_version++;
            }
        }
    }
    if (e) hair = e->hair;
    pthread_mutex_unlock(&cache_lock);
    free(small);
    return hair;
}

uint32_t hair_cache_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    const uint32_t r = e && e->mask && e->hair ? fn(ctx, e->version, e->mw, e->mh, e->mask) : 0;
    pthread_mutex_unlock(&cache_lock);
    return r;
}

uint32_t hair_cache_skin_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    const uint32_t r = e && e->skin && e->skin_n ? fn(ctx, e->skin_version, e->mw, e->mh, e->skin) : 0;
    pthread_mutex_unlock(&cache_lock);
    return r;
}

uint32_t hair_cache_mat_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    const uint32_t r = e && e->stack && (e->eyes_n || e->cloth_n || (skin_on && e->skin_n)) ? fn(ctx, e->mat_version, e->mw, 3 * e->mh, e->stack) : 0;
    pthread_mutex_unlock(&cache_lock);
    return r;
}

void hair_cache_mat_texels(uint64_t key, uint32_t *eyes, uint32_t *cloth) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    *eyes = e ? e->eyes_n : 0;
    *cloth = e ? e->cloth_n : 0;
    pthread_mutex_unlock(&cache_lock);
}

void hair_cache_set_mat(int eyes, int cloth) {
    pthread_mutex_lock(&cache_lock);
    eyes_on = eyes != 0;
    cloth_on = cloth != 0;
    pthread_mutex_unlock(&cache_lock);
}

uint32_t hair_cache_skin_texels(uint64_t key) {
    pthread_mutex_lock(&cache_lock);
    const HairSkin *e = find(key, 0);
    const uint32_t r = e && e->skin ? e->skin_n : 0;
    pthread_mutex_unlock(&cache_lock);
    return r;
}

void hair_cache_set_skin(int on) {
    pthread_mutex_lock(&cache_lock);
    skin_on = on != 0;
    pthread_mutex_unlock(&cache_lock);
}

void hair_cache_stats(uint32_t *skins, uint32_t *with_hair) {
    pthread_mutex_lock(&cache_lock);
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; i < HAIR_CACHE_SKINS; i++)
        if (cache[i].used) a++, b += cache[i].hair != 0;
    pthread_mutex_unlock(&cache_lock);
    *skins = a;
    *with_hair = b;
}

void hair_cache_clear(void) {
    pthread_mutex_lock(&cache_lock);
    for (uint32_t i = 0; i < HAIR_CACHE_SKINS; i++) {
            free(cache[i].mask), free(cache[i].region), free(cache[i].thin), free(cache[i].skin);
            free(cache[i].creg), free(cache[i].eye), free(cache[i].eyes), free(cache[i].cloth), free(cache[i].stack);
        }
    memset(cache, 0, sizeof cache);
    cache_n = 0;
    pthread_mutex_unlock(&cache_lock);
}
