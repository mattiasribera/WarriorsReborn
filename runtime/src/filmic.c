/* --filmic options, tonemap and grade math, procedural LUT and .cube parser (filmic.h). */
#include "filmic.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    float f = (float)strtod(v, &end);   /* compared as float: "0.1" is exactly the 0.1f bound */
    if (*end || !(f >= lo && f <= hi)) return -1;   /* NaN fails the range check */
    *out = f;
    return 0;
}

int filmic_arg(FilmicConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--filmic") || !strcmp(opt, "--no-filmic")) {
        c->on = opt[2] == 'f';
        return 1;
    }
    if (!strcmp(opt, "--filmic-strength")) return parse_float(val, 0.0f, 2.0f, &c->strength) ? -1 : 2;
    if (!strcmp(opt, "--filmic-grain")) return parse_float(val, 0.0f, 1.0f, &c->grain) ? -1 : 2;
    if (!strcmp(opt, "--filmic-vignette")) return parse_float(val, 0.0f, 1.0f, &c->vignette) ? -1 : 2;
    if (!strcmp(opt, "--filmic-lut")) {
        if (!val || !*val) return -1;
        c->lut_path = val;
        return 2;
    }
    return 0;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float minf(float a, float b) { return a < b ? a : b; }
static float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

float filmic_aces(float x) { return x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f); }

/* The tonemap's two knobs per unit of strength: how much of the ACES curve is blended in (the rest
 * is the scene's own values, so the look stays close to the original) and the pseudo-HDR range
 * the light-masked highlights are expanded into. */
#define FILMIC_BLEND 0.5f
#define FILMIC_HDR 1.0f
#define FILMIC_HALATION 0.3f

void filmic_params(float strength, float grain, float vignette, RFilmic *p) {
    const float s = strength < 0 ? 0 : strength > 2 ? 2 : strength;
    memset(p, 0, sizeof *p);
    p->hdr = FILMIC_HDR * s;
    p->white = 1 + p->hdr;
    p->blend = minf(1, FILMIC_BLEND * s);
    p->lut_mix = minf(1, s);
    p->halation = FILMIC_HALATION * s;
    p->grain = clamp01(grain);
    p->vignette = clamp01(vignette);
    /* Exposure: ACES(0.18 e) / ACES(white e) = 0.18 (linear mid-grey kept). The ratio rises from
     * 0.18 / white (e -> 0) toward 1, so a bisection finds it; white = 1 has no root above 0 and
     * no headroom to compress (blend is 0 there): e = 1. */
    float lo = 1e-3f, hi = 32, e = 1;
    if (p->white > 1.0001f) {
        for (int i = 0; i < 60; i++) {
            e = 0.5f * (lo + hi);
            if (filmic_aces(0.18f * e) / filmic_aces(p->white * e) < 0.18f) lo = e;
            else hi = e;
        }
    }
    p->exposure = e;
    p->norm = 1 / filmic_aces(p->white * e);
}

float filmic_tonemap(const RFilmic *p, float lin) {
    const float x = lin < 0 ? 0 : lin > p->white ? p->white : lin;
    return p->blend * filmic_aces(p->exposure * x) * p->norm + (1 - p->blend) * minf(x, 1);
}

float filmic_display(const RFilmic *p, float v, float mask) {
    const float lin = powf(clamp01(v), 2.2f);
    const float gain = 1 + p->hdr * clamp01(mask) * smoothstep(0.35f, 1.0f, lin);   /* pseudo-HDR */
    return powf(clamp01(filmic_tonemap(p, lin * gain)), 1 / 2.2f);
}

/* The grade's shape: the toe x^2 (1 + b) / (x + b) (0 and 1 fixed, flat at black: crushed),
 * saturation, the teal tint of the shadows (a bump 4 l (1 - l)^2: none at black or white, most
 * at l = 1/3) and the warm gain of the highlights (smoothstep from l = 0.45). */
#define GRADE_TOE 0.025f
#define GRADE_SATURATION 0.9f

static float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

void filmic_grade(const float in[3], float out[3]) {
    float c[3];
    for (int i = 0; i < 3; i++) {
        const float x = clamp01(in[i]);
        c[i] = x * x * (1 + GRADE_TOE) / (x + GRADE_TOE);
    }
    const float l = luma(c);
    for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * GRADE_SATURATION;
    const float sh = 4 * l * (1 - l) * (1 - l), hi = smoothstep(0.45f, 1.0f, l);
    c[0] = (c[0] - 0.035f * sh) * (1 + 0.05f * hi);
    c[1] = (c[1] + 0.008f * sh) * (1 + 0.01f * hi);
    c[2] = (c[2] + 0.03f * sh) * (1 - 0.07f * hi);
    for (int i = 0; i < 3; i++) out[i] = clamp01(c[i]);
}

