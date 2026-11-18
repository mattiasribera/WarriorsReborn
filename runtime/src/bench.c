/* --bench: statistics, settings and report text (see bench.h). */
#include "bench.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BenchOptions bench_options;

static int desc(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

void bench_summarize(const float *frame_ms, const float *gpu_ms, uint32_t n, BenchSummary *out) {
    memset(out, 0, sizeof *out);
    out->gpu_ms = -1;
    float *v = n ? malloc(n * sizeof *v) : NULL;
    uint32_t k = 0, gn = 0;
    double sum = 0, gsum = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (gpu_ms && gpu_ms[i] > 0) {
            gsum += gpu_ms[i];
            gn++;
        }
        if (!(frame_ms[i] > 0) || !v) continue;
        v[k++] = frame_ms[i];
        sum += frame_ms[i];
    }
    if (gn) out->gpu_ms = gsum / gn;
    if (k) {
        qsort(v, k, sizeof *v, desc);
        const uint32_t slow = (k + 99) / 100;   /* the slowest 1 %, at least one frame */
        double ssum = 0;
        for (uint32_t i = 0; i < slow; i++) ssum += v[i];
        out->frames = k;
        out->seconds = sum / 1000;
        out->avg_fps = k * 1000.0 / sum;
        out->low1_fps = 1000.0 * slow / ssum;
        out->worst_ms = v[0];
    }
    free(v);
}

static size_t put(char *buf, size_t len, size_t k, const char *fmt, ...) {
    if (k + 1 >= len) return k;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + k, len - k, fmt, ap);
    va_end(ap);
    if (n < 0) return k;
    return k + (size_t)n >= len ? len - 1 : k + (size_t)n;
}

void bench_settings(int argc, char **argv, char *buf, size_t len) {
    size_t k = 0;
    int started = 0;
    if (!len) return;
    buf[0] = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!started && strncmp(a, "--", 2)) continue;   /* the executable's own paths */
        started = 1;
        if (!strcmp(a, "--bench")) continue;
        if (!strcmp(a, "--bench-out")) {
            i++;
            continue;
        }
        const int path = strchr(a, '/') || strchr(a, '\\');
        k = put(buf, len, k, "%s%s", k ? " " : "", path ? "<path>" : a);
    }
}

size_t bench_report(char *buf, size_t len, const BenchReport *r) {
    size_t k = 0;
    if (!len) return 0;
    buf[0] = 0;
    k = put(buf, len, k, "WarriorsReborn benchmark (--bench): mission %s street, %d s camera pan\n", BENCH_WARP, BENCH_FRAMES / 60);
    k = put(buf, len, k, "version: %s\n", r->version ? r->version : "?");
    if (!r->sum) {
        k = put(buf, len, k, "result: failed: %s\n", r->failure ? r->failure : "unknown");
        k = put(buf, len, k, "gpu: %s\n", r->gpu ? r->gpu : "?");
        k = put(buf, len, k, "settings: %s\n", r->settings ? r->settings : "");
        return k;
    }
    const BenchSummary *s = r->sum;
    k = put(buf, len, k, "level: %s\n", r->level && *r->level ? r->level : "?");
    k = put(buf, len, k, "fps: average %.1f, 1%% low %.1f, worst frame %.1f ms (%u frames, %.1f s)\n", s->avg_fps, s->low1_fps, s->worst_ms,
            s->frames, s->seconds);
    if (s->gpu_ms > 0) k = put(buf, len, k, "gpu: %s, %.2f ms per frame\n", r->gpu ? r->gpu : "?", s->gpu_ms);
    else k = put(buf, len, k, "gpu: %s, no timestamps\n", r->gpu ? r->gpu : "?");
    if (r->render_w_min != r->render_w_max || r->render_h_min != r->render_h_max)
        k = put(buf, len, k, "render: %dx%d to %dx%d (mean width %.0f), output %dx%d, dynamic resolution %s\n", r->render_w_min, r->render_h_min,
                r->render_w_max, r->render_h_max, r->render_w_mean, r->out_w, r->out_h, r->dynres ? "on" : "off");
    else
        k = put(buf, len, k, "render: %dx%d, output %dx%d, dynamic resolution %s\n", r->render_w_max, r->render_h_max, r->out_w, r->out_h,
                r->dynres ? "on" : "off");
    k = put(buf, len, k, "passes: %s\n", r->passes && *r->passes ? r->passes : "(none)");
    k = put(buf, len, k, "settings: %s\n", r->settings ? r->settings : "");
    return k;
}
