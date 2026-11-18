/* --hat-fix, --hat-subdiv: options, hat detection, smoothed normals and PN subdivision (hats.h). */
#include "hats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int hat_arg(HatConfig *c, const char *opt) {
    if (!strcmp(opt, "--hat-fix") || !strcmp(opt, "--no-hat-fix")) {
        c->fix = opt[2] == 'h';
        return 1;
    }
    if (!strcmp(opt, "--hat-subdiv") || !strcmp(opt, "--no-hat-subdiv")) {
        c->subdiv = opt[2] == 'h';
        return 1;
    }
    return 0;
}
int hat_fix_on(const HatConfig *c) { return c->fix != 0; }
int hat_subdiv_on(const HatConfig *c) { return c->subdiv < 0 ? hat_fix_on(c) : c->subdiv; }

int hat_is_small(const float box_min[3], const float box_max[3]) {
    float d = 0;
    for (int k = 0; k < 3; k++) {
        const float e = box_max[k] - box_min[k];
        if (!(e >= 0)) return 0;
        d += e * e;
    }
    return d < HAT_MAX_DIAG * HAT_MAX_DIAG;
}
int hat_near_head(const float p[3], const float (*heads)[3], int n, float reach) {
    for (int i = 0; i < n; i++) {
        const float x = p[0] - heads[i][0], y = p[1] - heads[i][1], z = p[2] - heads[i][2];
        if (x * x + y * y + z * z < reach * reach) return 1;
    }
    return 0;
}

