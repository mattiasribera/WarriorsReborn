/* Mod menu: state machine, fades and layout (src/modmenu.h). */
#include "modmenu.h"
#include <math.h>
#include <string.h>

/* Copies at most cap - 1 bytes of s up to stop (or its end), never splitting a UTF-8 character. */
static void copy_line(char *dst, size_t cap, const char *s, const char *stop) {
    size_t n = stop ? (size_t)(stop - s) : strlen(s);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;   /* s[n] continues a character: cut before it starts */
    }
    memcpy(dst, s, n);
    dst[n] = 0;
}

int modmenu_rows(const ModMenu *m) { return m->values ? MODMENU_VALUE_ROWS : MODMENU_ROWS; }

static void keep_visible(ModMenu *m) {
    const int rows = modmenu_rows(m);
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + rows) m->top = m->sel - rows + 1;
    if (m->top > m->n - rows) m->top = m->n - rows;
    if (m->top < 0) m->top = 0;
}

/* Splits text into at most cap lines of dst (one per '\n'); returns the count. */
static int split_lines(char (*dst)[MODMENU_TEXT_MAX], size_t width, int cap, const char *text) {
    int n = 0;
    for (const char *p = text ? text : ""; *p && n < cap;) {
        const char *e = strchr(p, '\n');
        copy_line(dst[n++], width, p, e);
        if (!e) break;
        p = e + 1;
    }
    return n;
}

int modmenu_open(ModMenu *m, const char *title, const char *items, const char *footer, int sel, double now_ms) {
    memset(m, 0, sizeof *m);
    copy_line(m->title, sizeof m->title, title ? title : "", NULL);
    copy_line(m->footer, sizeof m->footer, footer ? footer : "", NULL);
    m->n = split_lines(m->item, MODMENU_TEXT_MAX, MODMENU_ITEMS_MAX, items);
    if (!m->n) return 0;
    m->sel = sel < 0 ? 0 : sel >= m->n ? m->n - 1 : sel;
    keep_visible(m);
    m->open = 1;
    m->held = MM_UP | MM_DOWN | MM_LEFT | MM_RIGHT | MM_OK | MM_BACK;   /* nothing held now counts as a press */
    m->moved_ms = now_ms;
    m->repeat_ms = now_ms + MODMENU_REPEAT_DELAY_MS;
    m->hovered = -1;
    return m->n;
}

void modmenu_close(ModMenu *m) { m->open = m->confirm_open = 0; }

void modmenu_values(ModMenu *m, int on) {
    m->values = on != 0;
    keep_visible(m);
}

void modmenu_tabs(ModMenu *m, const char *tabs, int tab) {
    static char names[MODMENU_TABS_MAX][MODMENU_TEXT_MAX];
    m->ntabs = split_lines(names, MODMENU_VALUE_MAX, MODMENU_TABS_MAX, tabs);
    for (int i = 0; i < m->ntabs; i++) memcpy(m->tabs[i], names[i], MODMENU_VALUE_MAX);
    m->tab = tab < 0 ? 0 : tab >= m->ntabs ? (m->ntabs ? m->ntabs - 1 : 0) : tab;
}

int modmenu_set_items(ModMenu *m, const char *items, int sel) {
    static char lines[MODMENU_ITEMS_MAX][MODMENU_TEXT_MAX];
    const int n = split_lines(lines, MODMENU_TEXT_MAX, MODMENU_ITEMS_MAX, items);
    if (!n) return 0;
    memcpy(m->item, lines, sizeof lines);
    m->n = n;
    memset(m->value, 0, sizeof m->value);
    memset(m->badge, 0, sizeof m->badge);
    memset(m->grey, 0, sizeof m->grey);
    m->sel = sel < 0 ? 0 : sel >= n ? n - 1 : sel;
    m->top = 0;
    keep_visible(m);
    m->hovered = -1;
    return n;
}

void modmenu_confirm(ModMenu *m, const char *text, const char *yes, const char *no) {
    copy_line(m->confirm, sizeof m->confirm, text ? text : "", NULL);
    copy_line(m->confirm_yes, sizeof m->confirm_yes, yes ? yes : "", NULL);
    copy_line(m->confirm_no, sizeof m->confirm_no, no ? no : "", NULL);
    m->confirm_open = 1;
    m->confirm_sel = 0;
}

