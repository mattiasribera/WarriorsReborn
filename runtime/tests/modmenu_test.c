/* Tests of src/modmenu.c: the mod menu's state machine, fades and layout (no game data). */
#include "../src/modmenu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, what)                                                     \
    do {                                                                   \
        if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } \
        else printf("ok   %s\n", what);                                    \
    } while (0)

static ModMenu m;

/* Steps once and returns the event; *ix the item. */
static int st(unsigned c, double t, int *ix) {
    int i = -9;
    int ev = modmenu_step(&m, c, t, &i);
    if (ix) *ix = i;
    return ev;
}

static void test_open(void) {
    int n = modmenu_open(&m, "Rocola", "one\ntwo\nthree\n", "A ok", 1, 1000);
    CHECK(n == 3 && m.n == 3 && m.open && m.sel == 1, "open: three items (trailing newline), sel 1");
    CHECK(!strcmp(m.item[2], "three") && !strcmp(m.title, "Rocola") && !strcmp(m.footer, "A ok"), "open: texts kept");
    n = modmenu_open(&m, "t", "a\nb", "", 9, 0);
    CHECK(n == 2 && m.sel == 1, "open: selection clamped");
    char big[MODMENU_ITEMS_MAX * 3 + 10] = "";
    for (int i = 0; i < MODMENU_ITEMS_MAX + 3; i++) strcat(big, "x\n");
    CHECK(modmenu_open(&m, "t", big, "", 0, 0) == MODMENU_ITEMS_MAX, "open: at most MODMENU_ITEMS_MAX items");
    char longl[300];
    memset(longl, 'a', sizeof longl - 1);
    longl[sizeof longl - 1] = 0;
    longl[MODMENU_TEXT_MAX - 2] = (char)0xc3;   /* a 2-byte UTF-8 character across the cut */
    longl[MODMENU_TEXT_MAX - 1] = (char)0xa9;
    modmenu_open(&m, "t", longl, "", 0, 0);
    CHECK(strlen(m.item[0]) == MODMENU_TEXT_MAX - 2, "open: long line cut before a split character");
    CHECK(modmenu_open(&m, "t", "", "", 0, 0) == 0 && !m.open, "open: no items keeps it closed");
}

