/* Keyboard and mouse button icons for the game's font/icon atlas (see kbm_glyphs.h). */
#include "kbm_glyphs.h"
#include "input_map.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- The glyph rect table ---- */

#define TABLE_OFFSET 0x861f4u   /* first record in the known global.pak */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Records of a table whose 16-byte header ends at rec_off, or -1 when it does not fit. */
static int table_at(const uint8_t *d, size_t n, size_t rec_off, KbmRect *out, int max) {
    if (rec_off < 16 || rec_off > n) return -1;
    const uint8_t *h = d + rec_off - 16;
    uint32_t count = rd32(h);
    if (rd32(h + 4) != KBM_RECT_BASE || rd32(h + 8) || rd32(h + 12) || count < 0x100 || count > KBM_RECTS_MAX ||
        (n - rec_off) / 16 < count)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        KbmRect r;
        memcpy(&r, d + rec_off + 16 * i, 16);
        if (!(r.u0 >= 0 && r.u0 <= r.u1 && r.u1 <= 1 && r.v0 >= 0 && r.v0 <= r.v1 && r.v1 <= 1)) return -1;
    }
    for (uint32_t i = 0; i < count && (int)i < max; i++) memcpy(&out[i], d + rec_off + 16 * i, 16);
    return (int)count;
}

int kbm_rects_parse(const uint8_t *d, size_t n, KbmRect *out, int max) {
    int c = table_at(d, n, TABLE_OFFSET, out, max);
    if (c > 0) return c;
    for (size_t pos = 4; pos + 12 <= n; pos += 4)   /* {base, 0, 0} at a 4-aligned offset */
        if (rd32(d + pos) == KBM_RECT_BASE && !rd32(d + pos + 4) && !rd32(d + pos + 8) &&
            (c = table_at(d, n, pos + 12, out, max)) > 0)
            return c;
    return -1;
}

static uint8_t *read_range(const char *path, long off, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = size ? malloc(size) : NULL;
    if (buf && (fseek(f, off, SEEK_SET) || fread(buf, 1, size, f) != size)) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    return n;
}

int kbm_rects_load(const char *disc, KbmRect *out, int max) {
    char path[1024];
    snprintf(path, sizeof path, "%s/XBoxWad.idx", disc);
    long n = file_size(path);
    if (n < 20 || n > (4l << 20)) return -1;
    uint8_t *idx = read_range(path, 0, (size_t)n);
    if (!idx) return -1;
    uint32_t wads = rd32(idx), files = rd32(idx + 4), wad_table = rd32(idx + 12), file_table = rd32(idx + 16);
    int c = -1;
    if (wads <= 256 && wad_table <= (uint32_t)n && ((uint32_t)n - wad_table) / 36 >= wads && file_table <= (uint32_t)n &&
        ((uint32_t)n - file_table) / 16 >= files) {
        for (uint32_t i = 0; i < files; i++) {
            const uint8_t *f = idx + file_table + 16 * i;
            uint32_t loc = rd32(f), size = rd32(f + 4);
            if (rd32(f + 12) != KBM_PAK_NAME_HASH || loc >> 24 >= wads || size > (16u << 20)) continue;
            char name[33];
            memcpy(name, idx + wad_table + 36 * (loc >> 24), 32);
            name[32] = 0;
            snprintf(path, sizeof path, "%s/%s", disc, name);
            uint8_t *pak = read_range(path, (long)(loc & 0xffffff) << 10, size);
            if (pak) c = kbm_rects_parse(pak, size, out, max);
            free(pak);
            break;
        }
    }
    free(idx);
    return c;
}

/* ---- Button cells ---- */

/* Character codes (record = code + 0x5e) as the Xbox glyph each one draws, then the large
 * face-button sprites (record indices). The trigger and stick-click tags draw a letter-badge
 * variant in Spanish, French and Italian (the tag switch at 0xe2580 picks it by the game language
 * [[0x5b9f78]+0x11c]: 1 es, 2 fr, 3 it): right trigger 0xa2 "D", left trigger 0xa3 "I" / 0xa5 "G"
 * / 0xa4 "S", left stick click 0xa6 "I" / 0xa7 "G" / 0xb3 "S", right stick click 0xb2 "D". They
 * get the same key icon as the English cell. */