static int move(ModMenu *m, unsigned dir) {
    int s = m->sel;
    if (dir & MM_UP) s = s > 0 ? s - 1 : m->n - 1;
    else if (dir & MM_DOWN) s = s < m->n - 1 ? s + 1 : 0;
    else if (m->values) return 0;   /* left / right adjust a value menu's row (modmenu_step) */
    else if (dir & MM_LEFT) s = s - MODMENU_ROWS < 0 ? 0 : s - MODMENU_ROWS;
    else if (dir & MM_RIGHT) s = s + MODMENU_ROWS > m->n - 1 ? m->n - 1 : s + MODMENU_ROWS;
    if (s == m->sel) return 0;
    m->sel = s;
    keep_visible(m);
    return 1;
}

int modmenu_step(ModMenu *m, unsigned c, double now_ms, int *index) {
    if (!m->open) return MM_EV_NONE;
    const unsigned pressed = c & ~m->held, dirs = MM_UP | MM_DOWN | MM_LEFT | MM_RIGHT;
    if (!(c & (MM_OK | MM_BACK))) m->armed = 1;
    if (m->confirm_open) {   /* the dialog takes every control until answered */
        m->held = c;
        if (pressed & dirs) m->confirm_sel ^= 1;
        if (m->armed && (pressed & (MM_OK | MM_BACK))) {
            m->confirm_open = 0;
            *index = (pressed & MM_OK) && m->confirm_sel == 0;
            m->repeat_ms = now_ms + MODMENU_REPEAT_DELAY_MS;
            return MM_EV_CONFIRM;
        }
        return MM_EV_NONE;
    }
    if (m->armed && (pressed & (MM_OK | MM_BACK))) {
        m->held = c;
        *index = m->sel;
        if (pressed & MM_BACK) {
            m->open = 0;
            return MM_EV_CANCEL;
        }
        if (!m->values) {
            m->open = 0;
            return MM_EV_CHOOSE;
        }
        return m->grey[m->sel] ? MM_EV_NONE : MM_EV_CHOOSE;
    }
    if (m->ntabs > 1 && (pressed & (MM_TAB_PREV | MM_TAB_NEXT))) {
        m->held = c;
        m->tab = (pressed & MM_TAB_NEXT) ? (m->tab + 1) % m->ntabs : (m->tab + m->ntabs - 1) % m->ntabs;
        *index = m->tab;
        return MM_EV_TAB;
    }
    unsigned dir = 0;
    if (pressed & dirs) {
        dir = pressed & dirs;
        m->repeat_ms = now_ms + MODMENU_REPEAT_DELAY_MS;
    } else if ((c & dirs & m->held) && now_ms >= m->repeat_ms) {
        dir = c & dirs;
        m->repeat_ms = now_ms + MODMENU_REPEAT_MS;
    }
    m->held = c;
    if (m->values && (dir & (MM_LEFT | MM_RIGHT)) && !(dir & (MM_UP | MM_DOWN))) {
        if (m->grey[m->sel]) return MM_EV_NONE;
        m->adjust = dir & MM_RIGHT ? 1 : -1;
        *index = m->sel;
        return MM_EV_ADJUST;
    }
    if (dir && move(m, dir)) m->moved_ms = now_ms;
    if (now_ms - m->moved_ms >= MODMENU_DEBOUNCE_MS && m->sel != m->hovered) {
        m->hovered = m->sel;
        *index = m->sel;
        return MM_EV_HOVER;
    }
    return MM_EV_NONE;
}

unsigned modmenu_controls(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly) {
    unsigned c = 0;
    if ((buttons & 0x1) || ly > 16384) c |= MM_UP;
    if ((buttons & 0x2) || ly < -16384) c |= MM_DOWN;
    if ((buttons & 0x4) || lx < -16384) c |= MM_LEFT;
    if ((buttons & 0x8) || lx > 16384) c |= MM_RIGHT;
    if ((buttons & 0x10) || analog[0] > 0x40 || analog[3] > 0x40) c |= MM_OK;
    if ((buttons & 0x20) || analog[1] > 0x40) c |= MM_BACK;
    return c;
}