void filmic_lut_build(int n, uint8_t *rgba) {
    if (n < 2 || n > 64) return;
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const float in[3] = {r / (float)(n - 1), g / (float)(n - 1), b / (float)(n - 1)};
                float out[3];
                filmic_grade(in, out);
                uint8_t *t = rgba + 4 * ((size_t)r + (size_t)g * n + (size_t)b * n * n);
                for (int i = 0; i < 3; i++) t[i] = (uint8_t)lrintf(out[i] * 255);
                t[3] = 255;
            }
}

/* ---- .cube ---- */

static int fail(char *err, size_t errn, int line, const char *why) {
    if (line) snprintf(err, errn, "line %d: %s", line, why);
    else snprintf(err, errn, "%s", why);
    return -1;
}

/* Reads three numbers from s; 0 when the line holds exactly them. */
static int three(const char *s, float v[3]) {
    char *end;
    for (int i = 0; i < 3; i++) {
        v[i] = (float)strtod(s, &end);
        if (end == s) return -1;
        s = end;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return *s ? -1 : 0;
}

uint8_t *filmic_cube_parse(const char *text, int *n, char *err, size_t errn) {
    int size = 0, line = 0;
    size_t count = 0, total = 0;
    uint8_t *lut = NULL;
    err[0] = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char buf[256];
        line++;
        if (len >= sizeof buf) {
            fail(err, errn, line, "line too long");
            goto bad;
        }
        memcpy(buf, p, len);
        buf[len] = 0;
        p += len + (e ? 1 : 0);
        char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        size_t k = strlen(s);
        while (k && (s[k - 1] == '\r' || s[k - 1] == ' ' || s[k - 1] == '\t')) s[--k] = 0;
        if (!*s || *s == '#' || !strncmp(s, "TITLE", 5)) continue;
        float v[3];
        if (!strncmp(s, "LUT_3D_SIZE", 11)) {
            char *end;
            long m = strtol(s + 11, &end, 10);
            if (size || end == s + 11 || *end || m < 2 || m > 64) {
                fail(err, errn, line, "LUT_3D_SIZE must be 2-64, given once");
                goto bad;
            }
            size = (int)m;
            total = (size_t)size * size * size;
            lut = malloc(total * 4);
            if (!lut) {
                fail(err, errn, 0, "out of memory");
                goto bad;
            }
        } else if (!strncmp(s, "DOMAIN_MIN", 10) || !strncmp(s, "DOMAIN_MAX", 10)) {
            const float want = s[8] == 'A' ? 1.0f : 0.0f;
            if (three(s + 10, v) || v[0] != want || v[1] != want || v[2] != want) {
                fail(err, errn, line, "only the default domain 0-1 is supported");
                goto bad;
            }
        } else if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.') {
            if (!size) {
                fail(err, errn, line, "data before LUT_3D_SIZE");
                goto bad;
            }
            if (three(s, v)) {
                fail(err, errn, line, "expected three numbers");
                goto bad;
            }
            if (count == total) {
                fail(err, errn, line, "more entries than LUT_3D_SIZE^3");
                goto bad;
            }
            uint8_t *t = lut + 4 * count++;
            for (int i = 0; i < 3; i++) t[i] = (uint8_t)lrintf(clamp01(v[i]) * 255);
            t[3] = 255;
        } else {
            fail(err, errn, line, "unsupported keyword (3D LUTs only)");
            goto bad;
        }
    }
    if (!size) {
        fail(err, errn, 0, "no LUT_3D_SIZE");
        goto bad;
    }
    if (count != total) {
        snprintf(err, errn, "%zu entries, LUT_3D_SIZE %d needs %zu", count, size, total);
        goto bad;
    }
    *n = size;
    return lut;
bad:
    free(lut);
    return NULL;
}

uint8_t *filmic_cube_load(const char *path, int *n, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errn, "cannot open %s", path);
        return NULL;
    }
    const size_t max = 16u << 20;
    char *text = malloc(max + 1);
    size_t len = text ? fread(text, 1, max + 1, f) : 0;
    fclose(f);
    if (!text || len > max) {
        free(text);
        snprintf(err, errn, "%s: %s", path, text ? "larger than 16 MB" : "out of memory");
        return NULL;
    }
    text[len] = 0;
    uint8_t *lut = filmic_cube_parse(text, n, err, errn);
    free(text);
    return lut;
}
