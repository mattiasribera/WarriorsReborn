/* --load-profile: see load_prof.h. */
#include "load_prof.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

int load_prof_on;
static uint64_t counters[LOAD_COUNTERS][3];

void load_prof_count(int counter, uint64_t bytes, uint64_t ns) {
    if (counter < 0 || counter >= LOAD_COUNTERS) return;
    __atomic_fetch_add(&counters[counter][0], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters[counter][1], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters[counter][2], ns, __ATOMIC_RELAXED);
}

void load_prof_snapshot(uint64_t out[LOAD_COUNTERS][3]) {
    for (int i = 0; i < LOAD_COUNTERS; i++)
        for (int k = 0; k < 3; k++) out[i][k] = __atomic_load_n(&counters[i][k], __ATOMIC_RELAXED);
}

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)s[n - e + i]) != ext[i]) return 0;
    return 1;
}

int load_prof_read_kind(const char *host_path) {
    if (!host_path) return LOAD_READ_DATA;
    if (ends_with(host_path, ".bik")) return LOAD_READ_MOVIE;
    static const char *const audio_ext[] = {".xwb", ".xsb", ".xgs", ".wav", ".wma"};
    for (size_t i = 0; i < sizeof audio_ext / sizeof *audio_ext; i++)
        if (ends_with(host_path, audio_ext[i])) return LOAD_READ_AUDIO;
    for (const char *p = host_path; *p; p++)   /* a file in a folder named audio */
        if ((*p == '/' || *p == '\\') && !strncmp(p + 1, "audio", 5) && (p[6] == '/' || p[6] == '\\')) return LOAD_READ_AUDIO;
    return LOAD_READ_DATA;
}

void load_window_begin(LoadWindow *w, const char *level, double t_ms, double cpu_ms, const uint64_t c0[LOAD_COUNTERS][3]) {
    memset(w, 0, sizeof *w);
    w->open = 1;
    snprintf(w->level, sizeof w->level, "%s", level ? level : "?");
    w->t0 = t_ms;
    w->cpu0 = cpu_ms;
    w->guest_ms = w->upload_ms = w->pipeline_ms = -1;   /* unmeasured until a frame has the field */
    memcpy(w->c0, c0, sizeof w->c0);
}

static void add(double *sum, double v) {
    if (v < 0) return;
    *sum = *sum < 0 ? v : *sum + v;
}

static void fmt_ms(char *out, size_t len, double v) {
    if (v < 0) snprintf(out, len, "?");
    else snprintf(out, len, "%.0f ms", v);
}

int load_window_frame(LoadWindow *w, const LoadFrame *f, const uint64_t c[LOAD_COUNTERS][3], char *line, size_t len) {
    if (!w->open) return 0;
    const double elapsed = f->t_ms - w->t0;
    w->frames++;
    if (f->frame_ms > w->longest) w->longest = f->frame_ms;
    add(&w->guest_ms, f->guest_ms);
    add(&w->upload_ms, f->upload_ms);
    if (f->pack_ms > 0) w->pack_ms += f->pack_ms;
    add(&w->pipeline_ms, f->pipeline_ms);
    if (elapsed > LOAD_PROF_TIMEOUT_MS) {
        snprintf(line, len, "[load] %s: no level frame within %.0f s (%u frames)", w->level, LOAD_PROF_TIMEOUT_MS / 1000, w->frames);
        w->open = 0;
        return 1;
    }
    if (f->movie || f->draws < LOAD_PROF_LEVEL_DRAWS) return 0;
    uint64_t d[LOAD_COUNTERS][3];
    for (int i = 0; i < LOAD_COUNTERS; i++)
        for (int k = 0; k < 3; k++) d[i][k] = c[i][k] - w->c0[i][k];
#define CALLS(i) (unsigned long long)d[i][0]
#define MB(i) d[i][1] / 1048576.0
#define MS(i) d[i][2] / 1e6
    char guest[32], upload[64], pipe[32];
    fmt_ms(guest, sizeof guest, w->guest_ms);
    if (w->upload_ms < 0) snprintf(upload, sizeof upload, "?");
    else snprintf(upload, sizeof upload, "%.0f ms (pack %.0f ms)", w->upload_ms, w->pack_ms);
    fmt_ms(pipe, sizeof pipe, w->pipeline_ms);
    snprintf(line, len,
             "[load] %s: %.0f ms to the first level frame (%u frames, longest %.0f ms) | guest frames %s | "
             "file reads %llu (%.1f MB, %.0f ms; audio %llu (%.1f MB, %.0f ms), movies %llu (%.1f MB, %.0f ms)) | "
             "texture creation %s | pipeline builds %s | pack loader reads %llu (%.1f MB, %.0f ms) | process CPU %.0f ms",
             w->level, elapsed, w->frames, w->longest, guest, CALLS(LOAD_READ_DATA), MB(LOAD_READ_DATA), MS(LOAD_READ_DATA),
             CALLS(LOAD_READ_AUDIO), MB(LOAD_READ_AUDIO), MS(LOAD_READ_AUDIO), CALLS(LOAD_READ_MOVIE), MB(LOAD_READ_MOVIE),
             MS(LOAD_READ_MOVIE), upload, pipe, CALLS(LOAD_PACK_READ), MB(LOAD_PACK_READ), MS(LOAD_PACK_READ), f->cpu_ms - w->cpu0);
#undef CALLS
#undef MB
#undef MS
    w->open = 0;
    return 1;
}