/* ---- fades ---- */
void modfade_init(ModFade *f, float level) { *f = (ModFade){level, level, 0, 0}; }

float modfade_level(const ModFade *f, double now_ms) {
    if (f->dur_ms <= 0 || now_ms >= f->t0_ms + f->dur_ms) return f->to;
    double k = (now_ms - f->t0_ms) / f->dur_ms;
    if (k < 0) k = 0;
    return f->from + (f->to - f->from) * (float)k;
}

void modfade_set(ModFade *f, float to, double dur_ms, double now_ms) {
    f->from = modfade_level(f, now_ms);
    f->to = to;
    f->t0_ms = now_ms;
    f->dur_ms = dur_ms;
}

int modfade_done(const ModFade *f, double now_ms) { return f->dur_ms <= 0 || now_ms >= f->t0_ms + f->dur_ms; }

float modfade_gain(float level) {
    if (level <= 0) return 0;
    if (level >= 1) return 1;
    return sinf(level * 1.57079633f);
}

/* ---- layout ---- */
#define RED 0xffc8102eu
#define WHITE 0xffffffffu
#define GREY 0xff9a9a9au

/* Next character code of a text: a UTF-8 2-byte sequence as its Latin-1 code (the font's accented
 * letters), longer sequences as 0, any other byte as itself (the game's own Latin-1 texts, e.g.
 * "m\250sica"). */
static int next_char(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *p += 2; return ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) { *p += 3; return 0; }
    (*p)++;
    return s[0];
}

static const float *glyph(const ModFont *f, int c) {
    if (!f || !f->rect || c == ' ' || c < f->first || c >= f->first + f->n) return NULL;
    const float *r = f->rect[c - f->first];
    return r[2] > r[0] && r[3] > r[1] ? r : NULL;
}

/* Advance of one glyph at height px (square pixels). */
static float advance(const ModFont *f, const float *r, float px) {
    if (!r) return px / 3;
    return (r[2] - r[0]) * (float)f->atlas_w / ((r[3] - r[1]) * (float)f->atlas_h) * px;
}

float modmenu_text_width(const ModFont *f, const char *text, float px) {
    float w = 0;
    for (const char *p = text; *p;) w += advance(f, glyph(f, next_char(&p)), px);
    return w;
}

typedef struct {
    ModQuad *q;
    int n, max;
} Out;

static uint32_t fade_color(uint32_t argb, float a) {   /* the alpha byte scaled by a (0..1) */
    return (uint32_t)((argb >> 24) * a + 0.5f) << 24 | (argb & 0xffffffu);
}
static void put(Out *o, float x0, float y0, float x1, float y1, const float uv[4], uint32_t color, int tex) {
    static const float none[4] = {0, 0, 1, 1};
    if (o->n >= o->max) return;
    if (!uv) uv = none;
    o->q[o->n++] = (ModQuad){x0, y0, x1, y1, uv[0], uv[1], uv[2], uv[3], color, tex};
}

/* Draws text from x at top y, glyph height px (horizontal sizes / aspect). A text that would pass
 * the right edge max_x (0 = no limit) ends with "..." before it. */
static void text(Out *o, const ModFont *f, const char *s, float x, float y, float px, float aspect, uint32_t color, float max_x) {
    const float *dot = glyph(f, '.');
    const float dw = dot ? advance(f, dot, px) / aspect : 0;
    const int cut = max_x > 0 && x + modmenu_text_width(f, s, px) / aspect > max_x;
    for (const char *p = s; *p;) {
        const float *r = glyph(f, next_char(&p));
        const float a = advance(f, r, px) / aspect;
        if (cut && x + a > max_x - 3 * dw) {
            for (int i = 0; dot && i < 3; i++, x += dw) put(o, x, y, x + dw, y + px, dot, color, MM_TEX_FONT);
            return;
        }
        if (r) put(o, x, y, x + a, y + px, r, color, MM_TEX_FONT);
        x += a;
    }
}

#define TAB_SEL 0xff6a0818u
#define DIM 0xff5a5a5au
#define NOTE 0xffb4b4b4u
#define BADGE 0xffe0b050u

/* Word-wraps s into lines no wider than max_w (pixels at px, already divided by aspect) and draws
 * at most `lines` of them from top y (line height lh); the last one ends with "..." when cut. */
