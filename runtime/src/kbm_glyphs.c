/* Keyboard and mouse button icons for the game's font/icon atlas (see kbm_glyphs.h). */
#include "kbm_glyphs.h"
#include "input_map.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- The glyph rect table ---- */

#define TABLE_OFFSET 0x861f4u       /* first record in the known global.pak */
#define BIG_TABLE_OFFSET 0x1728c4u  /* the big font's first record there */
#define BIG_DPAD_RECORDS 5          /* the neutral D-pad and the four directions */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Records of a table whose 16-byte header {count, base, 0, 0} ends at rec_off, or -1 when it does
 * not fit (another base, fewer than min_count records). */
static int table_at(const uint8_t *d, size_t n, size_t rec_off, uint32_t base, uint32_t min_count, KbmRect *out, int max) {
    if (rec_off < 16 || rec_off > n) return -1;
    const uint8_t *h = d + rec_off - 16;
    uint32_t count = rd32(h);
    if (rd32(h + 4) != base || rd32(h + 8) || rd32(h + 12) || count < min_count || count > KBM_RECTS_MAX ||
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

/* The table {count, base, 0, 0} with at least min_count records: at the known offset, else the
 * first one a scan of 4-aligned offsets finds. */
static int table_parse(const uint8_t *d, size_t n, size_t known, uint32_t base, uint32_t min_count, KbmRect *out, int max) {
    int c = table_at(d, n, known, base, min_count, out, max);
    if (c > 0) return c;
    for (size_t pos = 4; pos + 12 <= n; pos += 4)   /* {base, 0, 0} at a 4-aligned offset */
        if (rd32(d + pos) == base && !rd32(d + pos + 4) && !rd32(d + pos + 8) &&
            (c = table_at(d, n, pos + 12, base, min_count, out, max)) > 0)
            return c;
    return -1;
}

int kbm_rects_parse(const uint8_t *d, size_t n, KbmRect *out, int max) {
    return table_parse(d, n, TABLE_OFFSET, KBM_RECT_BASE, 0x100, out, max);
}

int kbm_big_rects_parse(const uint8_t *d, size_t n, KbmRect *out, int max) {
    return table_parse(d, n, BIG_TABLE_OFFSET, 0, KBM_BIG_DPAD_BASE + 1 + BIG_DPAD_RECORDS, out, max);
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

/* paks\global.pak of a game folder (XBoxWad.idx -> its WAD), or NULL. */
static uint8_t *global_pak(const char *disc, size_t *size_out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/XBoxWad.idx", disc);
    long n = file_size(path);
    if (n < 20 || n > (4l << 20)) return NULL;
    uint8_t *idx = read_range(path, 0, (size_t)n);
    if (!idx) return NULL;
    uint32_t wads = rd32(idx), files = rd32(idx + 4), wad_table = rd32(idx + 12), file_table = rd32(idx + 16);
    uint8_t *pak = NULL;
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
            pak = read_range(path, (long)(loc & 0xffffff) << 10, size);
            *size_out = size;
            break;
        }
    }
    free(idx);
    return pak;
}

int kbm_rects_load(const char *disc, KbmRect *out, int max) {
    size_t n = 0;
    uint8_t *pak = global_pak(disc, &n);
    int c = pak ? kbm_rects_parse(pak, n, out, max) : -1;
    free(pak);
    return c;
}

int kbm_big_rects_load(const char *disc, KbmRect *out, int max) {
    size_t n = 0;
    uint8_t *pak = global_pak(disc, &n);
    int c = pak ? kbm_big_rects_parse(pak, n, out, max) : -1;
    free(pak);
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

/* The big font's D-pad sprites (kbm_glyphs.h): record = code - 1 + KBM_BIG_DPAD_BASE. */
static const KbmCell big_cells[BIG_DPAD_RECORDS] = {
    {KBM_BIG_DPAD_BASE + 1, -1, "D-pad (neutral)"}, {KBM_BIG_DPAD_BASE + 2, IN_DOWN, "D-pad down (<SDD>)"},
    {KBM_BIG_DPAD_BASE + 3, IN_LEFT, "D-pad left (<SDL>, Revive)"}, {KBM_BIG_DPAD_BASE + 4, IN_RIGHT, "D-pad right (<SDR>)"},
    {KBM_BIG_DPAD_BASE + 5, IN_UP, "D-pad up (<SDU>)"},
};
int kbm_big_cell_count(void) { return BIG_DPAD_RECORDS; }
const KbmCell *kbm_big_cell(int i) { return i >= 0 && i < BIG_DPAD_RECORDS ? &big_cells[i] : NULL; }

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
        {1, "2026 022042"}, {2, "2026 042644"}, {3, "0343 210325"}, {4, "0343 214325"},
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
#define FACE 0xff2c2c2cu   /* key body: a little lighter than black, so it reads without a border */

/* The key's lower lip (its depth): the face colour darkened, never a light outline. */
static uint32_t lip_of(uint32_t face) {
    uint32_t r = (face >> 16 & 0xff) * 9 / 20, g = (face >> 8 & 0xff) * 9 / 20, b = (face & 0xff) * 9 / 20;
    return (face & 0xff000000u) | r << 16 | g << 8 | b;
}

/* Font unit that fits a label of len characters into w x h pixels. */
static float fit_unit(int len, float w, float h) {
    float uw = w / (len * FONT_W + (len - 1) * FONT_GAP + 1.1f), uh = h / (FONT_H + 1.1f);
    return uw < uh ? uw : uh;
}

/* A key cap with the face colour face; key < 0: an empty cap. */
static void draw_key(Canvas *c, int key, float cx, float cy, float aw, float ah, uint32_t face_color) {
    int sym = kbm_key_symbols && key >= 0 ? kbm_key_symbol(key) : KBM_SYM_NONE;
    const char *label = key >= 0 ? kbm_key_label(key, 0) : "";
    if (!label) label = "?";
    float side = aw < ah ? aw : ah, capw = sym == KBM_SYM_SPACE || (!sym && strlen(label) > 2) ? aw : side, caph = side;
    float rim = side * 0.075f < 1 ? 1 : side * 0.075f, rad = 0.22f * side;
    float x0 = cx - capw / 2, y0 = cy - caph / 2, x1 = cx + capw / 2, y1 = cy + caph / 2;
    /* No white border: the body in the face colour over a darker lower lip (the key's depth). */
    Shape outer = rrect(x0, y0, x1, y1, rad), face = rrect(x0, y0, x1, y1 - rim, rad);
    fill(c, &outer, NULL, lip_of(face_color));
    fill(c, &face, NULL, face_color);
    if (!*label) return;
    float fcy = (y0 + y1 - rim) / 2;
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

/* A mouse with the body colour face and no outline, its bound button red (white on a red body);
 * the button seams are dark grooves. */
static void draw_mouse(Canvas *c, int key, float cx, float cy, float aw, float ah, uint32_t face) {
    uint32_t mark = face == KBM_RED ? INK : KBM_RED;
    float hgt = ah * 0.98f, wid = hgt * 0.66f;
    if (wid > aw) {
        wid = aw;
        hgt = wid / 0.66f;
    }
    float t = wid * 0.085f < 1 ? 1 : wid * 0.085f;
    float x0 = cx - wid / 2, y0 = cy - hgt / 2, x1 = cx + wid / 2, y1 = cy + hgt / 2, split = y0 + 0.44f * hgt;
    Shape body = rrect(x0, y0, x1, y1, 0.48f * wid);
    fill(c, &body, NULL, face);
    if (key == INPUT_MOUSE_LEFT || key == INPUT_MOUSE_RIGHT) {
        Shape half = key == INPUT_MOUSE_LEFT ? rrect(x0 - 2, y0 - 2, cx, split, 0) : rrect(cx, y0 - 2, x1 + 2, split, 0);
        fill(c, &body, &half, mark);
    }
    Seg lines[2] = {{cx, y0, cx, split}, {x0, split, x1, split}};
    Shape div = {1, 0, 0, 0, 0, t / 2, lines, 2};
    fill(c, &div, &body, lip_of(face));
    /* Wheel: a dark groove, then the wheel (red when it is the bound button). */
    Seg wheel = {cx, y0 + 0.13f * hgt, cx, y0 + 0.29f * hgt};
    Shape gap = {1, 0, 0, 0, 0, 0.10f * wid + t * 0.6f, &wheel, 1}, w = {1, 0, 0, 0, 0, 0.10f * wid, &wheel, 1};
    fill(c, &gap, NULL, lip_of(face));
    fill(c, &w, NULL, key == INPUT_MOUSE_MIDDLE || key == INPUT_MOUSE_WHEEL ? mark : INK);
    if (key == INPUT_MOUSE_X1 || key == INPUT_MOUSE_X2) {   /* side buttons on the left flank */
        float sx = x0 + t * 1.9f, r = 0.07f * wid < 0.8f ? 0.8f : 0.07f * wid;
        Seg x2 = {sx, split + 0.07f * hgt, sx, split + 0.17f * hgt}, x1s = {sx, split + 0.24f * hgt, sx, split + 0.34f * hgt};
        Shape b2 = {1, 0, 0, 0, 0, r, &x2, 1}, b1 = {1, 0, 0, 0, 0, r, &x1s, 1};
        fill(c, &b2, NULL, key == INPUT_MOUSE_X2 ? mark : INK);
        fill(c, &b1, NULL, key == INPUT_MOUSE_X1 ? mark : INK);
    }
}

static int is_mouse(int key) { return key >= INPUT_MOUSE_LEFT && key <= INPUT_MOUSE_WHEEL; }

/* Icon of key into canvas c, in the rows top..bottom (pixels). */
static void draw_into(Canvas *c, int key, float top, float bottom) {
    float margin = 0.04f * c->w < 1 ? 1 : 0.04f * c->w;
    float aw = c->w - 2 * margin, ah = bottom - top - margin, cx = c->w / 2.0f, cy = (top + bottom) / 2;
    if (is_mouse(key)) draw_mouse(c, key, cx, cy, aw, ah, FACE);
    else draw_key(c, key, cx, cy, aw, ah, FACE);
}

/* The keys bound to the D-pad in an inverted T (up above left, down, right), in the rows
 * top..bottom; lit (an IN_* direction, or -1) gets a red face. */
static void draw_dpad_keys(Canvas *c, const int *keys, int lit, float top, float bottom) {
    static const int dirs[4] = {IN_UP, IN_LEFT, IN_DOWN, IN_RIGHT};
    static const float at[4][2] = {{0, -0.5f}, {-1, 0.5f}, {0, 0.5f}, {1, 0.5f}};   /* in caps from the centre */
    float margin = 0.04f * c->w < 1 ? 1 : 0.04f * c->w;
    float aw = c->w - 2 * margin, ah = bottom - top - margin, side = aw / 3 < ah / 2 ? aw / 3 : ah / 2;
    float cx = c->w / 2.0f, cy = (top + bottom) / 2, cap = 0.94f * side;
    for (int i = 0; i < 4; i++) {
        int key = keys[dirs[i]];
        uint32_t face = dirs[i] == lit ? KBM_RED : FACE;
        float x = cx + at[i][0] * side, y = cy + at[i][1] * side;
        if (is_mouse(key)) draw_mouse(c, key, x, y, cap, cap, face);
        else draw_key(c, key, x, y, cap, cap, face);
    }
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

int kbm_paint_big_atlas(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const int *keys) {
    if (nrecs <= big_cells[BIG_DPAD_RECORDS - 1].record) return 0;
    int any = keys[IN_UP] >= 0 || keys[IN_LEFT] >= 0 || keys[IN_DOWN] >= 0 || keys[IN_RIGHT] >= 0, painted = 0;
    for (int i = 0; i < BIG_DPAD_RECORDS; i++) {
        int ctl = big_cells[i].control;
        if (ctl < 0 ? !any : keys[ctl] < 0) continue;
        Box bx = cell_box(&recs[big_cells[i].record], w, h);
        if (bx.x1 <= bx.x0 || bx.y1 <= bx.y0) continue;
        float t = 0, b = 1;
        if (!opaque_rows(bgra, w, bx, &t, &b)) t = 0, b = 1;
        int cw = bx.x1 - bx.x0, ch = bx.y1 - bx.y0;
        Canvas c = {cw, ch, calloc((size_t)cw * ch * 4, sizeof(float))};
        if (!c.px) continue;
        draw_dpad_keys(&c, keys, ctl, t * ch, b * ch);
        store(&c, bgra + (size_t)bx.y0 * w + bx.x0, (int)w);
        free(c.px);
        painted++;
    }
    return painted;
}

int kbm_atlas_base(int pack_image, int pack_replaces, int sharp_fonts, int sharp_state) {
    if (pack_image) return KBM_BASE_PACK;
    if (pack_replaces || !sharp_fonts || sharp_state == 4) return KBM_BASE_GAME;
    return sharp_state == 0 || sharp_state == 1 ? KBM_BASE_WAIT : KBM_BASE_SHARP;
}

/* ---- Controller remap: cells repainted from the base's own cells ---- */

/* Source box s of snapshot src into box d of dst, scaled uniformly to fit (bilinear on
 * premultiplied alpha) and centred; the rest of d is cleared. Equal sizes copy exactly. */
static void copy_cell(uint32_t *dst, const uint32_t *src, uint32_t w, Box s, Box d) {
    int sw = s.x1 - s.x0, sh = s.y1 - s.y0, dw = d.x1 - d.x0, dh = d.y1 - d.y0;
    if (sw == dw && sh == dh) {
        for (int y = 0; y < dh; y++)
            memcpy(dst + (size_t)(d.y0 + y) * w + d.x0, src + (size_t)(s.y0 + y) * w + s.x0, (size_t)dw * 4);
        return;
    }
    float k = fminf((float)dw / sw, (float)dh / sh);
    float ow = sw * k, oh = sh * k, ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            uint32_t *o = dst + (size_t)(d.y0 + y) * w + d.x0 + x;
            float fx = (x + 0.5f - ox) / k - 0.5f, fy = (y + 0.5f - oy) / k - 0.5f;
            if (x + 0.5f < ox || x + 0.5f > ox + ow || y + 0.5f < oy || y + 0.5f > oy + oh) { *o = 0; continue; }
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float tx = fx - x0, ty = fy - y0, acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 4; j++) {
                int sx = x0 + (j & 1), sy = y0 + (j >> 1);
                sx = sx < 0 ? 0 : sx >= sw ? sw - 1 : sx;
                sy = sy < 0 ? 0 : sy >= sh ? sh - 1 : sy;
                uint32_t p = src[(size_t)(s.y0 + sy) * w + s.x0 + sx];
                float wt = ((j & 1) ? tx : 1 - tx) * ((j >> 1) ? ty : 1 - ty), a = (p >> 24) / 255.0f;
                acc[0] += wt * a;
                acc[1] += wt * a * (p >> 16 & 0xff);
                acc[2] += wt * a * (p >> 8 & 0xff);
                acc[3] += wt * a * (p & 0xff);
            }
            if (acc[0] <= 0) { *o = 0; continue; }
            uint32_t a8 = (uint32_t)lrintf(acc[0] * 255);
            *o = a8 << 24 | (uint32_t)lrintf(acc[1] / acc[0]) << 16 | (uint32_t)lrintf(acc[2] / acc[0]) << 8 | (uint32_t)lrintf(acc[3] / acc[0]);
        }
}

/* Cell i of table (n cells, the first `first_sprite` of one kind) shows the art of control
 * shown[control]: the first cell of that control of the same kind, else of any kind. */
static int remap_cells(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const KbmCell *table, int n,
                       int first_sprite, const int *shown) {
    int any = 0;
    for (int i = 0; i < n; i++) {
        int c = table[i].control;
        any |= c >= 0 && shown[c] >= 0 && shown[c] != c;
    }
    if (!any) return 0;
    uint32_t *snap = malloc((size_t)w * h * 4);
    if (!snap) return 0;
    memcpy(snap, bgra, (size_t)w * h * 4);
    int painted = 0;
    for (int i = 0; i < n; i++) {
        int c = table[i].control;
        if (c < 0 || shown[c] < 0 || shown[c] == c || table[i].record >= nrecs) continue;
        int src = -1;
        for (int pass = 0; pass < 2 && src < 0; pass++)
            for (int j = 0; j < n && src < 0; j++)
                if (table[j].control == shown[c] && table[j].record < nrecs && (pass || (j >= first_sprite) == (i >= first_sprite))) src = j;
        if (src < 0) continue;
        Box d = cell_box(&recs[table[i].record], w, h), sb = cell_box(&recs[table[src].record], w, h);
        if (d.x1 <= d.x0 || d.y1 <= d.y0 || sb.x1 <= sb.x0 || sb.y1 <= sb.y0) continue;
        copy_cell(bgra, snap, w, sb, d);
        painted++;
    }
    free(snap);
    return painted;
}

int kbm_remap_atlas(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const int *shown) {
    return remap_cells(bgra, w, h, recs, nrecs, cells, NCELLS, FIRST_SPRITE, shown);
}
int kbm_remap_big_atlas(uint32_t *bgra, uint32_t w, uint32_t h, const KbmRect *recs, int nrecs, const int *shown) {
    if (nrecs <= big_cells[BIG_DPAD_RECORDS - 1].record) return 0;
    return remap_cells(bgra, w, h, recs, nrecs, big_cells, BIG_DPAD_RECORDS, BIG_DPAD_RECORDS, shown);
}