static void test_navigation(void) {
    int ix;
    modmenu_open(&m, "t", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl", "", 0, 0);
    CHECK(st(0, 10, &ix) == MM_EV_NONE, "nav: nothing before the debounce");
    CHECK(st(0, 160, &ix) == MM_EV_HOVER && ix == 0, "nav: first item hovered after 150 ms");
    CHECK(st(0, 400, &ix) == MM_EV_NONE, "nav: hover reported once");
    st(MM_DOWN, 500, NULL);
    CHECK(m.sel == 1, "nav: down moves one");
    st(MM_DOWN, 520, NULL);
    CHECK(m.sel == 1, "nav: a held down does not move again before the repeat delay");
    st(MM_DOWN, 500 + MODMENU_REPEAT_DELAY_MS + 1, NULL);
    CHECK(m.sel == 2, "nav: held down repeats after the delay");
    st(MM_DOWN, 500 + MODMENU_REPEAT_DELAY_MS + MODMENU_REPEAT_MS + 2, NULL);
    CHECK(m.sel == 3, "nav: then every repeat period");
    CHECK(st(0, 1100, &ix) == MM_EV_NONE, "nav: fast scrolling: no hover until the selection rests 150 ms");
    CHECK(st(0, 1150, &ix) == MM_EV_HOVER && ix == 3, "nav: hover on the item where it rested");
    st(MM_UP, 1400, NULL);
    st(0, 1410, NULL);
    st(MM_UP, 1420, NULL);
    st(0, 1430, NULL);
    st(MM_UP, 1440, NULL);
    st(0, 1450, NULL);
    st(MM_UP, 1460, NULL);
    CHECK(m.sel == 11, "nav: up from the first item wraps to the last");
    CHECK(m.top <= m.sel && m.sel < m.top + MODMENU_ROWS, "nav: the selection stays in the visible rows");
    st(0, 1470, NULL);
    st(MM_LEFT, 1480, NULL);
    CHECK(m.sel == 11 - MODMENU_ROWS, "nav: left pages up");
    st(0, 1490, NULL);
    st(MM_LEFT, 1500, NULL);
    CHECK(m.sel == 0, "nav: page up clamps at the first item");
    st(0, 1510, NULL);
    st(MM_RIGHT, 1520, NULL);
    st(0, 1530, NULL);
    st(MM_RIGHT, 1540, NULL);
    CHECK(m.sel == 11 && m.top == 12 - MODMENU_ROWS, "nav: page down clamps at the last item, list scrolled to its end");
}

static void test_confirm_cancel(void) {
    int ix;
    modmenu_open(&m, "t", "a\nb\nc", "", 0, 0);
    CHECK(st(MM_OK, 10, &ix) == MM_EV_NONE && m.open, "confirm: OK still held from opening is ignored");
    st(0, 20, NULL);
    st(MM_DOWN, 30, NULL);
    st(0, 40, NULL);
    CHECK(st(MM_OK, 50, &ix) == MM_EV_CHOOSE && ix == 1 && !m.open, "confirm: a new OK press chooses the selection and closes");
    modmenu_open(&m, "t", "a\nb\nc", "", 2, 0);
    st(0, 10, NULL);
    CHECK(st(MM_BACK, 20, &ix) == MM_EV_CANCEL && ix == 2 && !m.open, "cancel: BACK closes with the selection");
    CHECK(st(MM_OK, 30, &ix) == MM_EV_NONE, "closed: no events");
    modmenu_open(&m, "t", "a", "", 0, 0);
    st(MM_BACK, 10, NULL);
    CHECK(m.open, "cancel: BACK held from before the menu opened is ignored");
}

static void test_controls(void) {
    uint8_t an[8] = {0};
    CHECK(modmenu_controls(0x1, an, 0, 0) == MM_UP && modmenu_controls(0x2, an, 0, 0) == MM_DOWN, "controls: d-pad up / down");
    CHECK(modmenu_controls(0, an, 0, 20000) == MM_UP && modmenu_controls(0, an, 0, -20000) == MM_DOWN, "controls: left stick");
    CHECK(modmenu_controls(0, an, 0, 9000) == 0, "controls: small stick deflection ignored");
    CHECK(modmenu_controls(0, an, -20000, 0) == MM_LEFT && modmenu_controls(0x8, an, 0, 0) == MM_RIGHT, "controls: left / right");
    an[0] = 0xff;
    CHECK(modmenu_controls(0, an, 0, 0) == MM_OK, "controls: A is OK");
    an[0] = 0;
    an[3] = 0x80;
    CHECK(modmenu_controls(0, an, 0, 0) == MM_OK, "controls: Y is OK (the action key)");
    an[3] = 0;
    CHECK(modmenu_controls(0x10, an, 0, 0) == MM_OK, "controls: Start (Enter) is OK");
    an[1] = 0xff;
    CHECK(modmenu_controls(0, an, 0, 0) == MM_BACK, "controls: B is BACK");
    an[1] = 0;
    CHECK(modmenu_controls(0x20, an, 0, 0) == MM_BACK, "controls: Back (Backspace) is BACK");
}

static void test_fade(void) {
    ModFade f;
    modfade_init(&f, 0);
    modfade_set(&f, 1, 300, 1000);
    CHECK(fabsf(modfade_level(&f, 1000)) < 1e-6f && fabsf(modfade_level(&f, 1150) - 0.5f) < 1e-4f, "fade: linear from 0 halfway at 150 ms");
    CHECK(modfade_level(&f, 1400) == 1.0f && modfade_done(&f, 1300) && !modfade_done(&f, 1299), "fade: reaches the target at 300 ms");
    modfade_set(&f, 0, 300, 1150);
    CHECK(fabsf(modfade_level(&f, 1150) - 0.5f) < 1e-4f && fabsf(modfade_level(&f, 1300) - 0.25f) < 1e-4f, "fade: a new target starts from the current level");
    modfade_set(&f, 0.25f, 0, 2000);
    CHECK(modfade_level(&f, 2000) == 0.25f, "fade: zero duration jumps");
    CHECK(fabsf(modfade_gain(0.5f) - 0.70710678f) < 1e-4f && modfade_gain(0) == 0 && fabsf(modfade_gain(1) - 1) < 1e-6f,
          "fade: equal-power gain");
    CHECK(fabsf(modfade_gain(0.3f) * modfade_gain(0.3f) + modfade_gain(0.7f) * modfade_gain(0.7f) - 1) < 1e-4f, "fade: crossfade keeps power");
}

static void test_layout(void) {
    static float rects[96][4];
    for (int i = 0; i < 96; i++) {   /* a fake 16 x 6 grid of 32 x 32 cells in a 512 x 192 atlas */
        rects[i][0] = (i % 16) * 32 / 512.0f;
        rects[i][1] = (i / 16) * 32 / 192.0f;
        rects[i][2] = rects[i][0] + 16 / 512.0f;   /* 16 texels wide, 32 high */
        rects[i][3] = rects[i][1] + 32 / 192.0f;
    }
    ModFont f = {rects, 32, 96, 512, 192};
    CHECK(fabsf(modmenu_text_width(&f, "AB", 20) - 20) < 1e-3f, "layout: glyph width scales with the height (16/32 * 20 * 2)");
    CHECK(fabsf(modmenu_text_width(&f, "\x01", 21) - 7) < 1e-3f, "layout: unknown character = a third of the height");
    static float lrects[256][4];
    for (int i = 0; i < 256; i++) lrects[i][2] = 16 / 512.0f, lrects[i][3] = 32 / 192.0f;
    ModFont lf = {lrects, 0, 256, 512, 192};
    CHECK(fabsf(modmenu_text_width(&lf, "m\xfasica", 20) - 60) < 1e-3f && fabsf(modmenu_text_width(&lf, "m\xc3\xbasica", 20) - 60) < 1e-3f,
          "layout: Latin-1 bytes and UTF-8 letters are one glyph each");
    static ModQuad q[MODMENU_QUADS_MAX];
    modmenu_open(&m, "Rocola", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl", "A elegir", 10, 0);
    const float uv[4] = {0, 0.2f, 0.5f, 0.7f};
    int n = modmenu_layout(&m, 640, 480, 1, &f, 1, uv, q, MODMENU_QUADS_MAX);
    int covers = 0, inside = 1, highlight = 0, glyphs = 0;
    for (int i = 0; i < n; i++) {
        if (q[i].tex == MM_TEX_COVER) {
            covers++;
            if (q[i].u0 != 0 || q[i].v0 != 0.2f || q[i].u1 != 0.5f || q[i].v1 != 0.7f) inside = 0;
        }
        if (q[i].x0 < 0 || q[i].y0 < 0 || q[i].x1 > 640 || q[i].y1 > 480 || q[i].x1 < q[i].x0 || q[i].y1 < q[i].y0) inside = 0;
        if (q[i].tex == MM_TEX_FONT) glyphs++;
        if (q[i].tex == MM_TEX_WHITE && q[i].color == 0xffc8102eu) highlight++;
    }
    CHECK(n > 0 && n < MODMENU_QUADS_MAX && covers == 1, "layout: one cover quad with the given uv");
    CHECK(inside, "layout: every quad on screen");
    CHECK(highlight >= 1, "layout: the selected row has the red highlight");
    /* visible rows: 9 one-letter items + "Rocola" (6) + "A elegir" (7 glyphs, space excluded) + row numbers "04".."12" (18) */
    CHECK(glyphs == MODMENU_ROWS + 6 + 7 + 2 * MODMENU_ROWS, "layout: only the visible rows' text is drawn");
    int n2 = modmenu_layout(&m, 640, 480, 1, &f, 0, uv, q, MODMENU_QUADS_MAX);
    covers = 0;
    for (int i = 0; i < n2; i++) covers += q[i].tex == MM_TEX_COVER;
    CHECK(covers == 0, "layout: no cover quad without a cover");
    CHECK(modmenu_layout(&m, 640, 480, 1, &f, 1, uv, q, 5) == 5, "layout: at most max quads");
    modmenu_close(&m);
    CHECK(modmenu_layout(&m, 640, 480, 1, &f, 1, uv, q, MODMENU_QUADS_MAX) == 0, "layout: nothing when closed");
    modmenu_open(&m, "t", "a", "", 0, 0);
    n = modmenu_layout(&m, 640, 480, 0.75f, &f, 1, uv, q, MODMENU_QUADS_MAX);
    float cw = 0, ch = 0;
    for (int i = 0; i < n; i++)
        if (q[i].tex == MM_TEX_COVER) { cw = q[i].x1 - q[i].x0; ch = q[i].y1 - q[i].y0; }
    CHECK(ch > 0 && fabsf(cw * 0.75f - ch) < 0.5f, "layout: the cover stays square on wide pixels");
}


/* ---- value menus (the in-game settings menu) ---- */
static void test_values(void) {
    int ix;
    modmenu_open(&m, "Ajustes", "Brillo\nNitidez\nResplandor", "", 0, 0);
    modmenu_values(&m, 1);
    snprintf(m.value[0], sizeof m.value[0], "50");
    m.grey[2] = 1;
    st(0, 10, NULL);
    CHECK(st(MM_RIGHT, 20, &ix) == MM_EV_ADJUST && ix == 0 && m.adjust == 1 && m.sel == 0, "values: right adjusts the row (+1), no paging");
    CHECK(st(MM_RIGHT, 30, &ix) == MM_EV_NONE, "values: a held right waits for the repeat delay");
    CHECK(st(MM_RIGHT, 20 + MODMENU_REPEAT_DELAY_MS + 1, &ix) == MM_EV_ADJUST && m.adjust == 1, "values: a held right repeats");
    st(0, 500, NULL);
    CHECK(st(MM_LEFT, 510, &ix) == MM_EV_ADJUST && ix == 0 && m.adjust == -1, "values: left adjusts (-1)");
    st(0, 520, NULL);
    CHECK(st(MM_OK, 530, &ix) == MM_EV_CHOOSE && ix == 0 && m.open, "values: OK chooses and the menu stays open");
    st(0, 540, NULL);
    st(MM_DOWN, 550, NULL);
    st(0, 560, NULL);
    st(MM_DOWN, 570, NULL);
    st(0, 580, NULL);
    CHECK(m.sel == 2, "values: a greyed row can be selected (its reason shows)");
    CHECK(st(MM_RIGHT, 590, &ix) == MM_EV_NONE && st(0, 600, NULL) == MM_EV_NONE && st(MM_OK, 610, &ix) == MM_EV_NONE && m.open,
          "values: a greyed row neither adjusts nor chooses");
    st(0, 620, NULL);
    CHECK(st(MM_BACK, 630, &ix) == MM_EV_CANCEL && !m.open, "values: BACK still closes");
    CHECK(modmenu_rows(&m) == MODMENU_VALUE_ROWS, "values: the value menu's row count");
}

static void test_tabs(void) {
    int ix;
    modmenu_open(&m, "Ajustes", "a\nb\nc", "", 2, 0);
    modmenu_values(&m, 1);
    modmenu_tabs(&m, "Pantalla\nEfectos\nAudio", 0);
    CHECK(m.ntabs == 3 && m.tab == 0 && !strcmp(m.tabs[1], "Efectos"), "tabs: three tabs, the first selected");
    st(0, 10, NULL);
    CHECK(st(MM_TAB_NEXT, 20, &ix) == MM_EV_TAB && ix == 1 && m.tab == 1, "tabs: next");
    CHECK(st(MM_TAB_NEXT, 600, &ix) != MM_EV_TAB && m.tab == 1, "tabs: a held tab button does not repeat");
    st(0, 610, NULL);
    CHECK(st(MM_TAB_PREV, 620, &ix) == MM_EV_TAB && ix == 0, "tabs: previous");
    st(0, 630, NULL);
    CHECK(st(MM_TAB_PREV, 640, &ix) == MM_EV_TAB && ix == 2, "tabs: previous from the first wraps to the last");
    CHECK(modmenu_set_items(&m, "x\ny", 5) == 2 && m.sel == 1 && !strcmp(m.item[0], "x") && m.open && m.hovered == -1,
          "tabs: new items replace the old, selection clamped");
    m.value[1][0] = 'v';
    m.grey[1] = 1;
    modmenu_set_items(&m, "p\nq", 0);
    CHECK(!m.value[1][0] && !m.grey[1], "tabs: new items clear the values and greyed flags");
    CHECK(modmenu_set_items(&m, "", 0) == 0 && m.n == 2, "tabs: no items keeps the old ones");
    modmenu_open(&m, "Rocola", "a\nb", "", 0, 0);
    CHECK(m.ntabs == 0 && !m.values, "tabs: a plain open has no tabs and no values (the jukebox)");
    st(0, 10, NULL);
    CHECK(st(MM_TAB_NEXT, 20, &ix) == MM_EV_NONE, "tabs: without tabs the tab buttons do nothing");
}

static void test_confirm_dialog(void) {
    int ix;
    modmenu_open(&m, "Ajustes", "a\nb", "", 1, 0);
    modmenu_values(&m, 1);
    st(0, 10, NULL);
    modmenu_confirm(&m, "Reiniciar ahora?", "Si", "No");
    CHECK(m.confirm_open && m.confirm_sel == 0, "confirm: open with yes selected");
    CHECK(st(MM_DOWN, 20, &ix) == MM_EV_NONE && m.sel == 1 && m.confirm_sel == 1, "confirm: directions move the answer, not the list");
    st(0, 30, NULL);
    CHECK(st(MM_OK, 40, &ix) == MM_EV_CONFIRM && ix == 0 && !m.confirm_open && m.open, "confirm: OK on no answers 0, the menu stays");
    modmenu_confirm(&m, "Reiniciar ahora?", "Si", "No");
    st(0, 50, NULL);
    CHECK(st(MM_OK, 60, &ix) == MM_EV_CONFIRM && ix == 1, "confirm: OK on yes answers 1");
    modmenu_confirm(&m, "Reiniciar ahora?", "Si", "No");
    st(0, 70, NULL);
    CHECK(st(MM_BACK, 80, &ix) == MM_EV_CONFIRM && ix == 0 && m.open, "confirm: BACK answers no and keeps the menu open");
}

static void test_value_layout(void) {
    static float rects[96][4];
    for (int i = 0; i < 96; i++) {
        rects[i][0] = (i % 16) * 32 / 512.0f;
        rects[i][1] = (i / 16) * 32 / 192.0f;
        rects[i][2] = rects[i][0] + 16 / 512.0f;
        rects[i][3] = rects[i][1] + 32 / 192.0f;
    }
    ModFont f = {rects, 32, 96, 512, 192};
    static ModQuad q[MODMENU_QUADS_MAX];
    modmenu_open(&m, "T", "Aa\nBb\nCc", "", 0, 0);
    modmenu_values(&m, 1);
    modmenu_tabs(&m, "P\nE", 1);
    snprintf(m.value[0], sizeof m.value[0], "V1");
    snprintf(m.badge[0], sizeof m.badge[0], "BG");
    m.grey[1] = 1;
    snprintf(m.note, sizeof m.note, "%s", "word word word word word word word word word word word word word word word "
             "word word word word word word word word word word word word word word word word word word word word word word word word word");
    int n = modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX);
    int inside = 1, tab_hl = 0, grey_glyphs = 0, glyphs = 0;
    float note_y[8];
    int nlines = 0;
    for (int i = 0; i < n; i++) {
        if (q[i].x0 < 0 || q[i].y0 < 0 || q[i].x1 > 640 || q[i].y1 > 480) inside = 0;
        if (q[i].tex == MM_TEX_FONT) glyphs++;
        if (q[i].tex == MM_TEX_FONT && q[i].color == 0xff5a5a5au) grey_glyphs++;
        if (q[i].tex == MM_TEX_WHITE && q[i].color == 0xff6a0818u) tab_hl++;
        if (q[i].tex == MM_TEX_FONT && q[i].color == 0xffb4b4b4u) {
            int seen = 0;
            for (int k = 0; k < nlines; k++) seen |= note_y[k] == q[i].y0;
            if (!seen && nlines < 8) note_y[nlines++] = q[i].y0;
        }
    }
    CHECK(n > 0 && inside, "value layout: every quad on screen");
    CHECK(tab_hl == 1, "value layout: the selected tab is highlighted");
    CHECK(grey_glyphs == 2, "value layout: the greyed row's label is drawn grey");
    /* T + P E + Aa Bb Cc + "< V1 >" (V1 and the two arrows) + BG; the note is extra */
    CHECK(glyphs >= 1 + 2 + 6 + 4 + 2, "value layout: title, tabs, labels, value with arrows and badge");
    CHECK(nlines >= 2 && nlines <= 3, "value layout: the long note wraps to at most 3 lines");
    modmenu_confirm(&m, "Q?", "Y", "N");
    int n2 = modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX);
    CHECK(n2 > n, "value layout: the confirm dialog adds its box and text");
}

