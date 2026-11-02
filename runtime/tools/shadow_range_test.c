/* Game-independent test of the --shadow-range options and the light projection fit
 * (src/shadow_range.c). */
#include <math.h>
#include <stdio.h>
#include "../src/shadow_range.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    ShadowRangeConfig c = SHADOW_RANGE_DEFAULTS;
    CHECK(!c.on && c.dist == SHADOW_RANGE_DIST_DEFAULT, "defaults: off, %.2f", c.dist);
    CHECK(shadow_range_arg(&c, "--shadow-soft", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(shadow_range_arg(&c, "--shadow-range", NULL) == 1 && c.on, "--shadow-range turns it on");
    CHECK(shadow_range_arg(&c, "--no-shadow-range", NULL) == 1 && !c.on, "--no-shadow-range turns it off (the last one wins)");
    CHECK(shadow_range_arg(&c, "--shadow-range-dist", "4.5") == 2 && c.dist == 4.5f && !c.on, "--shadow-range-dist sets the distance only");
    CHECK(shadow_range_arg(&c, "--shadow-range-dist", "0.5") == 2 && shadow_range_arg(&c, "--shadow-range-dist", "50") == 2 && c.dist == 50.0f,
          "distance range 0.5-50");
    const char *bad[] = {"0", "0.4", "-1", "51", "abc", "2x", "", "nan", "inf", NULL};
    for (int i = 0; i < 10; i++) {
        ShadowRangeConfig d = SHADOW_RANGE_DEFAULTS;
        CHECK(shadow_range_arg(&d, "--shadow-range-dist", bad[i]) == -1 && d.dist == SHADOW_RANGE_DIST_DEFAULT,
              "--shadow-range-dist '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }

    /* A light 2629-2729 units away over the D16 range (the hideout's): depth = A - B / q. */
    const double A = 1.7887e6, B = 4.703e9;
    float v[6][5];   /* x, y, r, q, pad: stride 5 floats, r at index 2 */
    const double qs[6] = {2650.2, 2651.0, 2652.7, 2655.3, 2660.9, 2649.8};
    for (int i = 0; i < 6; i++) {
        v[i][0] = v[i][1] = v[i][4] = 0;
        v[i][3] = (float)qs[i];
        v[i][2] = (float)(A * qs[i] - B);
    }
    float ab[2] = {0, 0};
    CHECK(shadow_range_fit(&v[0][2], 5, 6, ab) == 0, "a perspective draw fits");
    CHECK(fabs(ab[0] - A) < 1e-3 * A && fabs(ab[1] - B) < 1e-3 * B, "A %.6g B %.6g recovered", ab[0], ab[1]);
    const double d = 30000, qd = ab[1] / (ab[0] - d), want = B / (A - d);
    CHECK(fabs(qd - want) < 0.05, "stored depth 30000 at view depth %.3f (%.3f)", qd, want);
    float keep[2] = {7, 8};
    CHECK(shadow_range_fit(&v[0][2], 5, 1, keep) == -1 && keep[0] == 7 && keep[1] == 8, "one vertex: no fit, ab unchanged");
    float flat[3][2] = {{1e7f, 2650}, {1e7f, 2650}, {1e7f, 2650}};
    CHECK(shadow_range_fit(&flat[0][0], 2, 3, keep) == -1, "constant q (orthographic light or flat-on draw): no fit");
    float noisy[3][2] = {{(float)(A * 2650 - B), 2650}, {(float)(A * 2660 - B) + 5e5f, 2660}, {(float)(A * 2670 - B), 2670}};
    CHECK(shadow_range_fit(&noisy[0][0], 2, 3, keep) == -1, "coordinates that are not one projection: no fit");
    float behind[2][2] = {{100, -1}, {200, 5}};
    CHECK(shadow_range_fit(&behind[0][0], 2, 2, keep) == -1, "q <= 0: no fit");
    float linear[3][2] = {{-1000, 1}, {0, 2}, {1000, 3}};   /* r = 1000 q - 2000, B > 0 but A q - B < 0 */
    CHECK(shadow_range_fit(&linear[0][0], 2, 3, keep) == 0 && fabs(keep[0] - 1000) < 1e-3 && fabs(keep[1] - 2000) < 1e-3,
          "an exact small projection fits (%g %g)", keep[0], keep[1]);

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
