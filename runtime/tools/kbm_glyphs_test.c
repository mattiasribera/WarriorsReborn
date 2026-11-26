/* Game-independent test of the keyboard/mouse button icons (src/kbm_glyphs.c): the glyph rect
 * table in a synthetic pak (known offset and scan) and in a synthetic game folder (XBoxWad.idx
 * + WAD), key cap labels, and the atlas painter on a synthetic atlas: only the bound button cells
 * change, a mouse button is red on its own side, a remap changes the cell, painting is
 * deterministic; every glyph code the title draws (incl. the Spanish/French/Italian letter-badge
 * variants of the triggers and stick clicks) -> its pad control -> the default key.
 * The big font atlas: its rect table (known offset, scan, game folder) and the pulsing D-pad sprites
 * of <SDD> <SDL> <SDR> <SDU> (Revive, the heal legend) painted as the bound arrow keys.
 * Usage: kbm_glyphs_test [tmpdir] */
#include "../src/kbm_glyphs.h"
#include "../src/input_map.h"
#include "../src/hle_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The PC default key map: input_map.c itself, over stubs of the pad layer it publishes to. */
#include "../src/input_map.c"
float input_spin_rate = INPUT_SPIN_RATE_DEFAULT;
void input_set_mask(int src, uint32_t mask) { (void)src, (void)mask; }
void input_set_mouse_stick(int16_t rx, int16_t ry) { (void)rx, (void)ry; }
void input_set_spin(int on) { (void)on; }
void input_set_mash(int on) { (void)on; }
void input_tap(int control) { (void)control; }
float input_mash_rate;
void input_note_device(int device) { (void)device; }
const char *input_control_name(int control) { (void)control; return ""; }

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define NREC 371
#define ATLAS 1024

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Synthetic rects: record r is a 30 x 31 cell on a 32-column grid (every record up to NREC). */
static KbmRect synth_rect(int r) {
    KbmRect k = {0, 0, 0, 0};
    if (r < 1024) {
        float x = (float)(r % 32) * 32, y = (float)(r / 32) * 32;
        k = (KbmRect){x / ATLAS, y / ATLAS, (x + 30) / ATLAS, (y + 31) / ATLAS};
    }
    return k;
}

/* A pak with the table at rec_off (header 16 bytes before it); filler bytes elsewhere. */
static uint8_t *synth_pak(size_t n, size_t rec_off) {
    uint8_t *p = malloc(n);
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 13 + 5);
    put32(p + rec_off - 16, NREC);
    put32(p + rec_off - 12, KBM_RECT_BASE);
    put32(p + rec_off - 8, 0);
    put32(p + rec_off - 4, 0);
    for (int r = 0; r < NREC; r++) {
        KbmRect k = synth_rect(r);
        memcpy(p + rec_off + 16 * r, &k, 16);
    }
    return p;
}

static void test_rects(const char *tmp) {
    static KbmRect recs[KBM_RECTS_MAX];
    size_t n = 0x861f4 + NREC * 16 + 64;
    uint8_t *pak = synth_pak(n, 0x861f4);
    int c = kbm_rects_parse(pak, n, recs, KBM_RECTS_MAX);
    KbmRect want = synth_rect(0x9f + KBM_RECT_BASE);
    CHECK(c == NREC && !memcmp(&recs[0x9f + KBM_RECT_BASE], &want, sizeof want), "table at the known offset: %d records", c);
    free(pak);
    pak = synth_pak(200000, 0x1234 + 12);   /* elsewhere, 4-aligned */
    c = kbm_rects_parse(pak, 200000, recs, KBM_RECTS_MAX);
    CHECK(c == NREC && !memcmp(&recs[0x9f + KBM_RECT_BASE], &want, sizeof want), "table found by scan: %d records", c);
    put32(pak + 0x1240 + 16 * 3, 0x40000000);   /* u0 = 2.0: out of range */
    CHECK(kbm_rects_parse(pak, 200000, recs, KBM_RECTS_MAX) == -1, "a record outside [0, 1] rejects the table");
    CHECK(kbm_rects_parse(pak, 100, recs, KBM_RECTS_MAX) == -1, "no table in a short buffer");

    /* Game folder: XBoxWad.idx {wads, files, 0x100000, wad_table, file_table} and System.wad0
     * holding the pak at 2 KiB (offset field = loc & 0xffffff in KiB). */
    char path[512];
    snprintf(path, sizeof path, "%s/disc", tmp);
    mkdir(path, 0755);
    uint8_t idx[20 + 36 + 2 * 16];
    memset(idx, 0, sizeof idx);
    put32(idx, 1);
    put32(idx + 4, 2);
    put32(idx + 8, 0x100000);
    put32(idx + 12, 20);
    put32(idx + 16, 56);
    memcpy(idx + 20, "System.wad0", 11);
    put32(idx + 56, 0);                       /* another file */
    put32(idx + 60, 10);
    put32(idx + 68, 0x12345678);
    put32(idx + 72, 2);                       /* wad 0, offset 2 KiB */
    put32(idx + 76, 0x861f4 + NREC * 16);
    put32(idx + 84, KBM_PAK_NAME_HASH);
    snprintf(path, sizeof path, "%s/disc/XBoxWad.idx", tmp);
    FILE *f = fopen(path, "wb");
    fwrite(idx, 1, sizeof idx, f);
    fclose(f);
    pak = synth_pak(0x861f4 + NREC * 16, 0x861f4);
    snprintf(path, sizeof path, "%s/disc/System.wad0", tmp);
    f = fopen(path, "wb");
    static const uint8_t pad[2048];
    fwrite(pad, 1, sizeof pad, f);
    fwrite(pak, 1, 0x861f4 + NREC * 16, f);
    fclose(f);
    free(pak);
    snprintf(path, sizeof path, "%s/disc", tmp);
    memset(recs, 0, sizeof recs);
    c = kbm_rects_load(path, recs, KBM_RECTS_MAX);
    CHECK(c == NREC && !memcmp(&recs[0x9f + KBM_RECT_BASE], &want, sizeof want), "table from XBoxWad.idx + System.wad0: %d", c);
    snprintf(path, sizeof path, "%s/missing", tmp);
    CHECK(kbm_rects_load(path, recs, KBM_RECTS_MAX) == -1, "no game folder -> -1");
}