static void wrapped(Out *o, const ModFont *f, const char *s, float x, float y, float px, float lh, float aspect, uint32_t color,
                    float max_w, int lines) {
    char line[MODMENU_NOTE_MAX];
    const char *p = s;
    for (int l = 0; l < lines && *p; l++) {
        while (*p == ' ') p++;
        size_t best = 0, n = 0;
        const size_t len = strlen(p);
        while (n < len) {   /* the longest prefix ending at a word boundary that fits */
            size_t e = n;
            while (e < len && p[e] != ' ' && p[e] != '\n') e++;
            memcpy(line, p, e);
            line[e] = 0;
            if (modmenu_text_width(f, line, px) / aspect > max_w && best) break;
            best = e;
            if (e >= len || p[e] == '\n') break;
            n = e + 1;
        }
        if (l == lines - 1) best = len;   /* the last line takes the rest: text() cuts it with "..." */
        memcpy(line, p, best);
        line[best] = 0;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0, best = (size_t)(nl - line) + 1;
        text(o, f, line, x, y + l * lh, px, aspect, color, x + max_w);
        p += best;
        if (*p == '\n') p++;
    }
}

/* A value menu (ModMenu.values): tabs, label / value / badge columns, the note and the dialog. */
static int layout_values(const ModMenu *m, float W, float H, float aspect, const ModFont *font, Out *o) {
    if (!m->side_panel) put(o, 0, 0, W, H, NULL, 0xb0000000u, MM_TEX_WHITE);
    const float px0 = 0.05f * W, px1 = m->side_panel ? 0.47f * W : 0.95f * W, py0 = 0.05f * H, py1 = 0.95f * H, mx = 0.025f * W, my = 0.025f * H;
    put(o, px0, py0, px1, py1, NULL, 0xf4141414u, MM_TEX_WHITE);
    put(o, px0, py0, px1, py0 + 0.008f * H, NULL, RED, MM_TEX_WHITE);
    const float tpx = 0.055f * H, ty = py0 + my;
    text(o, font, m->title, px0 + mx, ty, tpx, aspect, WHITE, px1 - mx);
    float top = ty + tpx + my / 2;
    const float lx0 = px0 + mx, lx1 = px1 - mx;
    if (m->ntabs) {   /* the tab strip */
        const float th = 0.05f * H, tfx = 0.6f * th, pad = 8 / aspect;
        float x = lx0;
        for (int i = 0; i < m->ntabs; i++) {
            const float tw = modmenu_text_width(font, m->tabs[i], tfx) / aspect + 2 * pad;
            if (i == m->tab) put(o, x, top, x + tw, top + th, NULL, TAB_SEL, MM_TEX_WHITE);
            text(o, font, m->tabs[i], x + pad, top + (th - tfx) / 2, tfx, aspect, i == m->tab ? WHITE : GREY, lx1);
            x += tw + 2 / aspect;
        }
        put(o, lx0, top + th, lx1, top + th + 2, NULL, RED, MM_TEX_WHITE);
        top += th + my / 2;
    }
    const int rows = MODMENU_VALUE_ROWS;
    const float rh = 0.052f * H, fpx = 0.6f * rh, bpx = 0.5f * rh, pad = 4 / aspect, scroll = m->n > rows ? 4.0f / aspect : 0;
    const float lw = lx1 - scroll - lx0, vx0 = lx0 + 0.50f * lw, vx1 = lx0 + 0.76f * lw, bx0 = vx1 + 6 / aspect;
    for (int r = 0; r < rows && m->top + r < m->n; r++) {
        const int i = m->top + r;
        const float y = top + r * rh, ty2 = y + (rh - fpx) / 2;
        const int sel = i == m->sel, grey = m->grey[i];
        if (sel) put(o, lx0, y, lx1 - (scroll ? scroll + 2 / aspect : 0), y + rh, NULL, grey ? 0xff4a2028u : RED, MM_TEX_WHITE);
        const uint32_t c = grey ? DIM : WHITE;
        text(o, font, m->item[i], lx0 + pad, ty2, fpx, aspect, c, vx0 - 6 / aspect);
        if (m->value[i][0]) {   /* "< value >", centred in its column */
            const float aw = modmenu_text_width(font, "<", fpx) / aspect, vw = modmenu_text_width(font, m->value[i], fpx) / aspect;
            float vx = (vx0 + vx1) / 2 - vw / 2;
            if (vx < vx0 + aw + 4 / aspect) vx = vx0 + aw + 4 / aspect;
            if (!grey) text(o, font, "<", vx0, ty2, fpx, aspect, sel ? WHITE : GREY, 0);
            text(o, font, m->value[i], vx, ty2, fpx, aspect, c, vx1 - aw - 4 / aspect);
            if (!grey) text(o, font, ">", vx1 - aw, ty2, fpx, aspect, sel ? WHITE : GREY, 0);
        }
        if (m->badge[i][0]) text(o, font, m->badge[i], bx0, y + (rh - bpx) / 2, bpx, aspect, sel ? WHITE : grey ? DIM : BADGE, lx1 - scroll - 4 / aspect);
    }
    if (scroll) {
        const float bh = rows * rh, t0 = top + bh * m->top / m->n, t1 = top + bh * (m->top + rows) / m->n;
        put(o, lx1 - scroll, top, lx1, top + bh, NULL, 0x40ffffffu, MM_TEX_WHITE);
        put(o, lx1 - scroll, t0, lx1, t1, NULL, 0xd0ffffffu, MM_TEX_WHITE);
    }
    const float npx = 0.036f * H, nlh = 0.042f * H, ny = top + rows * rh + my / 2;
    put(o, lx0, ny - my / 4, lx1, ny - my / 4 + 1, NULL, 0x40ffffffu, MM_TEX_WHITE);
    if (m->note[0]) wrapped(o, font, m->note, lx0, ny, npx, nlh, aspect, NOTE, lx1 - lx0, 3);
    const float fpx2 = 0.034f * H;
    text(o, font, m->footer, lx0, py1 - my / 2 - fpx2, fpx2, aspect, GREY, lx1);
    if (m->confirm_open) {   /* the dialog, centred */
        const float bw = 0.6f * W, bh = 0.26f * H, bx = (W - bw) / 2, by = (H - bh) / 2;
        put(o, 0, 0, W, H, NULL, 0x80000000u, MM_TEX_WHITE);
        put(o, bx - 2, by - 2, bx + bw + 2, by + bh + 2, NULL, RED, MM_TEX_WHITE);
        put(o, bx, by, bx + bw, by + bh, NULL, 0xff181818u, MM_TEX_WHITE);
        wrapped(o, font, m->confirm, bx + mx, by + my, fpx, rh * 0.8f, aspect, WHITE, bw - 2 * mx, 2);
        const float ow = 0.22f * W, oh = rh, oy = by + bh - my - oh;
        const char *lbl[2] = {m->confirm_yes, m->confirm_no};
        for (int k = 0; k < 2; k++) {
            const float ox = k ? bx + bw - mx - ow : bx + mx;
            put(o, ox, oy, ox + ow, oy + oh, NULL, k == m->confirm_sel ? RED : 0xff303030u, MM_TEX_WHITE);
            const float lw2 = modmenu_text_width(font, lbl[k], fpx) / aspect;
            text(o, font, lbl[k], ox + (ow - lw2) / 2, oy + (oh - fpx) / 2, fpx, aspect, WHITE, ox + ow);
        }
    }
    return o->n;
}