static const KbmCell cells[] = {
    {0x9f + KBM_RECT_BASE, IN_X, "X"}, {0x9e + KBM_RECT_BASE, IN_A, "A"}, {0x9d + KBM_RECT_BASE, IN_B, "B"},
    {0x96 + KBM_RECT_BASE, IN_Y, "Y"}, {0x9c + KBM_RECT_BASE, IN_BLACK, "Black"}, {0x91 + KBM_RECT_BASE, IN_WHITE, "White"},
    {0x97 + KBM_RECT_BASE, IN_START, "Start"}, {0x93 + KBM_RECT_BASE, IN_BACK, "Back"},
    {0x95 + KBM_RECT_BASE, IN_LT, "left trigger"}, {0xa0 + KBM_RECT_BASE, IN_RT, "right trigger"},
    {0x94 + KBM_RECT_BASE, IN_LTHUMB, "left stick click"}, {0x92 + KBM_RECT_BASE, IN_RTHUMB, "right stick click"},
    {0x98 + KBM_RECT_BASE, IN_LEFT, "D-pad left"}, {0x99 + KBM_RECT_BASE, IN_UP, "D-pad up"},
    {0x9a + KBM_RECT_BASE, IN_RIGHT, "D-pad right"}, {0x9b + KBM_RECT_BASE, IN_DOWN, "D-pad down"},
    {0xa2 + KBM_RECT_BASE, IN_RT, "right trigger D (es/fr/it)"}, {0xa3 + KBM_RECT_BASE, IN_LT, "left trigger I (es)"},
    {0xa5 + KBM_RECT_BASE, IN_LT, "left trigger G (fr)"}, {0xa4 + KBM_RECT_BASE, IN_LT, "left trigger S (it)"},
    {0xa6 + KBM_RECT_BASE, IN_LTHUMB, "left stick click I (es)"}, {0xa7 + KBM_RECT_BASE, IN_LTHUMB, "left stick click G (fr)"},
    {0xb3 + KBM_RECT_BASE, IN_LTHUMB, "left stick click S (it)"}, {0xb2 + KBM_RECT_BASE, IN_RTHUMB, "right stick click D (es/fr/it)"},
    {79, IN_A, "large A"}, {73, IN_Y, "large Y"}, {65, IN_X, "large X"}, {6, IN_B, "large B"},
};
#define NCELLS (int)(sizeof cells / sizeof *cells)
#define FIRST_SPRITE 24

int kbm_cell_count(void) { return NCELLS; }
const KbmCell *kbm_cell(int i) { return i >= 0 && i < NCELLS ? &cells[i] : NULL; }

/* ---- Key labels ---- */

const char *kbm_key_label(int key, int short_form) {
    static char one[128][2], fkeys[12][4];
    static const struct { int key; const char *full, *brief; } named[] = {
        {' ', "SPACE", "SPC"}, {'\r', "ENTER", "ENT"}, {'\b', "BKSP", "BS"}, {'\t', "TAB", "TAB"},
        {INPUT_KEY_LSHIFT, "SHIFT", "SHF"}, {INPUT_KEY_RSHIFT, "SHIFT", "SHF"}, {INPUT_KEY_LCTRL, "CTRL", "CTL"},
        {INPUT_KEY_RCTRL, "CTRL", "CTL"}, {INPUT_KEY_LALT, "ALT", "ALT"}, {INPUT_KEY_RALT, "ALT", "ALT"},
        {INPUT_KEY_UP, "\x01", "\x01"}, {INPUT_KEY_DOWN, "\x02", "\x02"}, {INPUT_KEY_LEFT, "\x03", "\x03"},
        {INPUT_KEY_RIGHT, "\x04", "\x04"},
    };
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (named[i].key == key) return short_form ? named[i].brief : named[i].full;
    if (key >= INPUT_KEY_F1 && key < INPUT_KEY_F1 + 12) {
        snprintf(fkeys[key - INPUT_KEY_F1], 4, "F%d", key - INPUT_KEY_F1 + 1);
        return fkeys[key - INPUT_KEY_F1];
    }
    if (key > ' ' && key < 127 && strchr("abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./`", key)) {
        one[key][0] = (char)(key >= 'a' && key <= 'z' ? key - 32 : key);
        return one[key];
    }
    return NULL;
}

/* ---- Key symbols ---- */