static void test_labels(void) {
    CHECK(!strcmp(kbm_key_label('e', 0), "E") && !strcmp(kbm_key_label('7', 1), "7"), "letters upper case, digits as is");
    CHECK(!strcmp(kbm_key_label(INPUT_KEY_LSHIFT, 0), "SHIFT") && !strcmp(kbm_key_label(INPUT_KEY_LSHIFT, 1), "SHF"),
          "Shift: SHIFT, short SHF");
    CHECK(!strcmp(kbm_key_label(' ', 0), "SPACE") && !strcmp(kbm_key_label(' ', 1), "SPC"), "Space: SPACE, short SPC");
    CHECK(!strcmp(kbm_key_label(INPUT_KEY_F1 + 4, 0), "F5"), "F5");
    CHECK(kbm_key_label(INPUT_KEY_UP, 0)[0] == 1 && kbm_key_label(INPUT_KEY_RIGHT, 0)[0] == 4, "arrows are arrow glyphs");
    CHECK(!kbm_key_label(INPUT_MOUSE_LEFT, 0) && !kbm_key_label(-1, 0), "no label for mouse buttons or unknown keys");
}

/* Synthetic atlas: a gray texel pattern, opaque in the rows 4..30 of every cell (so the face
 * buttons' band is 4/31..31/31), transparent elsewhere. */
static uint32_t *synth_atlas(void) {
    uint32_t *a = malloc(ATLAS * ATLAS * 4);
    for (int y = 0; y < ATLAS; y++)
        for (int x = 0; x < ATLAS; x++) {
            int cy = y % 32;
            uint32_t g = (uint32_t)((x * 3 + y * 5) & 0xff);
            a[y * ATLAS + x] = (cy >= 4 && cy < 31 ? 0xff000000u : 0x10000000u) | g << 16 | g << 8 | g;
        }
    return a;
}

static int cell_of(int record, int *x0, int *y0) {
    KbmRect k = synth_rect(record);
    *x0 = (int)(k.u0 * ATLAS + 0.5f);
    *y0 = (int)(k.v0 * ATLAS + 0.5f);
    return k.u1 > k.u0;
}

static int is_red(uint32_t p) {
    int a = p >> 24, r = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
    return a > 200 && r > 150 && g < 60 && b < 80;
}

/* Red texels of a record's cell in its left / right half. */
static void red_halves(const uint32_t *a, int record, int *left, int *right) {
    int x0, y0;
    *left = *right = 0;
    cell_of(record, &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 30; x++)
            if (is_red(a[y * ATLAS + x])) (x < x0 + 15 ? left : right)[0]++;
}

static int cell_differs(const uint32_t *a, const uint32_t *b, int record) {
    int x0, y0, d = 0;
    cell_of(record, &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 30; x++) d += a[y * ATLAS + x] != b[y * ATLAS + x];
    return d;
}

static int record_of(int control) {
    for (int i = 0; i < kbm_cell_count(); i++)
        if (kbm_cell(i)->control == control && kbm_cell(i)->record >= KBM_RECT_BASE) return kbm_cell(i)->record;
    return -1;
}

static void test_paint(void) {
    static KbmRect recs[NREC];
    for (int r = 0; r < NREC; r++) recs[r] = synth_rect(r);
    int keys[IN_COUNT];
    for (int c = 0; c < IN_COUNT; c++) keys[c] = -1;
    keys[IN_X] = INPUT_MOUSE_LEFT;
    keys[IN_A] = INPUT_MOUSE_RIGHT;
    keys[IN_B] = 'f';
    keys[IN_Y] = 'e';
    keys[IN_LT] = 'q';
    keys[IN_RT] = INPUT_KEY_LSHIFT;
    keys[IN_START] = '\r';
    keys[IN_UP] = INPUT_KEY_UP;
    /* IN_BLACK, IN_WHITE, IN_BACK, the stick clicks and the other D-pad directions stay unbound */
    uint32_t *base = synth_atlas(), *a = synth_atlas();
    int painted = kbm_paint_atlas(a, ATLAS, ATLAS, recs, NREC, keys);
    int want = 0;
    for (int i = 0; i < kbm_cell_count(); i++) want += keys[kbm_cell(i)->control] >= 0;
    CHECK(painted == want && want >= 12, "%d cells painted (bound cells incl. the large face sprites: %d)", painted, want);

    /* No texel outside the painted cells changes. */
    static uint8_t inside[ATLAS * ATLAS];
    memset(inside, 0, sizeof inside);
    for (int i = 0; i < kbm_cell_count(); i++) {
        int x0, y0;
        if (keys[kbm_cell(i)->control] < 0 || !cell_of(kbm_cell(i)->record, &x0, &y0)) continue;
        for (int y = y0; y < y0 + 31; y++) memset(inside + y * ATLAS + x0, 1, 30);
    }
    int outside = 0, untouched = 0;
    for (int i = 0; i < ATLAS * ATLAS; i++)
        if (!inside[i]) outside += a[i] != base[i];
    for (int i = 0; i < kbm_cell_count(); i++)
        if (keys[kbm_cell(i)->control] >= 0) untouched += !cell_differs(a, base, kbm_cell(i)->record);
    CHECK(outside == 0, "no texel outside the bound cells changes (%d changed)", outside);
    CHECK(untouched == 0, "every bound cell changes (%d unchanged)", untouched);
    CHECK(!cell_differs(a, base, record_of(IN_BLACK)) && !cell_differs(a, base, record_of(IN_BACK)),
          "unbound controls (Black, Back) keep the base's art");

    int l, r;
    red_halves(a, record_of(IN_X), &l, &r);
    CHECK(l > 5 && r == 0, "X on mouse.left: red in the left half only (%d / %d texels)", l, r);
    red_halves(a, record_of(IN_A), &l, &r);
    CHECK(r > 5 && l == 0, "A on mouse.right: red in the right half only (%d / %d texels)", l, r);
    red_halves(a, record_of(IN_B), &l, &r);
    CHECK(l + r == 0, "key caps have no red");
    int opaque_light = 0, x0, y0;
    cell_of(record_of(IN_Y), &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 30; x++) {
            uint32_t p = a[y * ATLAS + x];
            opaque_light += (p >> 24) > 200 && (p & 0xff) > 200;
        }
    CHECK(opaque_light > 20, "the E key cap has light label texels (%d)", opaque_light);
    /* Rows outside the band (0..3) stay transparent: the icon sits where the old glyph did. */
    int top = 0;
    for (int y = y0; y < y0 + 3; y++)
        for (int x = x0; x < x0 + 30; x++) top += (a[y * ATLAS + x] >> 24) > 16;
    CHECK(top == 0, "nothing drawn above the glyph band (%d texels)", top);

    uint32_t *b = synth_atlas();
    kbm_paint_atlas(b, ATLAS, ATLAS, recs, NREC, keys);
    CHECK(!memcmp(a, b, ATLAS * ATLAS * 4), "painting is deterministic");
    memcpy(b, base, ATLAS * ATLAS * 4);
    keys[IN_Y] = 'f';
    kbm_paint_atlas(b, ATLAS, ATLAS, recs, NREC, keys);
    CHECK(cell_differs(a, b, record_of(IN_Y)) > 0 && !cell_differs(a, b, record_of(IN_B)), "a remap (Y: E -> F) repaints the cell");
    int yf = 0;
    {
        int bx, by;
        cell_of(record_of(IN_B), &bx, &by);
        cell_of(record_of(IN_Y), &x0, &y0);
        for (int y = 0; y < 31; y++)
            for (int x = 0; x < 30; x++) yf += b[(y0 + y) * ATLAS + x0 + x] != b[(by + y) * ATLAS + bx + x];
    }
    CHECK(yf == 0, "the same key gives the same icon in two cells (%d texels differ)", yf);
    memcpy(b, base, ATLAS * ATLAS * 4);
    keys[IN_X] = 'r';
    kbm_paint_atlas(b, ATLAS, ATLAS, recs, NREC, keys);
    red_halves(b, record_of(IN_X), &l, &r);
    CHECK(l + r == 0, "X remapped to R: a key cap, no mouse");

    /* A 4x atlas (same normalized rects) paints the full long labels and scales. */
    uint32_t *big = calloc((size_t)ATLAS * 4 * ATLAS * 4, 4);
    for (int i = 0; i < ATLAS * 4 * ATLAS * 4; i++) big[i] = 0xff000000u;
    CHECK(kbm_paint_atlas(big, ATLAS * 4, ATLAS * 4, recs, NREC, keys) == painted, "a 4096 x 4096 atlas paints the same cells");
    free(big);
    free(a);
    free(b);
    free(base);
}

