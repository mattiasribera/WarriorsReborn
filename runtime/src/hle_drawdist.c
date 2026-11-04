/* Draw distance patches (see hle_drawdist.h). */
#include "hle_drawdist.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read by the title's far-plane code once --draw-distance points its operands here. The host
 * image is 32-bit and non-relocated (Linux non-PIE, Windows --no-dynamicbase), so the address
 * fits the operand. */
float drawdist_cap_scale = 1.5f, drawdist_cap_max = 300.0f;

/* Shipped bytes (XBE: .text VA = file offset + 0x10000, .rdata 0x40fb20 = file offset 0x402000). */
#define STEADY_SIG_VA 0x0032a5dcu   /* movss xmm1,[0x4b062c]; comiss xmm1,xmm0; jbe 0x32a660 */
static const uint8_t steady_sig[] = {0xf3, 0x0f, 0x10, 0x0d, 0x2c, 0x06, 0x4b, 0x00, 0x0f, 0x2f, 0xc8, 0x76, 0x77};
#define CAP_SCALE_VA 0x0032a510u    /* mulss xmm0,[0x41b830] (1.5) */
static const uint8_t cap_scale_sig[] = {0xf3, 0x0f, 0x59, 0x05, 0x30, 0xb8, 0x41, 0x00};
#define CAP_MAX_VA 0x0032a64du      /* movss xmm0,[0x430238] (300) */
static const uint8_t cap_max_sig[] = {0xf3, 0x0f, 0x10, 0x05, 0x38, 0x02, 0x43, 0x00};
#define PROP_CULL_VA 0x004212d4u    /* 0.0004f, 0.0005f */
#define PROP_CULL_BITS 0x39d1b717u
#define PROP_FADE_START_BITS 0x3a03126fu
#define PROP_FADE_VA 0x0042142cu    /* 2549999.25f = 255 / (0.0005 - 0.0004) */
#define PROP_FADE_BITS 0x4a1ba3bdu

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void set_sig(RateFix *f, const char *name, uint32_t va, const void *sig, size_t len) {
    memset(f, 0, sizeof *f);
    f->name = name;
    f->sig_va = va;
    f->sig_len = (uint8_t)len;
    memcpy(f->sig, sig, len);
}

static int fix_matches(const RateFix *f, intptr_t bias) {
    return !memcmp((const uint8_t *)(uintptr_t)(f->sig_va + bias), f->sig, f->sig_len);
}

static void fix_write(const RateFix *f, intptr_t bias) {
    for (int w = 0; w < RATEFIX_WRITES_MAX && f->writes[w].width; w++)
        memcpy((uint8_t *)(uintptr_t)(f->writes[w].va + bias), &f->writes[w].value, f->writes[w].width);
}

/* Appends "s" to the report, keeping it NUL-terminated and within cap. */
static void put(char *report, size_t cap, size_t *len, const char *s) {
    int n = snprintf(report + *len, cap - *len, "%s", s);
    if (n > 0) *len += (size_t)n < cap - *len ? (size_t)n : cap - *len - 1;
}

/* One patch = 1-2 byte-checked sites, written together only when every site matches. */
typedef struct {
    char label[40];
    RateFix sites[2];
    int nsites;
} Patch;