int kbm_key_symbols = 1;

int kbm_key_symbol(int key) {
    switch (key) {
    case INPUT_KEY_LSHIFT: case INPUT_KEY_RSHIFT: return KBM_SYM_SHIFT;
    case '\r': return KBM_SYM_ENTER;
    case '\t': return KBM_SYM_TAB;
    case '\b': return KBM_SYM_BACKSPACE;
    case ' ': return KBM_SYM_SPACE;
    default: return KBM_SYM_NONE;
    }
}

/* Symbols as polylines on an 8 x 8 grid (y down), in the stroke font's "xy" point format. */
static const char *symbol_strokes(int sym) {
    switch (sym) {
    case KBM_SYM_SHIFT: return "4084646828240440";            /* hollow up arrow with a stem */
    case KBM_SYM_ENTER: return "717515 331537";                /* down, then left to an arrowhead */
    case KBM_SYM_TAB: return "0464 426446 8187";               /* right arrow to a bar */
    case KBM_SYM_BACKSPACE: return "042181872704 4365 6345";   /* left-pointing key with a cross */
    case KBM_SYM_SPACE: return "13167673";                     /* a bracket open at the top */
    default: return "";
    }
}

/* ---- Stroke font ----
 * Glyphs on a 4 x 6 grid (y down), as polylines of two-digit points "xy" separated by spaces.
 * A point repeated is a dot. Control characters 1-4 are the arrows up, down, left, right. */
static const char *glyph(char c) {
    static const char *const letters[26] = {
        "062046 1333", "0006 0030414233 033344453606", "4130100105163645", "0006 003041453606",
        "40000646 0333", "400006 0333", "41301001051636454323", "0006 4046 0343", "0040 2026 0646",
        "204045361605", "0006 4004 1346", "000646", "0600234046", "06004640", "103041453616050110",
        "06003041423303", "103041453616050110 2446", "06003041423303 2346", "413010010213334445361605",
        "0040 2026", "000516364540", "002640", "0016233640", "0046 4006", "002340 2326", "00400646",
    };
    static const char *const digits[10] = {
        "103041453616050110 4105", "1120 2026 0646", "01103041420646", "01103041423313 334445361605", "36300444",
        "400002324345361605", "30100105163645443303", "004026", "103041423313020110 133344453616050413",
        "16364541301001021343",
    };
    static const struct { char c; const char *s; } other[] = {
        {'-', "1333"}, {'=', "0242 0444"}, {'[', "30101636"}, {']', "10303616"}, {'\\', "0046"}, {'/', "0640"},
        {';', "2222 2516"}, {',', "2516"}, {'.', "2626"}, {'\'', "2022"}, {'`', "1021"},
        {1, "2026 022042"}, {2, "2026 042646"}, {3, "0343 210325"}, {4, "0343 214325"},
    };
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    for (size_t i = 0; i < sizeof other / sizeof *other; i++)
        if (other[i].c == c) return other[i].s;
    return "0110304142332324 2626";   /* ? */
}

#define FONT_W 4.0f
#define FONT_H 6.0f
#define FONT_GAP 1.6f

/* ---- Canvas: premultiplied float RGBA, shapes as signed distances in pixels ---- */

typedef struct { int w, h; float *px; } Canvas;
typedef struct { float ax, ay, bx, by; } Seg;
typedef struct {
    int kind;                 /* 0 rounded rect, 1 segments (union) */
    float cx, cy, hw, hh, r;  /* rect: centre, half size, corner radius; segments: radius r */
    const Seg *seg;
    int nseg;
} Shape;