/* Every button glyph code the title's text tags produce (0xe2580: the tag switch; the trigger and
 * stick-click tags pick a letter-badge variant by the game language [[0x5b9f78]+0x11c]: 0 English
 * and German, 1 Spanish, 2 French, 3 Italian), the Xbox control its art shows and the key of the
 * PC default layout (input_map.c). */
static const struct { int code, control, key; const char *what; } glyph_codes[] = {
    {0x9e, IN_A, INPUT_MOUSE_RIGHT, "A"}, {0x9d, IN_B, 'f', "B"}, {0x9f, IN_X, INPUT_MOUSE_LEFT, "X"},
    {0x96, IN_Y, 'e', "Y"}, {0x9c, IN_BLACK, INPUT_KEY_LCTRL, "Black"}, {0x91, IN_WHITE, '\t', "White"},
    {0x97, IN_START, '\r', "Start"}, {0x93, IN_BACK, '\b', "Back"},
    {0x98, IN_LEFT, INPUT_KEY_LEFT, "D-pad left"}, {0x99, IN_UP, INPUT_KEY_UP, "D-pad up"},
    {0x9a, IN_RIGHT, INPUT_KEY_RIGHT, "D-pad right"}, {0x9b, IN_DOWN, INPUT_KEY_DOWN, "D-pad down"},
    {0xa0, IN_RT, INPUT_KEY_LSHIFT, "right trigger (en/de)"},
    {0xa2, IN_RT, INPUT_KEY_LSHIFT, "right trigger 'D' (es/fr/it)"},
    {0x95, IN_LT, 'q', "left trigger (en/de)"},
    {0xa3, IN_LT, 'q', "left trigger 'I' (es)"},
    {0xa5, IN_LT, 'q', "left trigger 'G' (fr)"},
    {0xa4, IN_LT, 'q', "left trigger 'S' (it)"},
    {0x94, IN_LTHUMB, 'c', "left stick click (en/de)"},
    {0xa6, IN_LTHUMB, 'c', "left stick click 'I' (es)"},
    {0xa7, IN_LTHUMB, 'c', "left stick click 'G' (fr)"},
    {0xb3, IN_LTHUMB, 'c', "left stick click 'S' (it)"},
    {0x92, IN_RTHUMB, INPUT_MOUSE_MIDDLE, "right stick click (en/de)"},
    {0xb2, IN_RTHUMB, INPUT_MOUSE_MIDDLE, "right stick click 'D' (es/fr/it)"},
};

static const KbmCell *cell_for_code(int code) {
    for (int i = 0; i < kbm_cell_count(); i++)
        if (kbm_cell(i)->record == code + KBM_RECT_BASE) return kbm_cell(i);
    return NULL;
}

static void test_codes(void) {
    input_map_defaults();
    int n = (int)(sizeof glyph_codes / sizeof *glyph_codes), glyph_cells = 0;
    for (int i = 0; i < n; i++) {
        const KbmCell *c = cell_for_code(glyph_codes[i].code);
        int control = c ? c->control : -1, key = input_key_for_control(control);
        CHECK(control == glyph_codes[i].control && key == glyph_codes[i].key, "0x%02x %s -> control %d (want %d) -> key %d (want %d)",
              glyph_codes[i].code, glyph_codes[i].what, control, glyph_codes[i].control, key, glyph_codes[i].key);
    }
    for (int i = 0; i < kbm_cell_count(); i++) glyph_cells += kbm_cell(i)->record >= KBM_RECT_BASE;
    CHECK(glyph_cells == n, "the cell table has exactly the %d glyph codes (%d)", n, glyph_cells);

    /* Spanish: the variant cells are repainted with the same icon as the English ones. */
    static KbmRect recs[NREC];
    for (int r = 0; r < NREC; r++) recs[r] = synth_rect(r);
    int keys[IN_COUNT];
    for (int c = 0; c < IN_COUNT; c++) keys[c] = input_key_for_control(c);
    uint32_t *a = synth_atlas();
    kbm_paint_atlas(a, ATLAS, ATLAS, recs, NREC, keys);
    static const int pairs[][2] = {{0x95, 0xa3}, {0x95, 0xa4}, {0x95, 0xa5}, {0xa0, 0xa2}, {0x94, 0xa6}, {0x94, 0xa7}, {0x94, 0xb3}, {0x92, 0xb2}};
    for (size_t p = 0; p < sizeof pairs / sizeof *pairs; p++) {
        int ax, ay, bx, by, d = 0;
        cell_of(pairs[p][0] + KBM_RECT_BASE, &ax, &ay);
        cell_of(pairs[p][1] + KBM_RECT_BASE, &bx, &by);
        for (int y = 0; y < 31; y++)
            for (int x = 0; x < 30; x++) d += a[(ay + y) * ATLAS + ax + x] != a[(by + y) * ATLAS + bx + x];
        CHECK(d == 0, "variant 0x%02x shows the same key icon as 0x%02x (%d texels differ)", pairs[p][1], pairs[p][0], d);
    }
    free(a);
}

/* Keys whose names are long draw a symbol on the key cap instead of a (shortened) label. */
static int light_texels(const uint32_t *img, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += (img[i] >> 24) > 200 && (img[i] & 0xff) > 200;
    return c;
}

