/* Geometry helpers for the host renderer (see geom.h). */
#include "geom.h"
#include <math.h>
#include <string.h>

int geom_type_size(uint32_t type) {
    uint32_t count = (type >> 4) & 0xF;
    switch (type) {
    case GEOM_D3DCOLOR: case GEOM_NORMPACKED3: return 4;
    case GEOM_FLOAT2H: return 12;
    case GEOM_NONE: return 0;
    }
    if (count < 1 || count > 4) return -1;
    switch (type & 0xF) {
    case 0x2: return (int)count * 4;          /* FLOATn */
    case 0x1: case 0x5: return (int)count * 2;   /* NORMSHORTn, SHORTn */
    case 0x4: return (int)count;              /* PBYTEn */
    default: return -1;
    }
}

/* Declaration tokens (D3DVSD_*): token type in bits 29-31. */
#define TOK_NOP 0u
#define TOK_STREAM 1u
#define TOK_STREAMDATA 2u
#define TOK_TESSELLATOR 3u
#define TOK_CONSTMEM 4u
#define TOK_EXT 5u
#define TOK_END 7u
int geom_layout_from_decl(const uint32_t *t, int max_tokens, GeomLayout *l) {
    memset(l, 0, sizeof *l);
    int stream = 0;
    for (int i = 0; i < max_tokens; i++) {
        uint32_t tok = t[i];
        switch (tok >> 29) {
        case TOK_END: return 0;
        case TOK_NOP: break;
        case TOK_STREAM: stream = tok & 0xF; if (stream > 3) return -1; break;
        case TOK_STREAMDATA:
            if (tok & 0x10000000u) {   /* SKIP (dwords) or SKIPBYTES */
                uint32_t n = (tok >> 16) & 0xFF;
                l->size[stream] += tok & 0x08000000u ? n : n * 4;
            } else {
                uint32_t type = (tok >> 16) & 0xFF;
                int size = geom_type_size(type);
                if (size < 0 || l->n == GEOM_MAX_ELEMENTS) return -1;
                GeomElement *e = &l->e[l->n++];
                e->reg = tok & 0x1F;
                e->stream = (uint8_t)stream;
                e->type = (uint8_t)type;
                e->offset = l->size[stream];
                l->size[stream] += (uint16_t)size;
            }
            break;
        case TOK_CONSTMEM: i += ((tok >> 25) & 0xF) * 4; break;   /* followed by count vec4s */
        case TOK_TESSELLATOR: case TOK_EXT: break;
        default: return -1;
        }
    }
    return -1;
}

static void add(GeomLayout *l, int reg, int type, int *off) {
    GeomElement *e = &l->e[l->n++];
    e->reg = (uint8_t)reg;
    e->stream = 0;
    e->type = (uint8_t)type;
    e->offset = (uint16_t)*off;
    *off += geom_type_size(type);
}
int geom_layout_from_fvf(uint32_t fvf, GeomLayout *l, int *rhw) {
    memset(l, 0, sizeof *l);
    int off = 0, pos = fvf & 0x00E;
    *rhw = pos == 0x004;
    if (pos == 0x002 || pos == 0x004) add(l, GEOM_REG_POSITION, pos == 0x004 ? GEOM_FLOAT4 : GEOM_FLOAT3, &off);
    else if (pos >= 0x006 && pos <= 0x00C) {   /* XYZB1..XYZB4: position plus 1-4 blend weights */
        add(l, GEOM_REG_POSITION, GEOM_FLOAT3, &off);
        add(l, GEOM_REG_WEIGHT, ((pos - 0x004) / 2) << 4 | 0x2, &off);   /* FLOATn */
    } else if (pos) return -1;
    if (fvf & 0x010) add(l, GEOM_REG_NORMAL, GEOM_FLOAT3, &off);
    if (fvf & 0x040) add(l, GEOM_REG_DIFFUSE, GEOM_D3DCOLOR, &off);
    if (fvf & 0x080) add(l, GEOM_REG_SPECULAR, GEOM_D3DCOLOR, &off);
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < ntex && i < 4; i++) {
        static const int sizes[4] = {GEOM_FLOAT2, GEOM_FLOAT3, GEOM_FLOAT4, GEOM_FLOAT1};
        add(l, GEOM_REG_TEX0 + (int)i, sizes[(fvf >> (16 + 2 * i)) & 3], &off);
    }
    l->size[0] = (uint16_t)off;
    return off;
}

const GeomElement *geom_find(const GeomLayout *l, int reg) {
    for (int i = 0; i < l->n; i++)
        if (l->e[i].reg == reg) return &l->e[i];
    return NULL;
}

