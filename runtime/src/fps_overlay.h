/* `--fps-overlay`: a tiny frame counter ("60 FPS  16.6 ms") in one corner
 * of the presented image. Pure parts here: the frame-rate meter, the text, the box layout, the glyph
 * indices the renderer's shader reads (src/fps_font.inc) and a CPU reference of the ink (tests).
 * The renderer draws it (renderer_fps_overlay_text); hle_d3d ticks the meter per presented frame. */
#ifndef FPS_OVERLAY_H
#define FPS_OVERLAY_H
#include <stdint.h>

/* Characters the font has, in glyph order (src/fps_font.inc). Any other character draws as a space. */
#define FPS_FONT_CHARS "0123456789.FPSms CUG%-"
#define FPS_FONT_GLYPHS 22
#define FPS_GLYPH_SPACE 16
#define FPS_FONT_W 5
#define FPS_FONT_H 7
#define FPS_ADVANCE 6     /* font pixels per character (5 + 1 gap) */
#define FPS_PAD 2         /* font pixels of box around the text */
#define FPS_MARGIN 3      /* font pixels between the box and the target's edges */
#define FPS_MAX_CHARS 40  /* longest text the renderer draws (renderer push constants: 10 words) */
#define FPS_WINDOW_MS 500.0

enum { FPS_CORNER_TL, FPS_CORNER_TR, FPS_CORNER_BL, FPS_CORNER_BR };

/* "tl" / "tr" / "bl" / "br" -> FPS_CORNER_*, -1 for anything else. */
int fps_overlay_corner(const char *s);

/* Presented frames counted over windows of at least FPS_WINDOW_MS. */
typedef struct {
    double t0;        /* start of the window (ms); 0 = not started */
    uint32_t frames;  /* frames presented in the window */
    float fps, ms;    /* last published values */
} FpsMeter;
/* One presented frame at now_ms (monotonic). Returns 1 when a window closed and fps / ms hold its
 * averages (frames per second, mean frame time), else 0. The first call only starts the window. */
int fps_meter_frame(FpsMeter *m, double now_ms);

/* "60 FPS  16.6 ms" (fps rounded, clamped to 0-999; ms to 0-999.9). Returns the length.
 * Same as fps_overlay_format_items(FPS_ITEMS_DEFAULT). */
int fps_overlay_format(char *buf, int size, float fps, float ms);

/* What the counter shows (--fps-overlay-items): a mask of FPS_ITEM_*, always in this order. */
enum { FPS_ITEM_FPS = 1, FPS_ITEM_MS = 2, FPS_ITEM_CPU = 4, FPS_ITEM_GPU = 8 };
#define FPS_ITEMS_DEFAULT (FPS_ITEM_FPS | FPS_ITEM_MS)   /* --fps-overlay alone */
/* "fps,ms,cpu,gpu" (any non-empty subset, any order) -> FPS_ITEM_* mask; -1 for anything else. */
int fps_overlay_items(const char *s);
/* The items of the mask, two spaces apart: "60 FPS  16.6 ms  CPU 34%  GPU 71%". fps and ms as in
 * fps_overlay_format; cpu and gpu are percentages rounded and clamped to 0-100, negative = unknown
 * ("CPU --", "GPU --"). Returns the length (the longest text is FPS_MAX_CHARS - 3). */
int fps_overlay_format_items(char *buf, int size, int items, float fps, float ms, float cpu, float gpu);

/* CPU %: cpu_ms of this process's CPU time (all its threads) over wall_ms, normalized so 100 =
 * every one of the cpus logical cores busy; clamped to 0-100. -1 when wall_ms <= 0 or cpus < 1. */
float fps_cpu_percent(double cpu_ms, double wall_ms, int cpus);
/* GPU busy time of one frame from its two timestamps: (end - begin) masked to valid_bits, times
 * period_ns (VkPhysicalDeviceLimits.timestampPeriod), in ms. -1 when valid_bits is 0 (no timestamps). */
double fps_gpu_busy_ms(uint64_t begin, uint64_t end, uint32_t valid_bits, float period_ns);
/* GPU %: busy_ms of GPU work over wall_ms, clamped to 0-100; -1 when busy_ms < 0 or wall_ms <= 0. */
float fps_gpu_percent(double busy_ms, double wall_ms);

/* Glyph indices (FPS_FONT_CHARS) of text, at most max; returns the count. */
int fps_overlay_glyphs(const char *text, uint8_t *glyph, int max);

/* Size of one font pixel in target pixels for a target tw x th: max(1, round(th / 450)). */
int fps_overlay_scale(int tw, int th);

/* The box of n characters in a tw x th target at a corner: x, y (top left, target pixels, y down),
 * w, h, and the font pixel scale. */
typedef struct {
    int x, y, w, h, scale;
} FpsBox;
FpsBox fps_overlay_box(int tw, int th, int corner, int n);

/* Whether the font pixel (fx, fy) of the text area (0, 0 = top left of the first glyph) is ink. */
int fps_overlay_ink(const uint8_t *glyph, int n, int fx, int fy);
#endif
