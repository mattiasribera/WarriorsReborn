/* --shadow-range options and the light projection fit (shadow_range.h). */
#include "shadow_range.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int shadow_range_arg(ShadowRangeConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--shadow-range") || !strcmp(opt, "--no-shadow-range")) {
        c->on = opt[2] == 's';
        return 1;
    }
    if (!strcmp(opt, "--shadow-range-dist")) {
        char *end;
        if (!val || !*val) return -1;
        float f = (float)strtod(val, &end);
        if (*end || !(f >= SHADOW_RANGE_DIST_MIN && f <= SHADOW_RANGE_DIST_MAX)) return -1;   /* NaN fails too */
        c->dist = f;
        return 2;
    }
    return 0;
}

int shadow_range_fit(const float *rq, size_t stride, uint32_t n, float ab[2]) {
    if (n < 2) return -1;
    double sq = 0, sr = 0, qmin = INFINITY, qmax = -INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        const double r = rq[i * stride], q = rq[i * stride + 1];
        if (!(q > 0) || !isfinite(r)) return -1;
        sq += q;
        sr += r;
        if (q < qmin) qmin = q;
        if (q > qmax) qmax = q;
    }
    if (!(qmax - qmin > 1e-4 * qmax)) return -1;
    const double mq = sq / n, mr = sr / n;
    double cqr = 0, cqq = 0;
    for (uint32_t i = 0; i < n; i++) {
        const double dq = rq[i * stride + 1] - mq;
        cqr += dq * (rq[i * stride] - mr);
        cqq += dq * dq;
    }
    const double a = cqr / cqq, b = a * mq - mr;
    if (!(a > 0) || !(b > 0)) return -1;
    for (uint32_t i = 0; i < n; i++) {   /* the depth r / q of every vertex within 16 units of A - B / q */
        const double r = rq[i * stride], q = rq[i * stride + 1];
        if (fabs(r - (a * q - b)) > 16.0 * q) return -1;
    }
    ab[0] = (float)a;
    ab[1] = (float)b;
    return 0;
}
