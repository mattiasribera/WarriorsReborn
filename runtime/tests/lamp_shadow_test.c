/* Game-independent test of the --lamp-shadows options and renderer input (src/lamp_shadow.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/lamp_shadow.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A light 100 units above z = 0 looking straight down, depth range 90-110 over D16. */
#define ZL 100.0
#define F 10.0
static const double A = 65535.0 * 110.0 / 20.0, B = 65535.0 * 110.0 / 20.0 * 90.0;
static void projection(float m[4][4]) {
    const float r[4][4] = {{F, 0, -0.5f, 0.5f * ZL}, {0, F, -0.5f, 0.5f * ZL}, {0, 0, (float)-A, (float)(A * ZL - B)}, {0, 0, -1, ZL}};
    memcpy(m, r, sizeof r);
}
static void apply(const float m[4][4], const double p[4], double out[4]) {
    for (int i = 0; i < 4; i++) out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] * p[3];
}

int main(void) {
    LampShadowConfig c = LAMP_SHADOW_DEFAULTS;
    CHECK(!c.on && c.strength == LAMP_SHADOW_STRENGTH_DEFAULT, "defaults: off, %.2f", c.strength);
    CHECK(lamp_shadow_arg(&c, "--lamp-lights", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(lamp_shadow_arg(&c, "--lamp-shadows", NULL) == 1 && c.on, "--lamp-shadows turns it on");
    CHECK(lamp_shadow_arg(&c, "--no-lamp-shadows", NULL) == 1 && !c.on, "--no-lamp-shadows turns it off (the last one wins)");
    CHECK(lamp_shadow_arg(&c, "--lamp-shadows-strength", "0.8") == 2 && c.strength == 0.8f && !c.on, "--lamp-shadows-strength sets the strength only");
    CHECK(lamp_shadow_arg(&c, "--lamp-shadows-strength", "0.1") == 2 && lamp_shadow_arg(&c, "--lamp-shadows-strength", "1") == 2 && c.strength == 1.0f,
          "strength range 0.1-1");
    const char *bad[] = {"0", "0.05", "-1", "1.5", "abc", "1x", "", "nan", "inf", NULL};
    for (int i = 0; i < 10; i++) {
        LampShadowConfig d = LAMP_SHADOW_DEFAULTS;
        CHECK(lamp_shadow_arg(&d, "--lamp-shadows-strength", bad[i]) == -1 && d.strength == LAMP_SHADOW_STRENGTH_DEFAULT,
              "--lamp-shadows-strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }

    float m[4][4], inv[4][4];
    projection(m);
    CHECK(lamp_shadow_invert(m, inv) == 0, "the light projection inverts");
    const double p[4] = {1.5, -2.0, 0.7, 1}, eye[3] = {0, -10, 3};
    double h[4], back[4];
    apply(m, p, h);
    apply((const float (*)[4])inv, h, back);
    CHECK(fabs(back[0] / back[3] - p[0]) < 1e-3 && fabs(back[1] / back[3] - p[1]) < 1e-3 && fabs(back[2] / back[3] - p[2]) < 1e-3,
          "world -> stq -> world round trip (%.4f %.4f %.4f)", back[0] / back[3], back[1] / back[3], back[2] / back[3]);
    const float sing[4][4] = {{1, 2, 3, 4}, {2, 4, 6, 8}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    float keep[4][4] = {{7}};
    CHECK(lamp_shadow_invert(sing, keep) == -1 && keep[0][0] == 7, "a singular matrix: -1, inv unchanged");

    /* Lights: one far, one near, one faded out, one beyond the light's far side (q <= 0). */
    LampShadowLight l[12] = {{{20, 0, 3}, 8, 1.0f}, {{-3, 0, 3}, 8, 2.0f}, {{0, 2, 3}, 8, 0.005f}, {{0, 0, 150}, 8, 1.0f}};
    RLampShadows out;
    const float e[3] = {(float)eye[0], (float)eye[1], (float)eye[2]};
    int k = lamp_shadow_setup((const float (*)[4])m, l, 4, e, 0.6f, &out);
    CHECK(k == 2 && out.n == 2, "faded and behind-the-light lights dropped (%d)", k);
    CHECK(out.l[0].pos[0] == -3 && out.l[1].pos[0] == 20, "nearest the camera first (%.1f %.1f)", out.l[0].pos[0], out.l[1].pos[0]);
    CHECK(out.l[0].weight == 1.0f && out.l[1].weight == 1.0f && out.l[0].radius == 8, "weight min(intensity, 1), radius kept");
    const double lp[4] = {-3, 0, 3, 1};
    apply(m, lp, h);
    CHECK(fabs(out.l[0].stq[0] - h[0]) < 1e-3 * fabs(h[0]) + 1e-3 && fabs(out.l[0].stq[2] - h[2]) < 1e-3 * fabs(h[2]) &&
              fabs(out.l[0].stq[3] - h[3]) < 1e-4,
          "the light in the stage's coordinates (%.3f %.3f %.3f %.3f)", out.l[0].stq[0], out.l[0].stq[1], out.l[0].stq[2], out.l[0].stq[3]);
    CHECK(fabs(out.depth_a - A) < 1e-4 * A && fabs(out.depth_b - B) < 1e-4 * B, "A %.6g B %.6g from the r and q rows", out.depth_a, out.depth_b);
    CHECK(out.strength == 0.6f && out.height == LAMP_SHADOW_HEIGHT && out.length == LAMP_SHADOW_LENGTH && out.start == LAMP_SHADOW_START &&
              out.dir_share == LAMP_SHADOW_DIR_SHARE,
          "strength and shape");
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) CHECK(fabsf(out.to_world[i][j] - inv[i][j]) <= 1e-6f * (1 + fabsf(inv[i][j])), "to_world is the inverse [%d][%d]", i, j);
    for (int i = 0; i < 12; i++) l[i] = (LampShadowLight){{(float)i, 0, 3}, 8, 1};
    k = lamp_shadow_setup((const float (*)[4])m, l, 12, e, 0.6f, &out);
    CHECK(k == RENDERER_LAMP_SHADOW_MAX && out.n == RENDERER_LAMP_SHADOW_MAX && out.l[0].pos[0] == 0 && out.l[7].pos[0] == 7,
          "at most %d lights, the nearest (%d)", RENDERER_LAMP_SHADOW_MAX, k);
    float ortho[4][4];
    projection(ortho);
    ortho[2][2] = 0, ortho[2][3] = 30000;   /* r constant: not r = A q - B */
    CHECK(lamp_shadow_setup((const float (*)[4])ortho, l, 2, e, 0.6f, &out) == -1 && out.n == 0, "not a perspective light: -1");
    CHECK(lamp_shadow_setup((const float (*)[4])sing, l, 2, e, 0.6f, &out) == -1 && out.n == 0, "singular projection: -1");
    CHECK(lamp_shadow_setup((const float (*)[4])m, l, 0, e, 0.6f, &out) == 0 && out.n == 0, "no lights: 0");

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