static void test_symbols(void) {
    static const struct { int key, sym; const char *what; } want[] = {
        {INPUT_KEY_LSHIFT, KBM_SYM_SHIFT, "left Shift"}, {INPUT_KEY_RSHIFT, KBM_SYM_SHIFT, "right Shift"},
        {'\r', KBM_SYM_ENTER, "Enter"}, {'\t', KBM_SYM_TAB, "Tab"}, {'\b', KBM_SYM_BACKSPACE, "Backspace"},
        {' ', KBM_SYM_SPACE, "Space"}, {'a', KBM_SYM_NONE, "A"}, {'7', KBM_SYM_NONE, "7"},
        {INPUT_KEY_LCTRL, KBM_SYM_NONE, "Ctrl"}, {INPUT_KEY_RALT, KBM_SYM_NONE, "Alt"}, {INPUT_KEY_UP, KBM_SYM_NONE, "Up"},
        {INPUT_KEY_F1, KBM_SYM_NONE, "F1"}, {INPUT_MOUSE_LEFT, KBM_SYM_NONE, "mouse"}, {-1, KBM_SYM_NONE, "none"},
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        CHECK(kbm_key_symbol(want[i].key) == want[i].sym, "symbol of %s: %d (want %d)", want[i].what, kbm_key_symbol(want[i].key), want[i].sym);

    /* Each symbol cap at the 1x cell size and at 4x: drawn, different from its text cap and from
     * the other symbols; both Shift keys draw the same cap. */
    static const int keys[] = {INPUT_KEY_LSHIFT, '\r', '\t', '\b', ' '};
    static const int sizes[][2] = {{30, 31}, {120, 124}};
    for (int z = 0; z < 2; z++) {
        int w = sizes[z][0], h = sizes[z][1], n = w * h;
        uint32_t *img[5], *text = malloc((size_t)n * 4), *other = malloc((size_t)n * 4);
        for (int k = 0; k < 5; k++) {
            img[k] = malloc((size_t)n * 4);
            kbm_draw_icon(img[k], w, h, w, keys[k], 4.0f / 31, 1);
            kbm_key_symbols = 0;
            kbm_draw_icon(text, w, h, w, keys[k], 4.0f / 31, 1);
            kbm_key_symbols = 1;
            int diff = 0;
            for (int i = 0; i < n; i++) diff += img[k][i] != text[i];
            CHECK(light_texels(img[k], n) > (z ? 300 : 20) && diff > n / 40, "%dx%d: %s symbol cap drawn (%d light texels), "
                  "%d texels differ from the text cap", w, h, kbm_key_label(keys[k], 0), light_texels(img[k], n), diff);
        }
        int distinct = 1;
        for (int a = 0; a < 5; a++)
            for (int b = a + 1; b < 5; b++) distinct &= memcmp(img[a], img[b], (size_t)n * 4) != 0;
        kbm_draw_icon(other, w, h, w, INPUT_KEY_RSHIFT, 4.0f / 31, 1);
        CHECK(distinct && !memcmp(other, img[0], (size_t)n * 4), "%dx%d: the five symbols differ; right Shift = left Shift", w, h);
        for (int k = 0; k < 5; k++) free(img[k]);
        free(text);
        free(other);
    }
}

static void test_icon(void) {
    uint32_t img[40 * 40];
    memset(img, 0x55, sizeof img);
    kbm_draw_icon(img, 40, 40, 40, INPUT_MOUSE_MIDDLE, 0, 1);
    int red_mid = 0, corner = img[0] >> 24;
    for (int y = 0; y < 40; y++)
        for (int x = 15; x < 25; x++) red_mid += is_red(img[y * 40 + x]);
    CHECK(red_mid > 3 && corner == 0, "mouse.middle: red wheel in the middle (%d), cleared corners", red_mid);
    kbm_draw_icon(img, 40, 40, 40, INPUT_KEY_F1 + 11, 0, 1);
    int light = 0;
    for (int i = 0; i < 1600; i++) light += (img[i] >> 24) > 200 && (img[i] & 0xff) > 200;
    CHECK(light > 30, "F12 key cap drawn (%d light texels)", light);
}

/* Which atlas the icons are painted on. A pack's replacement wins; a pack-replaced atlas is never
 * upscaled by --sharp-fonts (its job never starts), so waiting for it would keep the Xbox icons
 * forever (a .dds-only pack atlas the runtime could not read). */
static void test_base(void) {
    CHECK(kbm_atlas_base(1, 1, 1, 0) == KBM_BASE_PACK, "a readable pack atlas is painted on");
    CHECK(kbm_atlas_base(0, 1, 1, 0) == KBM_BASE_GAME, "an unreadable pack atlas with sharp fonts on: the game's atlas, no wait");
    CHECK(kbm_atlas_base(0, 1, 1, 1) == KBM_BASE_GAME, "... whatever the sharp job's state");
    CHECK(kbm_atlas_base(0, 0, 1, 0) == KBM_BASE_WAIT && kbm_atlas_base(0, 0, 1, 1) == KBM_BASE_WAIT,
          "no pack, sharp fonts on: wait while the upscale has not started or runs");
    CHECK(kbm_atlas_base(0, 0, 1, 2) == KBM_BASE_SHARP && kbm_atlas_base(0, 0, 1, 3) == KBM_BASE_SHARP, "... then the sharp atlas");
    CHECK(kbm_atlas_base(0, 0, 1, 4) == KBM_BASE_GAME, "a failed upscale: the game's atlas");
    CHECK(kbm_atlas_base(0, 0, 0, 0) == KBM_BASE_GAME, "no pack, no sharp fonts: the game's atlas");
}

/* ---- The big font atlas: the pulsing D-pad sprites ---- */

#define BIG_NREC 262
#define BIG_OFF 0x1728c4

/* A pak with the main table at 0x861f4 and the big font table (base 0) at big_off. */
static uint8_t *synth_big_pak(size_t n, size_t big_off) {
    uint8_t *p = synth_pak(n, 0x861f4);
    put32(p + big_off - 16, BIG_NREC);
    put32(p + big_off - 12, 0);
    put32(p + big_off - 8, 0);
    put32(p + big_off - 4, 0);
    for (int r = 0; r < BIG_NREC; r++) {
        KbmRect k = synth_rect(r);
        memcpy(p + big_off + 16 * r, &k, 16);
    }
    return p;
}

static const KbmCell *big_cell_of(int control) {
    for (int i = 0; i < kbm_big_cell_count(); i++)
        if (kbm_big_cell(i)->control == control) return kbm_big_cell(i);
    return NULL;
}

/* Red texels of a record's cell in its left / right / top / bottom half. */
static void red_quads(const uint32_t *a, int record, int *l, int *r, int *t, int *b) {
    int x0, y0;
    *l = *r = *t = *b = 0;
    cell_of(record, &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 30; x++)
            if (is_red(a[y * ATLAS + x])) {
                (x < x0 + 15 ? l : r)[0]++;
                (y < y0 + 15 ? t : b)[0]++;
            }
}

static void test_big_rects(const char *tmp) {
    static KbmRect recs[KBM_RECTS_MAX];
    size_t n = BIG_OFF + BIG_NREC * 16 + 64;
    uint8_t *pak = synth_big_pak(n, BIG_OFF);
    KbmRect want = synth_rect(259);
    int c = kbm_big_rects_parse(pak, n, recs, KBM_RECTS_MAX);
    CHECK(c == BIG_NREC && !memcmp(&recs[259], &want, sizeof want), "big font table at the known offset: %d records", c);
    free(pak);
    pak = synth_big_pak(n, 0x100000 + 12);
    c = kbm_big_rects_parse(pak, n, recs, KBM_RECTS_MAX);
    CHECK(c == BIG_NREC && !memcmp(&recs[259], &want, sizeof want), "big font table found by scan: %d records", c);
    free(pak);
    n = 0x861f4 + NREC * 16 + 64;
    pak = synth_pak(n, 0x861f4);
    CHECK(kbm_big_rects_parse(pak, n, recs, KBM_RECTS_MAX) == -1, "the main table (base 0x5e) is not the big font's");
    free(pak);

    char path[512];
    snprintf(path, sizeof path, "%s/disc_big", tmp);
    mkdir(path, 0755);
    n = BIG_OFF + BIG_NREC * 16;
    uint8_t idx[20 + 36 + 16];
    memset(idx, 0, sizeof idx);
    put32(idx, 1);
    put32(idx + 4, 1);
    put32(idx + 8, 0x100000);
    put32(idx + 12, 20);
    put32(idx + 16, 56);
    memcpy(idx + 20, "System.wad0", 11);
    put32(idx + 56, 1);                       /* wad 0, offset 1 KiB */
    put32(idx + 60, (uint32_t)n);
    put32(idx + 68, KBM_PAK_NAME_HASH);
    snprintf(path, sizeof path, "%s/disc_big/XBoxWad.idx", tmp);
    FILE *f = fopen(path, "wb");
    fwrite(idx, 1, sizeof idx, f);
    fclose(f);
    pak = synth_big_pak(n, BIG_OFF);
    snprintf(path, sizeof path, "%s/disc_big/System.wad0", tmp);
    f = fopen(path, "wb");
    static const uint8_t pad[1024];
    fwrite(pad, 1, sizeof pad, f);
    fwrite(pak, 1, n, f);
    fclose(f);
    free(pak);
    snprintf(path, sizeof path, "%s/disc_big", tmp);
    memset(recs, 0, sizeof recs);
    c = kbm_big_rects_load(path, recs, KBM_RECTS_MAX);
    CHECK(c == BIG_NREC && !memcmp(&recs[259], &want, sizeof want), "big font table from the game folder: %d", c);
    c = kbm_rects_load(path, recs, KBM_RECTS_MAX);
    want = synth_rect(0x9f + KBM_RECT_BASE);
    CHECK(c == NREC && !memcmp(&recs[0x9f + KBM_RECT_BASE], &want, sizeof want), "the main table from the same pak: %d", c);
}

/* <SDD> <SDL> <SDR> <SDU> (0xe2e3b..: font 6, record base 0x100) pulse between code 2 (the
 * neutral D-pad) and 3/4/5/6; font 6 draws code c at record c - 1 + base. Revive is <SDL>. */
static const struct { int code, control; const char *what; } big_codes[] = {
    {2, -1, "neutral D-pad (every SD tag's other frame)"}, {3, IN_DOWN, "<SDD> down"},
    {4, IN_LEFT, "<SDL> left (Revive)"}, {5, IN_RIGHT, "<SDR> right"}, {6, IN_UP, "<SDU> up"},
};

static void test_big_paint(void) {
    CHECK(kbm_big_cell_count() == 5, "5 big font D-pad cells (%d)", kbm_big_cell_count());
    for (int i = 0; i < 5; i++) {
        const KbmCell *c = big_cell_of(big_codes[i].control);
        CHECK(c && c->record == KBM_BIG_DPAD_BASE + big_codes[i].code - 1, "code %d %s -> record %d", big_codes[i].code,
              big_codes[i].what, c ? c->record : -1);
    }
    static KbmRect recs[BIG_NREC];
    for (int r = 0; r < BIG_NREC; r++) recs[r] = synth_rect(r);
    int keys[IN_COUNT];
    input_map_defaults();
    for (int c = 0; c < IN_COUNT; c++) keys[c] = input_key_for_control(c);
    uint32_t *base = synth_atlas(), *a = synth_atlas();
    int painted = kbm_paint_big_atlas(a, ATLAS, ATLAS, recs, BIG_NREC, keys);
    CHECK(painted == 5, "default arrows: 5 D-pad cells painted (%d)", painted);
    static uint8_t inside[ATLAS * ATLAS];
    memset(inside, 0, sizeof inside);
    for (int i = 0; i < kbm_big_cell_count(); i++) {
        int x0, y0;
        cell_of(kbm_big_cell(i)->record, &x0, &y0);
        for (int y = y0; y < y0 + 31; y++) memset(inside + y * ATLAS + x0, 1, 30);
    }
    int outside = 0, untouched = 0;
    for (int i = 0; i < ATLAS * ATLAS; i++)
        if (!inside[i]) outside += a[i] != base[i];
    for (int i = 0; i < kbm_big_cell_count(); i++) untouched += !cell_differs(a, base, kbm_big_cell(i)->record);
    CHECK(outside == 0 && untouched == 0, "only the 5 D-pad cells change (%d outside, %d unchanged)", outside, untouched);

    int l, r, t, b;
    red_quads(a, big_cell_of(-1)->record, &l, &r, &t, &b);
    CHECK(l + r == 0, "neutral: the four keys, none lit (%d red)", l + r);
    red_quads(a, big_cell_of(IN_LEFT)->record, &l, &r, &t, &b);
    CHECK(l > 3 && r == 0 && t == 0, "left (Revive): the left key lit, bottom left (%d/%d/%d)", l, r, t);
    red_quads(a, big_cell_of(IN_RIGHT)->record, &l, &r, &t, &b);
    CHECK(r > 3 && l == 0 && t == 0, "right: the right key lit, bottom right (%d/%d/%d)", r, l, t);
    red_quads(a, big_cell_of(IN_UP)->record, &l, &r, &t, &b);
    CHECK(t > 3 && t > 3 * b && l > 0 && r > 0, "up: the top middle key lit (%d/%d)", t, b);
    red_quads(a, big_cell_of(IN_DOWN)->record, &l, &r, &t, &b);
    CHECK(b > 3 && t == 0 && l > 0 && r > 0, "down: the bottom middle key lit (%d/%d)", b, t);

    uint32_t *c2 = synth_atlas();
    keys[IN_LEFT] = 'a';
    kbm_paint_big_atlas(c2, ATLAS, ATLAS, recs, BIG_NREC, keys);
    int changed = 0;
    for (int i = 0; i < kbm_big_cell_count(); i++) changed += cell_differs(a, c2, kbm_big_cell(i)->record) > 0;
    CHECK(changed == 5, "a remap (left: arrow -> A) repaints every cell: each shows the four keys (%d)", changed);
    uint32_t *c3 = synth_atlas();
    kbm_paint_big_atlas(c3, ATLAS, ATLAS, recs, BIG_NREC, keys);
    CHECK(!memcmp(c2, c3, ATLAS * ATLAS * 4), "painting is deterministic");
    memcpy(c3, base, ATLAS * ATLAS * 4);
    keys[IN_LEFT] = -1;
    painted = kbm_paint_big_atlas(c3, ATLAS, ATLAS, recs, BIG_NREC, keys);
    CHECK(painted == 4 && !cell_differs(c3, base, big_cell_of(IN_LEFT)->record),
          "left unbound: its cell keeps the base's art, the others are painted (%d)", painted);
    memcpy(c3, base, ATLAS * ATLAS * 4);
    keys[IN_UP] = keys[IN_DOWN] = keys[IN_RIGHT] = -1;
    CHECK(kbm_paint_big_atlas(c3, ATLAS, ATLAS, recs, BIG_NREC, keys) == 0 && !memcmp(c3, base, ATLAS * ATLAS * 4),
          "no D-pad direction bound: nothing painted");
    memcpy(c3, base, ATLAS * ATLAS * 4);
    CHECK(kbm_paint_big_atlas(c3, ATLAS, ATLAS, recs, 200, keys) == 0, "a table too short for the sprites: nothing painted");
    keys[IN_LEFT] = INPUT_MOUSE_X1;
    keys[IN_RIGHT] = INPUT_KEY_RIGHT;
    memcpy(c3, base, ATLAS * ATLAS * 4);
    CHECK(kbm_paint_big_atlas(c3, ATLAS, ATLAS, recs, BIG_NREC, keys) == 3, "a mouse button on a direction is drawn too");
    free(a);
    free(c2);
    free(c3);
    free(base);
}

/* The arrow key caps are mirror-symmetric left to right (a lopsided down arrow was drawn once). */
/* Texels of record ra's cell in atlas a equal those of record rb's cell in atlas b. */
static int cell_equals(const uint32_t *a, int ra, const uint32_t *b, int rb) {
    int ax, ay, bx, by, d = 0;
    cell_of(ra, &ax, &ay);
    cell_of(rb, &bx, &by);
    for (int y = 0; y < 31; y++)
        for (int x = 0; x < 30; x++) d += a[(ay + y) * ATLAS + ax + x] != b[(by + y) * ATLAS + bx + x];
    return d == 0;
}

/* Controller remap: a cell shows the base's own art of the button that now does its action. */
static void test_pad_remap(void) {
    static KbmRect recs[NREC];
    for (int r = 0; r < NREC; r++) recs[r] = synth_rect(r);
    int shown[IN_COUNT];
    for (int c = 0; c < IN_COUNT; c++) shown[c] = c;
    uint32_t *base = synth_atlas(), *a = synth_atlas();
    CHECK(kbm_remap_atlas(a, ATLAS, ATLAS, recs, NREC, shown) == 0 && !memcmp(a, base, ATLAS * ATLAS * 4),
          "pad remap icons: no remap, no texel changes");
    shown[IN_A] = IN_X, shown[IN_X] = IN_A, shown[IN_START] = IN_RT, shown[IN_BACK] = -1;
    int n = kbm_remap_atlas(a, ATLAS, ATLAS, recs, NREC, shown);
    int want = 0;
    for (int i = 0; i < kbm_cell_count(); i++) {
        int c = kbm_cell(i)->control;
        want += c == IN_A || c == IN_X || c == IN_START;
    }
    CHECK(n == want && n == 5, "pad remap icons: the A, X (char + large) and Start cells repainted (%d of %d)", n, want);
    CHECK(cell_equals(a, record_of(IN_A), base, record_of(IN_X)) && cell_equals(a, record_of(IN_X), base, record_of(IN_A)),
          "pad remap icons: A and X swapped, the A prompt shows the X button");
    CHECK(cell_equals(a, 79, base, 65) && cell_equals(a, 65, base, 79), "pad remap icons: the large A and X sprites swap too");
    CHECK(cell_equals(a, record_of(IN_START), base, record_of(IN_RT)), "pad remap icons: Start done by RT shows the RT glyph");
    static uint8_t inside[ATLAS * ATLAS];
    memset(inside, 0, sizeof inside);
    for (int i = 0; i < kbm_cell_count(); i++) {
        int c = kbm_cell(i)->control, x0, y0;
        if ((c != IN_A && c != IN_X && c != IN_START) || !cell_of(kbm_cell(i)->record, &x0, &y0)) continue;
        for (int y = y0; y < y0 + 31; y++) memset(inside + y * ATLAS + x0, 1, 30);
    }
    int outside = 0;
    for (int i = 0; i < ATLAS * ATLAS; i++)
        if (!inside[i]) outside += a[i] != base[i];
    CHECK(outside == 0, "pad remap icons: nothing else changes, an action no button does keeps its glyph (%d)", outside);

    /* A source cell of another size is scaled to fit, centred, the rest of the cell cleared. */
    static KbmRect wide[NREC];
    memcpy(wide, recs, sizeof wide);
    int rt = record_of(IN_RT);
    wide[rt].u1 = wide[rt].u0 + 15.0f / ATLAS;   /* RT: 15 x 31 */
    memcpy(a, base, ATLAS * ATLAS * 4);
    for (int c = 0; c < IN_COUNT; c++) shown[c] = c;
    shown[IN_START] = IN_RT;
    kbm_remap_atlas(a, ATLAS, ATLAS, wide, NREC, shown);
    int x0, y0, clear = 1;
    cell_of(record_of(IN_START), &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 7; x++) clear &= (a[y * ATLAS + x] >> 24) == 0;
    CHECK(clear && cell_differs(a, base, record_of(IN_START)), "pad remap icons: a narrower source is centred, its sides cleared");

    /* Big font D-pad sprites: directions swap among themselves; another button keeps the sprite. */
    static KbmRect brecs[BIG_NREC];
    for (int r = 0; r < BIG_NREC; r++) brecs[r] = synth_rect(r);
    memcpy(a, base, ATLAS * ATLAS * 4);
    for (int c = 0; c < IN_COUNT; c++) shown[c] = c;
    shown[IN_UP] = IN_DOWN, shown[IN_DOWN] = IN_UP, shown[IN_LEFT] = IN_A;
    n = kbm_remap_big_atlas(a, ATLAS, ATLAS, brecs, BIG_NREC, shown);
    CHECK(n == 2 && cell_equals(a, big_cell_of(IN_UP)->record, base, big_cell_of(IN_DOWN)->record) &&
          cell_equals(a, big_cell_of(IN_DOWN)->record, base, big_cell_of(IN_UP)->record) &&
          !cell_differs(a, base, big_cell_of(IN_LEFT)->record), "pad remap icons: D-pad up/down sprites swap, left on A keeps its sprite (%d)", n);
    free(a);
    free(base);
}

static void test_arrow_symmetry(void) {
    static const int arrows[2] = {INPUT_KEY_UP, INPUT_KEY_DOWN};
    for (int k = 0; k < 2; k++) {
        enum { N = 64 };
        static uint32_t px[N * N];
        kbm_draw_icon(px, N, N, N, arrows[k], 0, 1);   /* the arrow is light ink on the dark face */
        int asym = 0;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N / 2; x++) {
                int a = px[y * N + x] & 0xff, b = px[y * N + N - 1 - x] & 0xff, d = a > b ? a - b : b - a;
                asym += d > 64;
            }
        CHECK(asym == 0, "%s arrow key cap is symmetric (%d texels differ)", k ? "down" : "up", asym);
    }
}