static void sub3(const float *a, const float *b, float *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static float norm3(float *v) {
    const float l = sqrtf(dot3(v, v));
    if (l > 1e-20f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}
static float *at(const HatMesh *m, uint32_t v, uint32_t off) { return (float *)(m->v + (size_t)v * m->stride + off); }

/* Position ids: vertices with bit-identical positions share one (0..*np-1). */
static const HatMesh *sort_mesh;
static int pos_cmp(const void *a, const void *b) {
    const uint32_t i = *(const uint32_t *)a, j = *(const uint32_t *)b;
    const int c = memcmp(at(sort_mesh, i, sort_mesh->pos_off), at(sort_mesh, j, sort_mesh->pos_off), 12);
    return c ? c : (i > j) - (i < j);
}
static uint32_t *position_ids(const HatMesh *m, uint32_t *np) {
    uint32_t *ord = malloc(m->nv * sizeof *ord), *pid = malloc(m->nv * sizeof *pid);
    if (!ord || !pid) {
        free(ord), free(pid);
        return NULL;
    }
    for (uint32_t i = 0; i < m->nv; i++) ord[i] = i;
    sort_mesh = m;
    qsort(ord, m->nv, sizeof *ord, pos_cmp);
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->nv; i++) {
        if (i && memcmp(at(m, ord[i], m->pos_off), at(m, ord[i - 1], m->pos_off), 12)) n++;
        pid[ord[i]] = n;
    }
    *np = m->nv ? n + 1 : 0;
    free(ord);
    return pid;
}
static int mesh_ok(const HatMesh *m) {
    if (!m || !m->v || !m->idx || m->nv < 3 || m->ni < 3 || m->ni % 3 || m->stride < 12) return 0;
    if (m->pos_off + 12 > m->stride || m->nrm_off + 12 > m->stride) return 0;
    for (uint32_t i = 0; i < m->ni; i++)
        if (m->idx[i] >= m->nv) return 0;
    return 1;
}

int hat_smooth_normals(HatMesh *m, float max_deg) {
    if (!mesh_ok(m)) return -1;
    const uint32_t nt = m->ni / 3;
    uint32_t np = 0, *pid = position_ids(m, &np);
    float (*g)[3] = malloc(nt * sizeof *g), (*u)[3] = malloc(nt * sizeof *u), (*out)[3] = malloc(m->nv * sizeof *out);
    uint32_t *first = calloc(np + 1, sizeof *first), *list = malloc(3 * nt * sizeof *list), *ftri = malloc(m->nv * sizeof *ftri);
    int ret = -1;
    if (!pid || !g || !u || !out || !first || !list || !ftri) goto done;
    /* each triangle's normal g (turned to its corners' normals; 0 when degenerate) and unit normal u */
    for (uint32_t v = 0; v < m->nv; v++) ftri[v] = UINT32_MAX;
    for (uint32_t t = 0; t < nt; t++) {
        const uint16_t *c = m->idx + 3 * t;
        float e1[3], e2[3], s[3] = {0, 0, 0};
        sub3(at(m, c[1], m->pos_off), at(m, c[0], m->pos_off), e1);
        sub3(at(m, c[2], m->pos_off), at(m, c[0], m->pos_off), e2);
        cross3(e1, e2, g[t]);
        for (int j = 0; j < 3; j++) {
            const float *n = at(m, c[j], m->nrm_off);
            s[0] += n[0], s[1] += n[1], s[2] += n[2];
            if (ftri[c[j]] == UINT32_MAX) ftri[c[j]] = t;
        }
        if (dot3(g[t], s) < 0) g[t][0] = -g[t][0], g[t][1] = -g[t][1], g[t][2] = -g[t][2];
        memcpy(u[t], g[t], sizeof u[t]);
        if (norm3(u[t]) <= 1e-12f) g[t][0] = g[t][1] = g[t][2] = 0;   /* degenerate: no share */
    }
    /* the triangles at each position (CSR; a triangle with two corners at one position is degenerate) */
    for (uint32_t i = 0; i < 3 * nt; i++) first[pid[m->idx[i]] + 1]++;
    for (uint32_t p = 0; p < np; p++) first[p + 1] += first[p];
    {
        uint32_t *fill = malloc(np * sizeof *fill);
        if (!fill) goto done;
        memcpy(fill, first, np * sizeof *fill);
        for (uint32_t i = 0; i < 3 * nt; i++) list[fill[pid[m->idx[i]]]++] = i;
        free(fill);
    }
    const float cmin = cosf(max_deg * 3.14159265f / 180);
    for (uint32_t v = 0; v < m->nv; v++) {
        float r[3];
        memcpy(r, at(m, v, m->nrm_off), sizeof r);
        if (!(norm3(r) > 1e-6f) && ftri[v] != UINT32_MAX) memcpy(r, u[ftri[v]], sizeof r);
        float s[3] = {0, 0, 0};
        for (uint32_t k = first[pid[v]]; k < first[pid[v] + 1]; k++) {
            const uint32_t t = list[k] / 3, j = list[k] % 3;
            if (!(dot3(u[t], r) > cmin) || !(g[t][0] || g[t][1] || g[t][2])) continue;
            /* weighted by the triangle's angle at this corner: the same for any split of a facet */
            const uint16_t *c = m->idx + 3 * t;
            float e1[3], e2[3];
            sub3(at(m, c[(j + 1) % 3], m->pos_off), at(m, c[j], m->pos_off), e1);
            sub3(at(m, c[(j + 2) % 3], m->pos_off), at(m, c[j], m->pos_off), e2);
            if (!(norm3(e1) > 1e-12f) || !(norm3(e2) > 1e-12f)) continue;
            const float d = dot3(e1, e2), a = acosf(d < -1 ? -1 : d > 1 ? 1 : d);
            s[0] += a * u[t][0], s[1] += a * u[t][1], s[2] += a * u[t][2];
        }
        if (norm3(s) > 1e-20f) memcpy(out[v], s, sizeof s);
        else memcpy(out[v], r, sizeof r);
    }
    for (uint32_t v = 0; v < m->nv; v++) memcpy(at(m, v, m->nrm_off), out[v], sizeof out[v]);
    ret = 0;
done:
    free(pid), free(g), free(u), free(out), free(first), free(list), free(ftri);
    return ret;
}

void hat_mesh_free(HatMesh *m) {
    if (!m) return;
    free(m->v);
    free(m->idx);
    m->v = NULL;
    m->idx = NULL;
    m->nv = m->ni = 0;
}

/* One edge of the subdivision (the positions a < b): its cubic Bezier control points c1 (near a) and
 * c2 (near b), and its level - 1 inner points from a. */
typedef struct {
    uint32_t a, b, t, e;   /* sort entry: position ids, triangle, edge (corner e to e + 1) */
} EdgeUse;
static int use_cmp(const void *x, const void *y) {
    const EdgeUse *p = x, *q = y;
    if (p->a != q->a) return (p->a > q->a) - (p->a < q->a);
    if (p->b != q->b) return (p->b > q->b) - (p->b < q->b);
    if (p->t != q->t) return (p->t > q->t) - (p->t < q->t);
    return (p->e > q->e) - (p->e < q->e);
}
typedef struct {
    float c1[3], c2[3];
    float *pts;   /* (level - 1) x 3 */
} Edge;

/* The normals of an edge's uses at one end: up to two groups (sums) of normals within
 * HAT_SAME_COS of each other; bad: a third group or an unusable normal. */
#define HAT_SAME_COS 0.995f   /* within ~6 degrees: the same normal on both sides */
#define HAT_MAX_BEND 0.5f     /* an edge more than 30 degrees off its end's tangent plane stays straight there (no folds) */
typedef struct {
    float g[2][3];
    int n, bad;
} EndNormals;
static void end_add(EndNormals *e, const float *nrm) {
    float n[3] = {nrm[0], nrm[1], nrm[2]};
    if (!(norm3(n) > 1e-6f)) {
        e->bad = 1;
        return;
    }
    for (int k = 0; k < e->n; k++) {
        float r[3] = {e->g[k][0], e->g[k][1], e->g[k][2]};
        norm3(r);
        if (dot3(r, n) >= HAT_SAME_COS) {
            for (int x = 0; x < 3; x++) e->g[k][x] += n[x];
            return;
        }
    }
    if (e->n == 2) {
        e->bad = 1;
        return;
    }
    memcpy(e->g[e->n++], n, sizeof n);
}
/* The edge's Bezier control point near end p (d: to the other end, dn: d normalized): with one
 * normal N the PN point (2p + q - (d . N) N) / 3 (unless d leaves the tangent plane by more than
 * HAT_MAX_BEND), on a crease the point a third of the edge along
 * the crease (the cross product of its two normals, turned along d; when that is not along the edge
 * the edge is straight there), else the straight third. */
static void end_control(const EndNormals *e, const float *p, const float *d, const float *dn, int degenerate, float c[3]) {
    for (int x = 0; x < 3; x++) c[x] = p[x] + d[x] / 3;
    if (degenerate || e->bad || !e->n) return;
    float a[3] = {e->g[0][0], e->g[0][1], e->g[0][2]};
    norm3(a);
    if (e->n == 1) {
        const float w = dot3(d, a);
        if (fabsf(w) > HAT_MAX_BEND * sqrtf(dot3(d, d))) return;   /* the edge turns too far from the surface: straight */
        for (int x = 0; x < 3; x++) c[x] = p[x] + (d[x] - w * a[x]) / 3;
        return;
    }
    float b[3] = {e->g[1][0], e->g[1][1], e->g[1][2]}, t[3];
    norm3(b);
    cross3(a, b, t);
    if (!(norm3(t) > 1e-6f)) return;
    float k = dot3(t, dn);
    if (fabsf(k) < 0.7f) return;   /* the edge leaves the crease: straight */
    const float len = sqrtf(dot3(d, d)) / 3 * (k < 0 ? -1 : 1);
    for (int x = 0; x < 3; x++) c[x] = p[x] + t[x] * len;
}

int hat_subdivide(const HatMesh *m, int level, HatMesh *out) {
    memset(out, 0, sizeof *out);
    if (level < 1 || !mesh_ok(m)) return -1;
    const uint32_t nt = m->ni / 3, L = (uint32_t)level, per = (L + 1) * (L + 2) / 2;
    if ((uint64_t)nt * per > 65535 || (uint64_t)nt * L * L * 3 > 0xFFFFFFFFu) return -1;
    uint32_t np = 0, *pid = position_ids(m, &np);
    EdgeUse *use = malloc(3 * nt * sizeof *use);
    uint32_t *tedge = malloc(3 * nt * sizeof *tedge);   /* triangle edge -> edge */
    Edge *edge = malloc(3 * nt * sizeof *edge);
    float *pts = L > 1 ? malloc((size_t)3 * nt * (L - 1) * 3 * sizeof *pts) : NULL;
    out->v = malloc((size_t)nt * per * m->stride);
    out->idx = malloc((size_t)nt * L * L * 3 * sizeof *out->idx);
    int ret = -1;
    if (!pid || !use || !tedge || !edge || (L > 1 && !pts) || !out->v || !out->idx) goto done;
    /* the edges by position: each computed once, from its uses' normals */
    for (uint32_t t = 0; t < nt; t++)
        for (uint32_t e = 0; e < 3; e++) {
            const uint32_t p = pid[m->idx[3 * t + e]], q = pid[m->idx[3 * t + (e + 1) % 3]];
            use[3 * t + e] = (EdgeUse){p < q ? p : q, p < q ? q : p, t, e};
        }
    qsort(use, 3 * nt, sizeof *use, use_cmp);
    uint32_t ne = 0;
    for (uint32_t i = 0; i < 3 * nt;) {
        uint32_t j = i;
        while (j < 3 * nt && use[j].a == use[i].a && use[j].b == use[i].b) j++;
        /* the normals at a and b of every use (its vertices at those positions), in up to two
         * groups per end (one: a smooth end; two: a crease, the edge follows it) */
        uint32_t va0 = 0, vb0 = 0;
        EndNormals ea = {0}, eb = {0};
        for (uint32_t k = i; k < j; k++) {
            const uint16_t *c = m->idx + 3 * use[k].t;
            uint32_t va = c[use[k].e], vb = c[(use[k].e + 1) % 3];
            if (pid[va] != use[k].a) {
                const uint32_t x = va;
                va = vb, vb = x;
            }
            if (k == i) va0 = va, vb0 = vb;
            end_add(&ea, at(m, va, m->nrm_off));
            end_add(&eb, at(m, vb, m->nrm_off));
            tedge[3 * use[k].t + use[k].e] = ne;
        }
        const float *pa = at(m, va0, m->pos_off), *pb = at(m, vb0, m->pos_off);
        Edge *ed = &edge[ne];
        float d[3], dn[3];
        sub3(pb, pa, d);
        memcpy(dn, d, sizeof dn);
        const int same = use[i].a == use[i].b || !(norm3(dn) > 1e-12f);
        end_control(&ea, pa, d, dn, same, ed->c1);
        const float back[3] = {-d[0], -d[1], -d[2]}, backn[3] = {-dn[0], -dn[1], -dn[2]};
        end_control(&eb, pb, back, backn, same, ed->c2);
        ed->pts = L > 1 ? pts + (size_t)ne * (L - 1) * 3 : NULL;
        for (uint32_t k = 1; k < L; k++) {
            const float s = (float)k / L, r = 1 - s;
            const float b0 = r * r * r, b1 = 3 * r * r * s, b2 = 3 * r * s * s, b3 = s * s * s;
            for (int x = 0; x < 3; x++) ed->pts[(k - 1) * 3 + x] = b0 * pa[x] + b1 * ed->c1[x] + b2 * ed->c2[x] + b3 * pb[x];
        }
        ne++;
        i = j;
    }
    /* the triangles' patches: row r (0..L) from corner 0 towards the edge 1-2, point c (0..r) */
    uint32_t ov = 0, oi = 0;
    for (uint32_t t = 0; t < nt; t++) {
        const uint16_t *c = m->idx + 3 * t;
        const float *P[3], *N[3];
        for (int k = 0; k < 3; k++) P[k] = at(m, c[k], m->pos_off), N[k] = at(m, c[k], m->nrm_off);
        /* control points along each edge e (corner e to e + 1): near e, near e + 1 */
        float cp[3][2][3];
        for (int e = 0; e < 3; e++) {
            const Edge *ed = &edge[tedge[3 * t + e]];
            const int fwd = pid[c[e]] <= pid[c[(e + 1) % 3]];
            memcpy(cp[e][0], fwd ? ed->c1 : ed->c2, 12);
            memcpy(cp[e][1], fwd ? ed->c2 : ed->c1, 12);
        }
        float b111[3];
        for (int x = 0; x < 3; x++) {
            const float E = (cp[0][0][x] + cp[0][1][x] + cp[1][0][x] + cp[1][1][x] + cp[2][0][x] + cp[2][1][x]) / 6;
            const float V = (P[0][x] + P[1][x] + P[2][x]) / 3;
            b111[x] = E + (E - V) / 2;
        }
        const uint32_t base = ov;
        for (uint32_t r = 0; r <= L; r++)
            for (uint32_t q = 0; q <= r; q++) {
                const uint32_t i0 = L - r, i1 = r - q, i2 = q;   /* corner weights x L */
                const float w[3] = {(float)i0 / L, (float)i1 / L, (float)i2 / L};
                uint8_t *o = out->v + (size_t)ov * m->stride;
                const int near = i0 >= i1 && i0 >= i2 ? 0 : i1 >= i2 ? 1 : 2;
                memcpy(o, m->v + (size_t)c[near] * m->stride, m->stride);
                ov++;
                if (i0 == L || i1 == L || i2 == L) continue;   /* a corner: the original vertex */
                float p[3];
                int e = -1;
                uint32_t steps = 0;   /* an edge point: on edge e, steps from its first corner */
                if (!i2) e = 0, steps = i1;
                else if (!i0) e = 1, steps = i2;
                else if (!i1) e = 2, steps = i0;
                if (e >= 0) {
                    const Edge *ed = &edge[tedge[3 * t + e]];
                    const int fwd = pid[c[e]] <= pid[c[(e + 1) % 3]];
                    memcpy(p, ed->pts + (size_t)((fwd ? steps : L - steps) - 1) * 3, sizeof p);
                } else {
                    const float u = w[0], v = w[1], s = w[2];
                    for (int x = 0; x < 3; x++)
                        p[x] = P[0][x] * u * u * u + P[1][x] * v * v * v + P[2][x] * s * s * s + 3 * cp[0][0][x] * u * u * v +
                               3 * cp[0][1][x] * u * v * v + 3 * cp[1][0][x] * v * v * s + 3 * cp[1][1][x] * v * s * s + 3 * cp[2][0][x] * s * s * u +
                               3 * cp[2][1][x] * s * u * u + 6 * b111[x] * u * v * s;
                }
                memcpy(o + m->pos_off, p, sizeof p);
                float n[3];
                for (int x = 0; x < 3; x++) n[x] = w[0] * N[0][x] + w[1] * N[1][x] + w[2] * N[2][x];
                if (norm3(n) > 1e-12f) memcpy(o + m->nrm_off, n, sizeof n);
                for (int a = 0; a < m->nattr; a++) {
                    const HatAttr *ha = &m->attr[a];
                    if (ha->kind == HAT_ATTR_FLOAT)
                        for (int k = 0; k < ha->n; k++) {
                            float f = 0;
                            for (int j = 0; j < 3; j++) f += w[j] * ((const float *)(m->v + (size_t)c[j] * m->stride + ha->off))[k];
                            ((float *)(o + ha->off))[k] = f;
                        }
                    else
                        for (int k = 0; k < ha->n; k++) {
                            float f = 0.5f;
                            for (int j = 0; j < 3; j++) f += w[j] * m->v[(size_t)c[j] * m->stride + ha->off + k];
                            o[ha->off + k] = (uint8_t)(f > 255 ? 255 : f);
                        }
                }
            }
#define HAT_PT(r, q) (base + (r) * ((r) + 1) / 2 + (q))
        for (uint32_t r = 0; r < L; r++)
            for (uint32_t q = 0; q <= r; q++) {
                out->idx[oi++] = (uint16_t)HAT_PT(r, q);
                out->idx[oi++] = (uint16_t)HAT_PT(r + 1, q);
                out->idx[oi++] = (uint16_t)HAT_PT(r + 1, q + 1);
                if (q < r) {
                    out->idx[oi++] = (uint16_t)HAT_PT(r, q);
                    out->idx[oi++] = (uint16_t)HAT_PT(r + 1, q + 1);
                    out->idx[oi++] = (uint16_t)HAT_PT(r, q + 1);
                }
            }
#undef HAT_PT
    }
    out->nv = ov, out->ni = oi, out->stride = m->stride, out->pos_off = m->pos_off, out->nrm_off = m->nrm_off;
    out->nattr = m->nattr;
    memcpy(out->attr, m->attr, sizeof out->attr);
    ret = 0;
done:
    free(pid), free(use), free(tedge), free(edge), free(pts);
    if (ret) hat_mesh_free(out);
    return ret;
}