static float sext(uint32_t v, int bits) {   /* sign-extends the low `bits` bits */
    int32_t s = (int32_t)(v << (32 - bits)) >> (32 - bits);
    return (float)s;
}
void geom_fetch(uint32_t type, const uint8_t *p, float out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    uint32_t count = (type >> 4) & 0xF, u;
    switch (type) {
    case GEOM_D3DCOLOR:
        memcpy(&u, p, 4);
        out[0] = ((u >> 16) & 0xFF) / 255.0f;
        out[1] = ((u >> 8) & 0xFF) / 255.0f;
        out[2] = (u & 0xFF) / 255.0f;
        out[3] = (u >> 24) / 255.0f;
        return;
    case GEOM_NORMPACKED3:   /* 11:11:10 signed normalized */
        memcpy(&u, p, 4);
        out[0] = sext(u, 11) / 1023.0f;
        out[1] = sext(u >> 11, 11) / 1023.0f;
        out[2] = sext(u >> 22, 10) / 511.0f;
        return;
    case GEOM_FLOAT2H:
        memcpy(&out[0], p, 8);
        memcpy(&out[3], p + 8, 4);
        return;
    case GEOM_NONE: return;
    }
    for (uint32_t i = 0; i < count && i < 4; i++) {
        int16_t sv;
        switch (type & 0xF) {
        case 0x2: memcpy(&out[i], p + 4 * i, 4); break;
        case 0x1: memcpy(&sv, p + 2 * i, 2); out[i] = sv / 32767.0f; break;
        case 0x5: memcpy(&sv, p + 2 * i, 2); out[i] = sv; break;
        case 0x4: out[i] = p[i] / 255.0f; break;
        }
    }
}

void geom_fetch_rows(uint32_t type, const uint8_t *p, uint32_t stride, int n, float *rows, int row_stride) {
    float *r0 = rows, *r1 = rows + row_stride, *r2 = rows + 2 * row_stride, *r3 = rows + 3 * row_stride;
    uint32_t count = (type >> 4) & 0xF, u;
    switch (type) {
    case GEOM_D3DCOLOR:
        for (int j = 0; j < n; j++, p += stride) {
            memcpy(&u, p, 4);
            r0[j] = ((u >> 16) & 0xFF) / 255.0f;
            r1[j] = ((u >> 8) & 0xFF) / 255.0f;
            r2[j] = (u & 0xFF) / 255.0f;
            r3[j] = (u >> 24) / 255.0f;
        }
        return;
    case GEOM_NORMPACKED3:
        for (int j = 0; j < n; j++, p += stride) {
            memcpy(&u, p, 4);
            r0[j] = sext(u, 11) / 1023.0f;
            r1[j] = sext(u >> 11, 11) / 1023.0f;
            r2[j] = sext(u >> 22, 10) / 511.0f;
            r3[j] = 1;
        }
        return;
    case GEOM_FLOAT2H:
        for (int j = 0; j < n; j++, p += stride) {
            memcpy(&r0[j], p, 4);
            memcpy(&r1[j], p + 4, 4);
            r2[j] = 0;
            memcpy(&r3[j], p + 8, 4);
        }
        return;
    }
    uint32_t kind = type & 0xF;
    if (type == GEOM_NONE || (kind != 0x1 && kind != 0x2 && kind != 0x4 && kind != 0x5)) count = 0;
    float *r[4] = {r0, r1, r2, r3};
    for (uint32_t q = count < 4 ? count : 4; q < 4; q++)   /* missing components: (0, 0, 0, 1) */
        for (int j = 0; j < n; j++) r[q][j] = q == 3 ? 1 : 0;
    for (uint32_t q = 0; q < count && q < 4; q++) {
        const uint8_t *s = p;
        float *o = r[q];
        int16_t sv;
        switch (kind) {
        case 0x2: for (int j = 0; j < n; j++, s += stride) memcpy(&o[j], s + 4 * q, 4); break;
        case 0x1: for (int j = 0; j < n; j++, s += stride) { memcpy(&sv, s + 2 * q, 2); o[j] = sv / 32767.0f; } break;
        case 0x5: for (int j = 0; j < n; j++, s += stride) { memcpy(&sv, s + 2 * q, 2); o[j] = sv; } break;
        case 0x4: for (int j = 0; j < n; j++, s += stride) o[j] = s[q] / 255.0f; break;
        }
    }
}

