/* --ssao options, the opaque-world split rule and the projection scale (ssao.h). */
#include "ssao.h"
#include <math.h>
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

int ssao_arg(SsaoConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--ssao") || !strcmp(opt, "--no-ssao")) {
        c->on = opt[2] == 's';
        return 1;
    }
    if (!strcmp(opt, "--ssao-strength")) return parse_float(val, SSAO_STRENGTH_MIN, SSAO_STRENGTH_MAX, &c->strength) ? -1 : 2;
    if (!strcmp(opt, "--ssao-radius")) return parse_float(val, SSAO_RADIUS_MIN, SSAO_RADIUS_MAX, &c->radius) ? -1 : 2;
    return 0;
}

int ssao_opaque_draw(int offscreen, uint32_t draw_flags, uint32_t depth_write) {
    return !offscreen && (draw_flags & SSAO_DRAW_DEPTH_TEST) && depth_write;
}

uint32_t ssao_split(const uint8_t *kind, uint32_t n) {
    uint32_t last = n, clear = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (kind[i] == SSAO_OP_OPAQUE) last = i;
        else if (kind[i] == SSAO_OP_CLEAR) clear = i + 1;
    }
    return last < n && last >= clear ? last : n;
}

int ssao_proj_scale(const float c0[4], const float c1[4], const float c3[4], float out[2]) {
    const float n3 = sqrtf(c3[0] * c3[0] + c3[1] * c3[1] + c3[2] * c3[2]);
    if (!(n3 > 0)) return -1;
    const float px = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]) / n3;
    const float py = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]) / n3;
    if (!(px >= 0.2f && px <= 20.0f && py >= 0.2f && py <= 20.0f)) return -1;
    out[0] = px;
    out[1] = py;
    return 0;
}

int contact_arg(ContactConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--contact-shadows") || !strcmp(opt, "--no-contact-shadows")) {
        c->on = opt[2] == 'c';
        return 1;
    }
    if (!strcmp(opt, "--contact-shadows-strength")) return parse_float(val, CONTACT_STRENGTH_MIN, CONTACT_STRENGTH_MAX, &c->strength) ? -1 : 2;
    return 0;
}

int contact_build(const ContactConfig *c, const float view[16], const float proj[16], const float ab[2], RContact *out) {
    if (!c->on || !(fabsf(proj[11]) == 1 && proj[15] == 0 && proj[3] == 0 && proj[7] == 0 && proj[0] > 0 && proj[5] > 0)) return 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            const float dot = view[4 * i] * view[4 * j] + view[4 * i + 1] * view[4 * j + 1] + view[4 * i + 2] * view[4 * j + 2];
            if (!(fabsf(dot - (i == j)) < 1e-2f)) return 0;
        }
    RContact p;
    memset(&p, 0, sizeof p);
    p.depth_a = ab ? ab[0] : SSAO_DEPTH_A_DEFAULT;
    p.depth_b = ab ? ab[1] : SSAO_DEPTH_B_DEFAULT;
    p.px = proj[0];
    p.py = proj[5];
    /* World z (row 2 of the view's rotation) in view space, z towards the viewer (as rain_wet_build). */
    p.up_view[0] = view[8];
    p.up_view[1] = view[9];
    p.up_view[2] = -proj[11] * view[10];
    p.radius = CONTACT_RADIUS;
    p.strength = c->strength;
    *out = p;
    return 1;
}