/* No white border: on every key cap and mouse icon, at small and large sizes, the texels on the
 * icon's outline (opaque, next to a transparent texel or the canvas edge) are dark, not light ink;
 * the label/symbol (or the red bound mouse button) stays inside. */
static void test_no_white_rim(void) {
    static const int keys[] = {'e', 'w', ' ', INPUT_KEY_LSHIFT, '\r', '\t', INPUT_KEY_UP, INPUT_KEY_LEFT,
                               INPUT_KEY_F1 + 11, INPUT_MOUSE_LEFT, INPUT_MOUSE_RIGHT, INPUT_MOUSE_MIDDLE,
                               INPUT_MOUSE_WHEEL, INPUT_MOUSE_X1, INPUT_MOUSE_X2};
    static const int sizes[] = {24, 32, 64, 128};
    static uint32_t px[128 * 128];
    int rim_light = 0, no_label = 0;
    for (int s = 0; s < 4; s++)
        for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
            int n = sizes[s], light = 0;
            memset(px, 0, sizeof px);
            kbm_draw_icon(px, n, n, n, keys[k], 0, 1);
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    uint32_t p = px[y * n + x];
                    if ((p >> 24) < 128) continue;
                    int lum = ((p >> 16 & 0xff) + (p >> 8 & 0xff) + (p & 0xff)) / 3;
                    light += lum > 200 || is_red(p);   /* the label/symbol, or the red bound button */
                    int edge = !x || !y || x == n - 1 || y == n - 1 || (px[y * n + x - 1] >> 24) < 64 ||
                               (px[y * n + x + 1] >> 24) < 64 || (px[(y - 1) * n + x] >> 24) < 64 ||
                               (px[(y + 1) * n + x] >> 24) < 64;
                    if (edge && lum > 160) rim_light++;
                }
            no_label += light < 3;
        }
    CHECK(rim_light == 0, "no light texel on any key/mouse icon's outline (%d)", rim_light);
    CHECK(no_label == 0, "every icon still has its light label/symbol or red button (%d without)", no_label);
}