static float sd_rrect(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(x - cx) - (hw - r), qy = fabsf(y - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float sd_seg(float x, float y, const Seg *s) {
    float px = x - s->ax, py = y - s->ay, dx = s->bx - s->ax, dy = s->by - s->ay;
    float len2 = dx * dx + dy * dy, t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    px -= t * dx;
    py -= t * dy;
    return sqrtf(px * px + py * py);
}

static float sd_shape(const Shape *s, float x, float y) {
    if (s->kind == 0) return sd_rrect(x, y, s->cx, s->cy, s->hw, s->hh, s->r);
    float d = 1e9f;
    for (int i = 0; i < s->nseg; i++) {
        float e = sd_seg(x, y, &s->seg[i]);
        if (e < d) d = e;
    }
    return d - s->r;
}

static Shape rrect(float x0, float y0, float x1, float y1, float r) {
    Shape s = {0, (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2, 0, NULL, 0};
    float m = s.hw < s.hh ? s.hw : s.hh;
    s.r = r < m ? r : m;
    return s;
}

/* Paints color (0xAARRGGBB) where shape (intersected with clip when given) covers the pixel. */
static void fill(Canvas *c, const Shape *shape, const Shape *clip, uint32_t color) {
    float a = (color >> 24) / 255.0f, r = (color >> 16 & 0xff) / 255.0f * a, g = (color >> 8 & 0xff) / 255.0f * a,
          b = (color & 0xff) / 255.0f * a;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            float px = x + 0.5f, py = y + 0.5f, d = sd_shape(shape, px, py);
            if (clip) {
                float e = sd_shape(clip, px, py);
                if (e > d) d = e;
            }
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            float *p = c->px + 4 * (y * c->w + x), k = 1 - cov * a;
            p[0] = p[0] * k + r * cov;
            p[1] = p[1] * k + g * cov;
            p[2] = p[2] * k + b * cov;
            p[3] = p[3] * k + a * cov;
        }
}

/* Text centred at (cx, cy) with u pixels per font unit. */
static void text(Canvas *c, const char *s, float cx, float cy, float u, uint32_t color) {
    Seg segs[160];
    int n = 0, len = (int)strlen(s);
    float width = len * FONT_W + (len - 1) * FONT_GAP, x0 = cx - width * u / 2, y0 = cy - FONT_H * u / 2;
    for (int i = 0; i < len; i++, x0 += (FONT_W + FONT_GAP) * u)
        for (const char *g = glyph(s[i]); *g;) {
            if (*g == ' ') { g++; continue; }
            float px = x0 + (g[0] - '0') * u, py = y0 + (g[1] - '0') * u;
            int first = 1;
            for (g += 2; g[0] && g[0] != ' ' && n < 160; g += 2, first = 0) {
                float qx = x0 + (g[0] - '0') * u, qy = y0 + (g[1] - '0') * u;
                segs[n++] = (Seg){px, py, qx, qy};
                px = qx;
                py = qy;
            }
            if (first && n < 160) segs[n++] = (Seg){px, py, px, py};   /* a dot */
        }
    float r = 0.55f * u;
    Shape shape = {1, 0, 0, 0, 0, r < 0.6f ? 0.6f : r, segs, n};
    fill(c, &shape, NULL, color);
}

/* Symbol sym's strokes in the box x0, y0 (top left), w x h, lines of radius r. */
static void symbol(Canvas *c, int sym, float x0, float y0, float w, float h, float r, uint32_t color) {
    Seg segs[24];
    int n = 0;
    for (const char *g = symbol_strokes(sym); *g;) {
        if (*g == ' ') { g++; continue; }
        float px = x0 + (g[0] - '0') * w / 8, py = y0 + (g[1] - '0') * h / 8;
        for (g += 2; g[0] && g[0] != ' ' && n < 24; g += 2) {
            float qx = x0 + (g[0] - '0') * w / 8, qy = y0 + (g[1] - '0') * h / 8;
            segs[n++] = (Seg){px, py, qx, qy};
            px = qx;
            py = qy;
        }
    }
    Shape shape = {1, 0, 0, 0, 0, r < 0.6f ? 0.6f : r, segs, n};
    fill(c, &shape, NULL, color);
}

#define INK 0xfff5f5f5u
#define FACE 0xff181818u

/* Font unit that fits a label of len characters into w x h pixels. */
static float fit_unit(int len, float w, float h) {
    float uw = w / (len * FONT_W + (len - 1) * FONT_GAP + 1.1f), uh = h / (FONT_H + 1.1f);
    return uw < uh ? uw : uh;
}

static void draw_key(Canvas *c, int key, float cx, float cy, float aw, float ah) {
    int sym = kbm_key_symbols ? kbm_key_symbol(key) : KBM_SYM_NONE;
    const char *label = kbm_key_label(key, 0);
    if (!label) label = "?";
    float side = aw < ah ? aw : ah, capw = sym == KBM_SYM_SPACE || (!sym && strlen(label) > 2) ? aw : side, caph = side;
    float rim = side * 0.075f < 1 ? 1 : side * 0.075f, rad = 0.22f * side;
    float x0 = cx - capw / 2, y0 = cy - caph / 2, x1 = cx + capw / 2, y1 = cy + caph / 2;
    Shape outer = rrect(x0, y0, x1, y1, rad), face = rrect(x0 + rim, y0 + rim, x1 - rim, y1 - 2 * rim, rad - rim);
    fill(c, &outer, NULL, INK);
    fill(c, &face, NULL, FACE);
    float fcy = (y0 + rim + y1 - 2 * rim) / 2;
    if (sym) {   /* the symbol in a square (Space: a wide box) inside the face */
        float s = 0.56f * side, w = sym == KBM_SYM_SPACE ? 0.62f * capw : s, r = 0.05f * s;
        symbol(c, sym, cx - w / 2, fcy - s / 2, w, s, r, INK);
        return;
    }
    float lw = capw - 2 * rim - 0.16f * side, lh = (strlen(label) > 1 ? 0.42f : 0.52f) * side;
    float u = fit_unit((int)strlen(label), lw, lh);
    if (u * FONT_H < 7) {   /* too small to read: the short form */
        label = kbm_key_label(key, 1);
        if (!label) label = "?";
        u = fit_unit((int)strlen(label), lw, lh);
    }
    text(c, label, cx, fcy, u, INK);
}

static void draw_mouse(Canvas *c, int key, float cx, float cy, float aw, float ah) {
    float hgt = ah * 0.98f, wid = hgt * 0.66f;
    if (wid > aw) {
        wid = aw;
        hgt = wid / 0.66f;
    }
    float t = wid * 0.085f < 1 ? 1 : wid * 0.085f;
    float x0 = cx - wid / 2, y0 = cy - hgt / 2, x1 = cx + wid / 2, y1 = cy + hgt / 2, split = y0 + 0.44f * hgt;
    Shape body = rrect(x0, y0, x1, y1, 0.48f * wid), inner = rrect(x0 + t, y0 + t, x1 - t, y1 - t, 0.48f * wid - t);
    fill(c, &body, NULL, INK);
    fill(c, &inner, NULL, FACE);
    if (key == INPUT_MOUSE_LEFT || key == INPUT_MOUSE_RIGHT) {
        Shape half = key == INPUT_MOUSE_LEFT ? rrect(x0 - 2, y0 - 2, cx, split, 0) : rrect(cx, y0 - 2, x1 + 2, split, 0);
        fill(c, &inner, &half, KBM_RED);
    }
    Seg lines[2] = {{cx, y0, cx, split}, {x0, split, x1, split}};
    Shape div = {1, 0, 0, 0, 0, t / 2, lines, 2};
    fill(c, &div, &body, INK);
    /* Wheel: a dark gap, then the wheel (red when it is the bound button). */
    Seg wheel = {cx, y0 + 0.13f * hgt, cx, y0 + 0.29f * hgt};
    Shape gap = {1, 0, 0, 0, 0, 0.10f * wid + t * 0.6f, &wheel, 1}, w = {1, 0, 0, 0, 0, 0.10f * wid, &wheel, 1};
    fill(c, &gap, NULL, FACE);
    fill(c, &w, NULL, key == INPUT_MOUSE_MIDDLE ? KBM_RED : INK);
    if (key == INPUT_MOUSE_X1 || key == INPUT_MOUSE_X2) {   /* side buttons on the left flank */
        float sx = x0 + t * 1.9f, r = 0.07f * wid < 0.8f ? 0.8f : 0.07f * wid;
        Seg x2 = {sx, split + 0.07f * hgt, sx, split + 0.17f * hgt}, x1s = {sx, split + 0.24f * hgt, sx, split + 0.34f * hgt};
        Shape b2 = {1, 0, 0, 0, 0, r, &x2, 1}, b1 = {1, 0, 0, 0, 0, r, &x1s, 1};
        fill(c, &b2, NULL, key == INPUT_MOUSE_X2 ? KBM_RED : INK);
        fill(c, &b1, NULL, key == INPUT_MOUSE_X1 ? KBM_RED : INK);
    }
}

static int is_mouse(int key) { return key >= INPUT_MOUSE_LEFT && key <= INPUT_MOUSE_X2; }

/* Icon of key into canvas c, in the rows top..bottom (pixels). */
static void draw_into(Canvas *c, int key, float top, float bottom) {
    float margin = 0.04f * c->w < 1 ? 1 : 0.04f * c->w;
    float aw = c->w - 2 * margin, ah = bottom - top - margin, cx = c->w / 2.0f, cy = (top + bottom) / 2;
    if (is_mouse(key)) draw_mouse(c, key, cx, cy, aw, ah);
    else draw_key(c, key, cx, cy, aw, ah);
}

/* Canvas -> B8G8R8A8 (straight alpha). */
static void store(const Canvas *c, uint32_t *dst, int stride) {
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            const float *p = c->px + 4 * (y * c->w + x);
            uint32_t a = (uint32_t)(p[3] * 255 + 0.5f), o = 0;
            if (a) {
                float k = 255 / p[3];
                uint32_t r = (uint32_t)(p[0] * k + 0.5f), g = (uint32_t)(p[1] * k + 0.5f), b = (uint32_t)(p[2] * k + 0.5f);
                o = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
            dst[y * stride + x] = o;
        }
}

void kbm_draw_icon(uint32_t *bgra, int w, int h, int stride, int key, float band_top, float band_bottom) {
    Canvas c = {w, h, calloc((size_t)w * h * 4, sizeof(float))};
    if (!c.px) return;
    draw_into(&c, key, band_top * h, band_bottom * h);
    store(&c, bgra, stride);
    free(c.px);
}

/* ---- Atlas ---- */

typedef struct { int x0, y0, x1, y1; } Box;

static Box cell_box(const KbmRect *r, uint32_t w, uint32_t h) {
    Box b = {(int)floorf(r->u0 * w + 1e-4f), (int)floorf(r->v0 * h + 1e-4f), (int)ceilf(r->u1 * w - 1e-4f),
             (int)ceilf(r->v1 * h - 1e-4f)};
    if (b.x1 > (int)w) b.x1 = (int)w;
    if (b.y1 > (int)h) b.y1 = (int)h;
    return b;
}

/* Rows (fractions of the cell height) with base texels of alpha > 128; 0 when there are none. */
static int opaque_rows(const uint32_t *a, uint32_t w, Box b, float *top, float *bottom) {
    int t = -1, e = -1;
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++)
            if ((a[(size_t)y * w + x] >> 24) > 128) {
                if (t < 0) t = y;
                e = y + 1;
                break;
            }
    if (t < 0) return 0;
    *top = (float)(t - b.y0) / (b.y1 - b.y0);
    *bottom = (float)(e - b.y0) / (b.y1 - b.y0);
    return 1;
}

int kbm_paint_atlas(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const int *keys) {
    /* The band the face-button glyphs occupy, measured before anything is repainted. */
    float band_t = 1, band_b = 0, t, b;
    for (int i = 0; i < 4; i++)
        if (cells[i].record < nrecs && opaque_rows(bgra, w, cell_box(&recs[cells[i].record], w, h), &t, &b)) {
            if (t < band_t) band_t = t;
            if (b > band_b) band_b = b;
        }
    if (band_b <= band_t) band_t = 0, band_b = 1;
    float sprite_t[NCELLS], sprite_b[NCELLS];
    for (int i = FIRST_SPRITE; i < NCELLS; i++)
        if (cells[i].record >= nrecs || !opaque_rows(bgra, w, cell_box(&recs[cells[i].record], w, h), &sprite_t[i], &sprite_b[i]))
            sprite_t[i] = 0, sprite_b[i] = 1;
    int painted = 0;
    for (int i = 0; i < NCELLS; i++) {
        int key = keys[cells[i].control];
        if (key < 0 || cells[i].record >= nrecs) continue;
        Box bx = cell_box(&recs[cells[i].record], w, h);
        if (bx.x1 <= bx.x0 || bx.y1 <= bx.y0) continue;
        int sprite = i >= FIRST_SPRITE;
        kbm_draw_icon(bgra + (size_t)bx.y0 * w + bx.x0, bx.x1 - bx.x0, bx.y1 - bx.y0, (int)w, key,
                      sprite ? sprite_t[i] : band_t, sprite ? sprite_b[i] : band_b);
        painted++;
    }
    return painted;
}