int drawdist_parse_factor(const char *s, float lo, float hi, float *out) {
    char *end;
    if (!s || !*s) return 0;
    float v = strtof(s, &end);
    if (*end || !isfinite(v) || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

int drawdist_parse_arg(int argc, char **argv, int *i, DrawDistOptions *o) {
    const char *a = argv[*i];
    if (!strcmp(a, "--fog-breathing")) { o->steady_fog = 0; return 1; }
    if (!strcmp(a, "--steady-fog")) { o->steady_fog = 1; return 1; }
    float *dst = !strcmp(a, "--draw-distance") ? &o->draw_distance : !strcmp(a, "--prop-detail") ? &o->prop_detail : NULL;
    if (!dst) return 0;
    int draw = dst == &o->draw_distance;
    if (*i + 1 >= argc) return -1;
    (*i)++;
    return drawdist_parse_factor(argv[*i], draw ? DRAWDIST_MIN : PROPDETAIL_MIN, draw ? DRAWDIST_MAX : PROPDETAIL_MAX, dst) ? 1 : -1;
}

/* The requested patches, in log order. Returns the count. */
static int build(const DrawDistOptions *o, Patch *p) {
    int n = 0;
    if (o->steady_fog) {
        Patch *x = &p[n++];
        snprintf(x->label, sizeof x->label, "steady fog");
        set_sig(&x->sites[0], "jbe 0x32a5e7", STEADY_SIG_VA, steady_sig, sizeof steady_sig);
        x->sites[0].writes[0] = (RateFixWrite){0x0032a5e7u, 1, 0x90};
        x->sites[0].writes[1] = (RateFixWrite){0x0032a5e8u, 1, 0x90};
        x->nsites = 1;
    }
    if (o->draw_distance != 1.0f) {
        Patch *x = &p[n++];
        drawdist_cap_scale = 1.5f * o->draw_distance;
        drawdist_cap_max = 300.0f * o->draw_distance;
        snprintf(x->label, sizeof x->label, "draw distance %.2fx", o->draw_distance);
        set_sig(&x->sites[0], "cap scale", CAP_SCALE_VA, cap_scale_sig, sizeof cap_scale_sig);
        x->sites[0].writes[0] = (RateFixWrite){CAP_SCALE_VA + 4, 4, (uint32_t)(uintptr_t)&drawdist_cap_scale};
        set_sig(&x->sites[1], "cap max", CAP_MAX_VA, cap_max_sig, sizeof cap_max_sig);
        x->sites[1].writes[0] = (RateFixWrite){CAP_MAX_VA + 4, 4, (uint32_t)(uintptr_t)&drawdist_cap_max};
        x->nsites = 2;
    }
    if (o->prop_detail != 1.0f) {
        Patch *x = &p[n++];
        float k = o->prop_detail * o->prop_detail;
        static const uint32_t cull[2] = {PROP_CULL_BITS, PROP_FADE_START_BITS}, fade = PROP_FADE_BITS;
        snprintf(x->label, sizeof x->label, "prop detail %.2fx", o->prop_detail);
        set_sig(&x->sites[0], "prop cull", PROP_CULL_VA, cull, sizeof cull);
        x->sites[0].writes[0] = (RateFixWrite){PROP_CULL_VA, 4, fbits(bitsf(PROP_CULL_BITS) / k)};
        x->sites[0].writes[1] = (RateFixWrite){PROP_CULL_VA + 4, 4, fbits(bitsf(PROP_FADE_START_BITS) / k)};
        set_sig(&x->sites[1], "prop fade", PROP_FADE_VA, &fade, sizeof fade);
        x->sites[1].writes[0] = (RateFixWrite){PROP_FADE_VA, 4, fbits(bitsf(PROP_FADE_BITS) * k)};
        x->nsites = 2;
    }
    return n;
}

int drawdist_install_at(const DrawDistOptions *o, intptr_t bias, char *report, size_t cap) {
    Patch patches[3];
    char done[256] = "", skipped[256] = "", item[96];
    size_t len = 0, dlen = 0, slen = 0;
    int n = build(o, patches), applied = 0;
    if (!cap) return 0;
    report[0] = 0;
    for (int i = 0; i < n; i++) {
        const Patch *x = &patches[i];
        int ok = 1;
        for (int s = 0; s < x->nsites; s++) ok &= fix_matches(&x->sites[s], bias);
        if (ok) {
            for (int s = 0; s < x->nsites; s++) fix_write(&x->sites[s], bias);
            snprintf(item, sizeof item, "%s%s (", dlen ? ", " : "", x->label);
            put(done, sizeof done, &dlen, item);
            for (int s = 0; s < x->nsites; s++) {
                snprintf(item, sizeof item, "%s%08x", s ? ", " : "", x->sites[s].writes[0].va);
                put(done, sizeof done, &dlen, item);
            }
            put(done, sizeof done, &dlen, ")");
            applied++;
        } else {
            snprintf(item, sizeof item, "%s%s (bytes differ)", slen ? ", " : "", x->label);
            put(skipped, sizeof skipped, &slen, item);
        }
    }
    put(report, cap, &len, "[drawdist] applied: ");
    put(report, cap, &len, dlen ? done : "none");
    put(report, cap, &len, "; skipped: ");
    put(report, cap, &len, slen ? skipped : "none");
    if (!o->steady_fog) put(report, cap, &len, "; fog breathing kept (--fog-breathing)");
    return applied;
}

int hle_drawdist_install(const DrawDistOptions *o) {
    char report[512];
    int n = drawdist_install_at(o, 0, report, sizeof report);
    printf("%s\n", report);
    return n;
}