int modmenu_layout(const ModMenu *m, int w, int h, float aspect, const ModFont *font, int cover, const float cover_uv[4],
                   ModQuad *out, int max) {
    if (!m->open || max <= 0) return 0;
    if (aspect <= 0) aspect = 1;
    Out o = {out, 0, max};
    if (m->bars > 0) {   /* letterbox bars over the game (photo mode) */
        const float bh = m->bars * (float)h;
        put(&o, 0, 0, (float)w, bh, NULL, 0xff000000u, MM_TEX_WHITE);
        put(&o, 0, (float)h - bh, (float)w, (float)h, NULL, 0xff000000u, MM_TEX_WHITE);
    }
    if (m->progress > 0) {   /* a progress bar at the bottom centre: track, then the done part */
        const float x0 = 0.3f * w, x1 = 0.7f * w, y0 = 0.9f * h, y1 = y0 + 0.018f * h, p = m->progress < 1 ? m->progress : 1;
        put(&o, x0 - 3, y0 - 3, x1 + 3, y1 + 3, NULL, 0xc0000000u, MM_TEX_WHITE);
        put(&o, x0, y0, x0 + (x1 - x0) * p, y1, NULL, RED, MM_TEX_WHITE);
    }
    if (m->toast[0] && m->toast_alpha > 0 && font) {   /* a one-line hint at the bottom centre (photo mode, hidden panel) */
        const float a = m->toast_alpha < 1 ? m->toast_alpha : 1, tpx = 0.04f * h, padx = 0.6f * tpx / aspect, pady = 0.35f * tpx;
        const float tw = modmenu_text_width(font, m->toast, tpx) / aspect, x0 = ((float)w - tw) / 2, y0 = 0.86f * h;
        put(&o, x0 - padx, y0 - pady, x0 + tw + padx, y0 + tpx + pady, NULL, fade_color(0xc0000000u, a), MM_TEX_WHITE);
        text(&o, font, m->toast, x0, y0, tpx, aspect, fade_color(WHITE, a), 0);
    }
    if (m->hidden) return o.n;
    if (m->values || m->ntabs) return layout_values(m, (float)w, (float)h, aspect, font, &o);
    const float W = (float)w, H = (float)h;
    put(&o, 0, 0, W, H, NULL, 0x99000000u, MM_TEX_WHITE);   /* the game, dimmed */
    const float px0 = 0.08f * W, px1 = 0.92f * W, py0 = 0.12f * H, py1 = 0.88f * H, mx = 0.03f * W, my = 0.03f * H;
    put(&o, px0, py0, px1, py1, NULL, 0xf4141414u, MM_TEX_WHITE);
    put(&o, px0, py0, px1, py0 + 0.008f * H, NULL, RED, MM_TEX_WHITE);
    const float tpx = 0.06f * H, ty = py0 + my;
    text(&o, font, m->title, px0 + mx, ty, tpx, aspect, WHITE, px1 - mx);
    const float top = ty + tpx + my;
    float lx0 = px0 + mx;
    if (cover) {
        const float s = 0.46f * H;
        put(&o, lx0, top, lx0 + s / aspect, top + s, cover_uv, WHITE, MM_TEX_COVER);
        lx0 += s / aspect + mx;
    }
    const float lx1 = px1 - mx, rh = 0.055f * H, fpx = 0.6f * rh, scroll = m->n > MODMENU_ROWS ? 4.0f / aspect : 0;
    const float pad = 4 / aspect, nx = lx0 + pad + modmenu_text_width(font, "00", fpx) / aspect + 10 / aspect;
    for (int r = 0; r < MODMENU_ROWS && m->top + r < m->n; r++) {
        const int i = m->top + r;
        const float y = top + r * rh, ty2 = y + (rh - fpx) / 2;
        if (i == m->sel) put(&o, lx0, y, lx1 - (scroll ? scroll + 2 / aspect : 0), y + rh, NULL, RED, MM_TEX_WHITE);
        const char num[3] = {(char)('0' + (i + 1) / 10 % 10), (char)('0' + (i + 1) % 10), 0};
        text(&o, font, num, lx0 + pad, ty2, fpx, aspect, i == m->sel ? WHITE : GREY, 0);
        text(&o, font, m->item[i], nx, ty2, fpx, aspect, WHITE, lx1 - scroll - 6 / aspect);
    }
    if (scroll) {   /* a thin scroll bar: the track and the visible part */
        const float bh = MODMENU_ROWS * rh, t0 = top + bh * m->top / m->n, t1 = top + bh * (m->top + MODMENU_ROWS) / m->n;
        put(&o, lx1 - scroll, top, lx1, top + bh, NULL, 0x40ffffffu, MM_TEX_WHITE);
        put(&o, lx1 - scroll, t0, lx1, t1, NULL, 0xd0ffffffu, MM_TEX_WHITE);
    }
    const float fpx2 = 0.04f * H;
    text(&o, font, m->footer, px0 + mx, py1 - my - fpx2, fpx2, aspect, GREY, px1 - mx);
    return o.n;
}
