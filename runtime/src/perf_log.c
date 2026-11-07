/* Always-on performance log lines: see perf_log.h. */
#include "perf_log.h"
#include <stdio.h>
#include <string.h>

int perf_is_hitch(double total_ms) { return total_ms > PERF_HITCH_MS; }

int perf_hitch_gate(PerfHitchGate *g, double now_ms, uint32_t *flush) {
    *flush = 0;
    if (g->lines >= PERF_HITCH_MAX_LINES) return 0;
    if (g->any && now_ms - g->last < PERF_HITCH_INTERVAL_MS) {
        g->suppressed++;
        return 0;
    }
    *flush = perf_hitch_flush(g);
    if (g->lines >= PERF_HITCH_MAX_LINES) return 0;
    g->lines++;
    g->last = now_ms;
    g->any = 1;
    return 1;
}

uint32_t perf_hitch_flush(PerfHitchGate *g) {
    const uint32_t n = g->suppressed;
    if (!n || g->lines >= PERF_HITCH_MAX_LINES) return 0;
    g->suppressed = 0;
    g->lines++;
    return n;
}

const char *perf_fmt(char *buf, size_t cap, double v) {
    if (v < 0) snprintf(buf, cap, "?");
    else snprintf(buf, cap, "%.1f", v);
    return buf;
}

static const char *level_or_unknown(const char *level) { return level && *level ? level : "?"; }

double perf_other_ms(const PerfFrame *f) {
    if (f->total < 0 || f->game < 0 || f->render < 0) return -1;
    const double o = f->total - f->game - f->render - (f->pace > 0 ? f->pace : 0);
    return o > 0 ? o : 0;
}

int perf_hitch_line(char *buf, size_t cap, uint32_t frame, const PerfFrame *f, const char *level) {
    char v[10][24];
    return snprintf(buf, cap,
                    "[hitch] frame %u: %s ms (game %s, hle %s, render %s, gpu %s, tex-upload %s, pipeline-compile %s ms, pace %s, wait %s, other %s ms) level %s%s",
                    frame, perf_fmt(v[0], sizeof v[0], f->total), perf_fmt(v[1], sizeof v[1], f->game), perf_fmt(v[2], sizeof v[2], f->hle),
                    perf_fmt(v[3], sizeof v[3], f->render), perf_fmt(v[4], sizeof v[4], f->gpu), perf_fmt(v[5], sizeof v[5], f->upload),
                    perf_fmt(v[6], sizeof v[6], f->pipeline), perf_fmt(v[7], sizeof v[7], f->pace), perf_fmt(v[8], sizeof v[8], f->wait),
                    perf_fmt(v[9], sizeof v[9], perf_other_ms(f)), level_or_unknown(level), f->loading ? " (loading)" : "");
}

int perf_suppressed_line(char *buf, size_t cap, uint32_t n) { return snprintf(buf, cap, "[hitch] %u more suppressed", n); }

void perf_level_reset(PerfLevel *l) { memset(l, 0, sizeof *l); }

void perf_level_frame(PerfLevel *l, const PerfFrame *f) {
    if (f->loading) {
        l->loading_frames++;
        return;
    }
    const double ms = f->total < 0 ? 0 : f->total;
    l->frames++;
    l->sum_ms += ms;
    if (ms > l->worst_ms) l->worst_ms = ms;
    const double b = ms / PERF_HIST_BIN_MS;
    l->hist[b >= PERF_HIST_BINS ? PERF_HIST_BINS : (int)b]++;
    if (perf_is_hitch(ms)) l->hitches++;
    if (f->cpu >= 0) l->cpu_ms += f->cpu, l->cpu_n++;
    if (f->gpu >= 0) l->gpu_ms += f->gpu, l->gpu_n++;
}

void perf_level_vram(PerfLevel *l, uint64_t bytes) {
    if (!l->vram_known || bytes > l->vram_peak) l->vram_peak = bytes;
    l->vram_known = 1;
}

void perf_level_tex_misses(PerfLevel *l, uint32_t misses, int known) {
    l->tex_misses += misses;
    if (known) l->tex_known = 1;
}

double perf_level_p99_ms(const PerfLevel *l) {
    if (!l->frames) return 0;
    /* The frame at rank ceil(0.99 n) in ascending order. */
    const uint32_t rank = (uint32_t)(((uint64_t)l->frames * 99 + 99) / 100);
    uint32_t seen = 0;
    for (int i = 0; i <= PERF_HIST_BINS; i++) {
        seen += l->hist[i];
        if (seen >= rank) return i * PERF_HIST_BIN_MS;
    }
    return PERF_HIST_BINS * PERF_HIST_BIN_MS;
}

double perf_level_low1_fps(const PerfLevel *l) {
    const double p = perf_level_p99_ms(l);
    return p > 0 ? 1000.0 / p : 0;
}

double perf_level_avg_fps(const PerfLevel *l) { return l->frames && l->sum_ms > 0 ? 1000.0 * l->frames / l->sum_ms : 0; }

int perf_level_line(char *buf, size_t cap, const PerfLevel *l, const char *level) {
    char a[24], b[24], w[24], c[24], g[24], v[24], m[24];
    const int any = l->frames > 0;
    perf_fmt(a, sizeof a, any ? perf_level_avg_fps(l) : -1);
    perf_fmt(b, sizeof b, any && perf_level_p99_ms(l) > 0 ? perf_level_low1_fps(l) : -1);
    perf_fmt(w, sizeof w, any ? l->worst_ms : -1);
    perf_fmt(c, sizeof c, l->cpu_n ? l->cpu_ms / l->cpu_n : -1);
    perf_fmt(g, sizeof g, l->gpu_n ? l->gpu_ms / l->gpu_n : -1);
    if (l->vram_known) snprintf(v, sizeof v, "%llu", (unsigned long long)((l->vram_peak + (1u << 19)) >> 20));
    else snprintf(v, sizeof v, "?");
    if (l->tex_known) snprintf(m, sizeof m, "%u", l->tex_misses);
    else snprintf(m, sizeof m, "?");
    return snprintf(buf, cap,
                    "[level-summary] level %s: %u frames, avg %s fps, 1%% low %s fps, worst %s ms, cpu %s ms/frame, gpu %s ms/frame, "
                    "vram peak %s MiB, hitches %u, tex misses %s",
                    level_or_unknown(level), l->frames, a, b, w, c, g, v, l->hitches, m);
}
