/* `--fps-overlay`: pure parts (see fps_overlay.h). */
#include "fps_overlay.h"
#include <stdio.h>
#include <string.h>

static const uint32_t font[FPS_FONT_GLYPHS * 2] = {
#include "fps_font.inc"
};

int fps_overlay_corner(const char *s) {
    static const char *names[] = {"tl", "tr", "bl", "br"};
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

int fps_meter_frame(FpsMeter *m, double now_ms) {
    if (m->t0 <= 0) {
        m->t0 = now_ms;
        m->frames = 0;
        return 0;
    }
    m->frames++;
    double elapsed = now_ms - m->t0;
    if (elapsed + 0.5 < FPS_WINDOW_MS) return 0;   /* half a millisecond of slack: 15 frames of 33.3 ms close it */
    m->fps = (float)(m->frames * 1000.0 / elapsed);
    m->ms = (float)(elapsed / m->frames);
    m->t0 = now_ms;
    m->frames = 0;
    return 1;
}

int fps_overlay_format(char *buf, int size, float fps, float ms) {
    return fps_overlay_format_items(buf, size, FPS_ITEMS_DEFAULT, fps, ms, -1, -1);
}

int fps_overlay_items(const char *s) {
    static const char *names[] = {"fps", "ms", "cpu", "gpu"};
    int items = 0;
    for (;;) {
        size_t len = strcspn(s, ",");
        int bit = -1;
        for (int i = 0; i < 4; i++)
            if (len == strlen(names[i]) && !strncmp(s, names[i], len)) bit = i;
        if (bit < 0) return -1;   /* empty, unknown or malformed */
        items |= 1 << bit;
        if (!s[len]) return items;
        s += len + 1;
    }
}

/* A percentage for the text: rounded, 0-100; -1 (unknown) for a negative value. */
static int percent(float v) { return v < 0 ? -1 : v >= 99.5f ? 100 : (int)(v + 0.5f); }

int fps_overlay_format_items(char *buf, int size, int items, float fps, float ms, float cpu, float gpu) {
    if (size <= 0) return 0;
    char t[64];
    int n = 0;
    t[0] = 0;
    if (items & FPS_ITEM_FPS) n += snprintf(t + n, sizeof t - (size_t)n, "%d FPS", fps < 0 ? 0 : fps > 999 ? 999 : (int)(fps + 0.5f));
    if (items & FPS_ITEM_MS)
        n += snprintf(t + n, sizeof t - (size_t)n, "%s%.1f ms", n ? "  " : "", ms < 0 ? 0 : ms > 999.9f ? 999.9f : ms);
    const float pv[2] = {cpu, gpu};
    const char *pn[2] = {"CPU", "GPU"};
    for (int i = 0; i < 2; i++) {
        if (!(items & (FPS_ITEM_CPU << i))) continue;
        int v = percent(pv[i]);
        if (v < 0) n += snprintf(t + n, sizeof t - (size_t)n, "%s%s --", n ? "  " : "", pn[i]);
        else n += snprintf(t + n, sizeof t - (size_t)n, "%s%s %d%%", n ? "  " : "", pn[i], v);
    }
    if (n >= size) n = size - 1;
    memcpy(buf, t, (size_t)n);
    buf[n] = 0;
    return n;
}

float fps_cpu_percent(double cpu_ms, double wall_ms, int cpus) {
    if (wall_ms <= 0 || cpus < 1) return -1;
    double p = cpu_ms * 100.0 / (wall_ms * cpus);
    return p < 0 ? 0.0f : p > 100 ? 100.0f : (float)p;
}

double fps_gpu_busy_ms(uint64_t begin, uint64_t end, uint32_t valid_bits, float period_ns) {
    if (!valid_bits) return -1;
    const uint64_t mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
    return (double)((end - begin) & mask) * period_ns / 1e6;
}

float fps_gpu_percent(double busy_ms, double wall_ms) {
    if (busy_ms < 0 || wall_ms <= 0) return -1;
    double p = busy_ms * 100.0 / wall_ms;
    return p > 100 ? 100.0f : (float)p;
}

int fps_overlay_glyphs(const char *text, uint8_t *glyph, int max) {
    int n = 0;
    for (; text[n] && n < max; n++) {
        const char *p = strchr(FPS_FONT_CHARS, text[n]);
        glyph[n] = (uint8_t)(p ? p - FPS_FONT_CHARS : FPS_GLYPH_SPACE);
    }
    return n;
}

int fps_overlay_scale(int tw, int th) {
    (void)tw;
    int s = (th + 225) / 450;
    return s < 1 ? 1 : s;
}

FpsBox fps_overlay_box(int tw, int th, int corner, int n) {
    FpsBox b;
    b.scale = fps_overlay_scale(tw, th);
    b.w = (n * FPS_ADVANCE - 1 + 2 * FPS_PAD) * b.scale;
    b.h = (FPS_FONT_H + 2 * FPS_PAD) * b.scale;
    int m = FPS_MARGIN * b.scale;
    b.x = corner == FPS_CORNER_TR || corner == FPS_CORNER_BR ? tw - m - b.w : m;
    b.y = corner == FPS_CORNER_BL || corner == FPS_CORNER_BR ? th - m - b.h : m;
    return b;
}

/* Same lookup as shaders/fps_overlay.frag. */
int fps_overlay_ink(const uint8_t *glyph, int n, int fx, int fy) {
    if (fx < 0 || fy < 0 || fy >= FPS_FONT_H) return 0;
    int c = fx / FPS_ADVANCE, x = fx % FPS_ADVANCE;
    if (c >= n || x >= FPS_FONT_W || glyph[c] >= FPS_FONT_GLYPHS) return 0;
    uint32_t row = font[glyph[c] * 2 + (fy >> 2)] >> (8 * (fy & 3)) & 0x1Fu;
    return (int)(row >> (FPS_FONT_W - 1 - x) & 1u);
}