void geom_mat_mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}
void geom_row_xform(const float v[4], const float m[16], float out[4]) {
    float r[4];
    for (int j = 0; j < 4; j++) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(out, r, sizeof r);
}
void geom_dp4_rows(const float v[4], const float rows[16], float out[4]) {
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = v[0] * rows[i * 4] + v[1] * rows[i * 4 + 1] + v[2] * rows[i * 4 + 2] + v[3] * rows[i * 4 + 3];
    memcpy(out, r, sizeof r);
}

/* vsCharacter: a0.x = idx[k]; r2..r4 += w[k] * c[a0.x + base + 0..2]; r5.xyz = dp4(pos, r2..r4). */
void geom_skin3(const float pos[4], const float idx[4], const float w[4], const float (*c)[4], int base, float out[4]) {
    float rows[12] = {0};
    for (int k = 0; k < 3; k++) {
        int r = (int)lrintf(idx[k]) + base;
        if (r < 0 || r + 2 >= 192) continue;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) rows[i * 4 + j] += w[k] * c[r + i][j];
    }
    for (int i = 0; i < 3; i++) out[i] = pos[0] * rows[i * 4] + pos[1] * rows[i * 4 + 1] + pos[2] * rows[i * 4 + 2] + pos[3] * rows[i * 4 + 3];
    out[3] = 1;
}

void geom_screen_to_clip(const float s[4], float vx, float vy, float vw, float vh, float out[4]) {
    float w = s[3] > 0 ? 1.0f / s[3] : 1.0f;
    out[0] = (2.0f * (s[0] - vx) / vw - 1.0f) * w;
    out[1] = (1.0f - 2.0f * (s[1] - vy) / vh) * w;
    out[2] = s[2] * w;
    out[3] = w;
}

uint32_t geom_triangulated_max(uint32_t count) { return count < 3 ? 0 : (count - 2) * 3 + count; }

uint32_t geom_triangulate(uint32_t prim, uint32_t count, uint32_t *out, uint32_t max) {
    uint32_t n = 0;
#define TRI(a, b, c) do { if (n + 3 > max) return n; out[n++] = (a); out[n++] = (b); out[n++] = (c); } while (0)
    switch (prim) {
    case 5: for (uint32_t i = 0; i + 3 <= count; i += 3) TRI(i, i + 1, i + 2); break;
    case 6: for (uint32_t i = 0; i + 3 <= count; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case 7: case 10: for (uint32_t i = 1; i + 2 <= count; i++) TRI(0, i, i + 1); break;
    case 8: for (uint32_t i = 0; i + 4 <= count; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); } break;
    case 9: for (uint32_t i = 0; i + 4 <= count; i += 2) { TRI(i, i + 1, i + 3); TRI(i, i + 3, i + 2); } break;
    default: break;   /* points, lines, line loops/strips: not rendered */
    }
#undef TRI
    return n;
}

static uint32_t pack(float r, float g, float b) {
    uint32_t R = (uint32_t)lrintf(fminf(fmaxf(r, 0), 1) * 255), G = (uint32_t)lrintf(fminf(fmaxf(g, 0), 1) * 255),
             B = (uint32_t)lrintf(fminf(fmaxf(b, 0), 1) * 255);
    return R << 16 | G << 8 | B;
}
uint32_t geom_debug_color(const float *n, const float pos[4]) {
    if (n) {
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float x = len > 0 ? n[0] / len : 0, y = len > 0 ? n[1] / len : 1, z = len > 0 ? n[2] / len : 0;
        /* Normal as color, dimmed by a fixed light from above so shapes read as solids. */
        float light = 0.45f + 0.55f * fmaxf(0, 0.3f * x + 0.8f * y + 0.5f * z);
        return pack((0.5f + 0.5f * x) * light, (0.5f + 0.5f * y) * light, (0.5f + 0.5f * z) * light);
    }
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; i++) {
        int32_t q = (int32_t)floorf(pos[i] * 0.25f);
        h = (h ^ (uint32_t)q) * 16777619u;
    }
    return pack(0.3f + 0.7f * ((h >> 8) & 0xFF) / 255.0f, 0.3f + 0.7f * ((h >> 16) & 0xFF) / 255.0f, 0.3f + 0.7f * (h >> 24) / 255.0f);
}

/* Fixed-function lighting per D3D8 ("Mathematics of Lighting") as Cxbx-Reloaded implements it
 * (src/core/hle/D3D8/Direct3D9/FixedFunctionVertexShader.hlsl: DoMaterial, DoLight, CalcLighting):
 * diffuse = Me + Ga * Ma + sum(att * spot * (La * Ma + Ld * Md * max(N.L, 0))), alpha = Md.a;
 * specular = sum(att * spot * Ls * Ms * max(N.H, 0)^P) with H = normalize(L + V) when
 * D3DRS_SPECULARENABLE is set. Point/spot attenuation 1 / (a0 + a1 d + a2 d^2), zero beyond
 * Range; spot factor ((cos a - cos(phi/2)) / (cos(theta/2) - cos(phi/2)))^falloff. */