/* Photo mode's overlay options: hidden draws only the letterbox and the progress bar; side_panel keeps
 * the game undimmed and the value panel in the left half. */
static void test_photo_overlay(void) {
    static ModMenu m;
    static ModQuad q[MODMENU_QUADS_MAX];
    modmenu_open(&m, "Photo", "a\nb", "", 0, 0);
    modmenu_values(&m, 1);
    m.hidden = 1;
    CHECK(modmenu_layout(&m, 640, 480, 1, NULL, 0, NULL, q, MODMENU_QUADS_MAX) == 0, "photo: hidden with no bars or progress draws nothing");
    m.bars = 0.1f;
    m.progress = 0.5f;
    int n = modmenu_layout(&m, 640, 480, 1, NULL, 0, NULL, q, MODMENU_QUADS_MAX);
    CHECK(n == 4 && q[0].y0 == 0 && q[0].y1 == 48 && q[1].y0 == 432 && q[1].y1 == 480 && q[0].color == 0xff000000u, "photo: two bars of 10% of the height");
    CHECK(q[3].x1 - q[3].x0 == 0.5f * (q[2].x1 - q[2].x0 - 6), "photo: the progress bar is half full");
    m.hidden = 0, m.bars = 0, m.progress = 0, m.side_panel = 1;
    n = modmenu_layout(&m, 640, 480, 1, NULL, 0, NULL, q, MODMENU_QUADS_MAX);
    int dim = 0;
    float right = 0;
    for (int i = 0; i < n; i++) {
        if (q[i].x0 == 0 && q[i].y0 == 0 && q[i].x1 == 640 && q[i].y1 == 480) dim = 1;
        if (q[i].x1 > right) right = q[i].x1;
    }
    CHECK(n > 0 && !dim && right <= 0.5f * 640, "photo: the side panel leaves the game undimmed and the right half free");
    m.side_panel = 0;
    n = modmenu_layout(&m, 640, 480, 1, NULL, 0, NULL, q, MODMENU_QUADS_MAX);
    CHECK(n > 0 && q[0].x1 == 640 && q[0].y1 == 480, "photo: a plain value menu still dims the game first");
    /* The toast: a short hint pill drawn even while hidden, faded by toast_alpha (0 = none). */
    static float rects[96][4];
    for (int i = 0; i < 96; i++) rects[i][0] = rects[i][1] = 0, rects[i][2] = 16 / 512.0f, rects[i][3] = 32 / 192.0f;
    ModFont f = {rects, 32, 96, 512, 192};
    m.hidden = 1;
    snprintf(m.toast, sizeof m.toast, "Y: show panel");
    m.toast_alpha = 0;
    CHECK(modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX) == 0, "photo: a toast at alpha 0 draws nothing");
    m.toast_alpha = 1;
    n = modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX);
    int glyphs = 0;
    for (int i = 0; i < n; i++) glyphs += q[i].tex == MM_TEX_FONT;
    CHECK(n == 12 && glyphs == 11 && q[0].tex == MM_TEX_WHITE && q[0].y0 > 0.8f * 480 && q[0].x0 > 0 && q[0].x1 < 640,
          "photo: hidden with a toast draws its box at the bottom and its text");
    const uint32_t full = q[1].color, box = q[0].color;
    m.toast_alpha = 0.5f;
    n = modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX);
    CHECK(n == 12 && (q[1].color >> 24) == (uint32_t)((full >> 24) * 0.5f + 0.5f) && (q[1].color & 0xffffff) == (full & 0xffffff) &&
              (q[0].color >> 24) == (uint32_t)((box >> 24) * 0.5f + 0.5f),
          "photo: the toast fades with toast_alpha");
    m.toast[0] = 0;
    m.toast_alpha = 1;
    CHECK(modmenu_layout(&m, 640, 480, 1, &f, 0, NULL, q, MODMENU_QUADS_MAX) == 0, "photo: an empty toast draws nothing");
    m.hidden = 0;
}

int main(void) {
    test_open();
    test_navigation();
    test_confirm_cancel();
    test_controls();
    test_fade();
    test_layout();
    test_values();
    test_tabs();
    test_confirm_dialog();
    test_value_layout();
    test_photo_overlay();
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