/* PlayStation-style controller icons painted by the runtime (--button-icons ps-black|ps-white|
 * ps-color, --pad-icons): with no glyph pack, the rects come from the user's own global.pak (here a
 * synthetic one) and our shapes are painted into the atlas at its own size. */
static int lum_of(uint32_t p) { return ((p >> 16 & 0xff) + (p >> 8 & 0xff) + (p & 0xff)) / 3; }

/* Opaque (alpha > 200) texels of a record's cell: dark (lum < 60) and light (lum > 200) ones; the
 * light texels on the opaque outline (next to a texel of alpha < 64 or the cell border). */
static void cell_tones(const uint32_t *a, int record, int *dark, int *light, int *rim_light) {
    int x0, y0;
    *dark = *light = *rim_light = 0;
    cell_of(record, &x0, &y0);
    for (int y = y0; y < y0 + 31; y++)
        for (int x = x0; x < x0 + 30; x++) {
            uint32_t p = a[y * ATLAS + x];
            if ((p >> 24) <= 200) continue;
            int l = lum_of(p);
            *dark += l < 60;
            *light += l > 200;
            int edge = x == x0 || y == y0 || x == x0 + 29 || y == y0 + 30 || (a[y * ATLAS + x - 1] >> 24) < 64 ||
                       (a[y * ATLAS + x + 1] >> 24) < 64 || (a[(y - 1) * ATLAS + x] >> 24) < 64 || (a[(y + 1) * ATLAS + x] >> 24) < 64;
            *rim_light += edge && l > 160;
        }
}