_Static_assert(sizeof(GeomLight) == 104, "D3DLIGHT8 layout");
_Static_assert(sizeof(GeomMaterial) == 68, "D3DMATERIAL8 layout");
static void color_of(const GeomLighting *L, uint32_t src, const float *mat, const float *vd, const float *vs, float out[4]) {
    const float *c = mat;
    if (L->color_vertex && src == 1 && vd) c = vd;
    if (L->color_vertex && src == 2 && vs) c = vs;
    memcpy(out, c, 16);
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *v) {
    float l = sqrtf(dot3(v, v));
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
void geom_light(const GeomLighting *L, const float pos[3], const float *normal, const float *vd, const float *vs, float d[4], float s[4]) {
    float md[4], ma[4], ms[4], me[4], n[3] = {0, 0, 0};
    color_of(L, L->dsrc, L->mat.diffuse, vd, vs, md);
    color_of(L, L->asrc, L->mat.ambient, vd, vs, ma);
    color_of(L, L->ssrc, L->mat.specular, vd, vs, ms);
    color_of(L, L->esrc, L->mat.emissive, vd, vs, me);
    if (normal) { memcpy(n, normal, 12); norm3(n); }
    float acc[3], spec[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) acc[k] = me[k] + L->ambient[k] * ma[k];
    for (int i = 0; i < L->n && i < GEOM_MAX_LIGHTS; i++) {
        const GeomLight *l = &L->light[i];
        float tl[3], att = 1, spot = 1;
        if (l->type == GEOM_LIGHT_DIRECTIONAL) {
            for (int k = 0; k < 3; k++) tl[k] = -l->dir[k];
            norm3(tl);
        } else {
            for (int k = 0; k < 3; k++) tl[k] = l->pos[k] - pos[k];
            float dist = sqrtf(dot3(tl, tl));
            if (dist > l->range) continue;
            norm3(tl);
            float den = l->att[0] + l->att[1] * dist + l->att[2] * dist * dist;
            att = den > 0 ? 1 / den : 1;
            if (l->type == GEOM_LIGHT_SPOT) {
                float ld[3] = {-l->dir[0], -l->dir[1], -l->dir[2]};
                norm3(ld);
                float rho = dot3(tl, ld), ct = cosf(l->theta / 2), cp = cosf(l->phi / 2);
                spot = rho > ct ? 1 : rho <= cp ? 0 : powf((rho - cp) / (ct - cp), l->falloff);
            }
        }
        float ndl = dot3(n, tl), f = att * spot;
        if (ndl < 0) ndl = 0;
        for (int k = 0; k < 3; k++) acc[k] += f * (l->ambient[k] * ma[k] + l->diffuse[k] * md[k] * ndl);
        if (L->specular_enable && ndl > 0) {
            float v[3], h[3];
            if (L->local_viewer) { for (int k = 0; k < 3; k++) v[k] = L->eye[k] - pos[k]; norm3(v); }
            else memcpy(v, L->view_dir, 12);
            for (int k = 0; k < 3; k++) h[k] = tl[k] + v[k];
            norm3(h);
            float ndh = dot3(n, h);
            float p = ndh > 0 ? powf(ndh, L->mat.power) : 0;
            for (int k = 0; k < 3; k++) spec[k] += f * l->specular[k] * ms[k] * p;
        }
    }
    for (int k = 0; k < 3; k++) { d[k] = sat(acc[k]); s[k] = sat(spec[k]); }
    d[3] = sat(md[3]);
    s[3] = 0;
}

/* As xemu evaluates the NV2A fog modes (hw/xbox/nv2a/pgraph/glsl/vsh.c): linear
 * f = (end - d) / (end - start), exp f = 2^(-density d / ln 2) = e^(-density d), exp2
 * f = e^(-(density d)^2). */
float geom_fog_factor(uint32_t mode, float start, float end, float density, float d) {
    float f;
    switch (mode) {
    case GEOM_FOG_LINEAR: f = end != start ? (end - d) / (end - start) : 1; break;
    case GEOM_FOG_EXP: f = expf(-density * d); break;
    case GEOM_FOG_EXP2: f = expf(-(density * d) * (density * d)); break;
    default: f = d; break;
    }
    return f != f ? 1 : sat(f);   /* NaN -> no fog, as xemu */
}