static void test_ps_paint(void) {
    CHECK(kbm_ps_style("ps-black") == KBM_PS_BLACK && kbm_ps_style("ps-white") == KBM_PS_WHITE &&
          kbm_ps_style("ps-color") == KBM_PS_COLOR && kbm_ps_style("xbox") == KBM_PS_NONE && kbm_ps_style("nes") < 0,
          "ps icons: style names");
    /* No glyph pack: the rects from a synthetic global.pak, the atlas the game's own (synthetic). */
    static KbmRect recs[KBM_RECTS_MAX];
    size_t n = 0x861f4 + NREC * 16 + 64;
    uint8_t *pak = synth_pak(n, 0x861f4);
    int nrecs = kbm_rects_parse(pak, n, recs, KBM_RECTS_MAX);
    free(pak);
    uint32_t *base = synth_atlas(), *a = synth_atlas();
    int painted = kbm_paint_ps_atlas(a, ATLAS, ATLAS, recs, nrecs, KBM_PS_BLACK);
    CHECK(nrecs == NREC && painted == kbm_cell_count(), "ps-black, no glyph pack: every button cell painted (%d of %d)", painted,
          kbm_cell_count());
    static uint8_t inside[ATLAS * ATLAS];
    memset(inside, 0, sizeof inside);
    for (int i = 0; i < kbm_cell_count(); i++) {
        int x0, y0;
        if (!cell_of(kbm_cell(i)->record, &x0, &y0)) continue;
        for (int y = y0; y < y0 + 31; y++) memset(inside + y * ATLAS + x0, 1, 30);
    }
    int outside = 0, untouched = 0;
    for (int i = 0; i < ATLAS * ATLAS; i++)
        if (!inside[i]) outside += a[i] != base[i];
    for (int i = 0; i < kbm_cell_count(); i++) untouched += !cell_differs(a, base, kbm_cell(i)->record);
    CHECK(outside == 0 && untouched == 0, "ps-black: only the button cells change (%d outside, %d unchanged)", outside, untouched);
    int dark, light, rim, bad = 0, rims = 0;
    for (int i = 0; i < kbm_cell_count(); i++) {
        cell_tones(a, kbm_cell(i)->record, &dark, &light, &rim);
        bad += dark < 40 || light < 4;   /* a black button with a white symbol or label */
        if (kbm_cell(i)->control > IN_RIGHT) rims += rim;   /* the D-pad's lit key is solid ink */
    }
    CHECK(bad == 0, "ps-black: every cell is a black button with white ink (%d not)", bad);
    CHECK(rims == 0, "ps-black: no light rim on any button outline (%d texels)", rims);
    int x0, y0;
    cell_of(record_of(IN_A), &x0, &y0);
    CHECK(lum_of(a[(y0 + 17) * ATLAS + x0 + 15]) > 200, "ps-black: the cross crosses at the A cell's centre");
    cell_of(record_of(IN_B), &x0, &y0);
    CHECK(lum_of(a[(y0 + 17) * ATLAS + x0 + 15]) < 60, "ps-black: the circle is a ring (the B cell's centre is the button)");
    /* Rows outside the band the base's face buttons use (0..3) stay transparent. */
    int above = 0;
    for (int y = y0; y < y0 + 3; y++)
        for (int x = x0; x < x0 + 30; x++) above += (a[y * ATLAS + x] >> 24) > 16;
    CHECK(above == 0, "ps-black: the icon sits in the base's glyph band (%d texels above it)", above);
    uint32_t *again = synth_atlas();
    kbm_paint_ps_atlas(again, ATLAS, ATLAS, recs, nrecs, KBM_PS_BLACK);
    CHECK(!memcmp(a, again, ATLAS * ATLAS * 4), "ps-black: painting is deterministic");

    /* White: a white button with black ink; color: the face symbols in colour. */
    uint32_t *w = synth_atlas();
    kbm_paint_ps_atlas(w, ATLAS, ATLAS, recs, nrecs, KBM_PS_WHITE);
    bad = rims = 0;
    for (int i = 0; i < kbm_cell_count(); i++) {
        cell_tones(w, kbm_cell(i)->record, &dark, &light, &rim);
        bad += light < 40 || dark < 4;
    }
    CHECK(bad == 0, "ps-white: every cell is a white button with black ink (%d not)", bad);
    uint32_t *c = synth_atlas();
    kbm_paint_ps_atlas(c, ATLAS, ATLAS, recs, nrecs, KBM_PS_COLOR);
    int blue = 0, red = 0;
    for (int y = 0; y < 31; y++)
        for (int x = 0; x < 30; x++) {
            int ax, ay, bx, by;
            cell_of(record_of(IN_A), &ax, &ay);
            cell_of(record_of(IN_B), &bx, &by);
            uint32_t p = c[(ay + y) * ATLAS + ax + x], q = c[(by + y) * ATLAS + bx + x];
            blue += (p >> 24) > 200 && (p & 0xff) > 180 && (p >> 16 & 0xff) < 160;
            red += (q >> 24) > 200 && (q >> 16 & 0xff) > 200 && (q & 0xff) < 140;
        }
    CHECK(blue > 4 && red > 4, "ps-color: a blue cross and a red circle (%d / %d texels)", blue, red);
    CHECK(!cell_differs(c, a, record_of(IN_START)), "ps-color: the labels match ps-black (START)");

    /* An upscaled atlas (a texture pack's, or --sharp-fonts): the same cells, painted at its size. */
    uint32_t *big = calloc((size_t)ATLAS * 2 * ATLAS * 2, 4);
    int p2 = kbm_paint_ps_atlas(big, ATLAS * 2, ATLAS * 2, recs, nrecs, KBM_PS_BLACK);
    int ink2 = 0;
    KbmRect k = recs[record_of(IN_Y)];
    for (int y = (int)(k.v0 * ATLAS * 2); y < (int)(k.v1 * ATLAS * 2); y++)
        for (int x = (int)(k.u0 * ATLAS * 2); x < (int)(k.u1 * ATLAS * 2); x++) {
            uint32_t p = big[(size_t)y * ATLAS * 2 + x];
            ink2 += (p >> 24) > 200 && lum_of(p) > 200;
        }
    CHECK(p2 == kbm_cell_count() && ink2 > 16, "ps-black on a 2x atlas: painted at its size (%d cells, %d triangle texels)", p2, ink2);

    /* The big font's D-pad sprites: a PlayStation D-pad, the sprite's direction lit. */
    static KbmRect brecs[BIG_NREC];
    for (int r = 0; r < BIG_NREC; r++) brecs[r] = synth_rect(r);
    uint32_t *d = synth_atlas();
    int nb = kbm_paint_ps_big_atlas(d, ATLAS, ATLAS, brecs, BIG_NREC, KBM_PS_BLACK);
    memset(inside, 0, sizeof inside);
    for (int i = 0; i < kbm_big_cell_count(); i++) {
        cell_of(kbm_big_cell(i)->record, &x0, &y0);
        for (int y = y0; y < y0 + 31; y++) memset(inside + y * ATLAS + x0, 1, 30);
    }
    outside = untouched = 0;
    for (int i = 0; i < ATLAS * ATLAS; i++)
        if (!inside[i]) outside += d[i] != base[i];
    for (int i = 0; i < kbm_big_cell_count(); i++) untouched += !cell_differs(d, base, kbm_big_cell(i)->record);
    CHECK(nb == 5 && outside == 0 && untouched == 0, "ps-black big font: the 5 D-pad sprites painted, nothing else (%d, %d, %d)", nb,
          outside, untouched);
    int ul, ur, dl, dr;   /* up lit: more light texels in the top half than the neutral sprite */
    cell_tones(d, big_cell_of(IN_UP)->record, &dark, &ul, &rim);
    cell_tones(d, big_cell_of(-1)->record, &dark, &ur, &rim);
    cell_tones(d, big_cell_of(IN_DOWN)->record, &dark, &dl, &rim);
    dr = ur;
    CHECK(ul > ur && dl > dr, "ps-black big font: a lit direction has more ink than the neutral D-pad (%d, %d > %d)", ul, dl, ur);
    CHECK(kbm_paint_ps_big_atlas(d, ATLAS, ATLAS, brecs, 100, KBM_PS_BLACK) == 0, "ps big font: a short table paints nothing");
    CHECK(kbm_paint_ps_atlas(d, ATLAS, ATLAS, recs, nrecs, KBM_PS_NONE) == 0, "ps icons: no style paints nothing");
    free(a), free(again), free(w), free(c), free(big), free(d), free(base);
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "/tmp";
    test_rects(tmp);
    test_labels();
    test_paint();
    test_codes();
    test_symbols();
    test_icon();
    test_base();
    test_big_rects(tmp);
    test_big_paint();
    test_arrow_symmetry();
    test_no_white_rim();
    test_pad_remap();
    test_ps_paint();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
