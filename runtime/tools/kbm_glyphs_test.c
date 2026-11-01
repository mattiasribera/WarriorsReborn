/* Game-independent test of the keyboard/mouse button icons (src/kbm_glyphs.c): the glyph rect
 * table in a synthetic pak (known offset and scan) and in a synthetic game folder (XBoxWad.idx
 * + WAD), key cap labels, and the atlas painter on a synthetic atlas: only the bound button cells
 * change, a mouse button is red on its own side, a remap changes the cell, painting is
 * deterministic; every glyph code the title draws (incl. the Spanish/French/Italian letter-badge
 * variants of the triggers and stick clicks) -> its pad control -> the default key.
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
    CHECK(opaque_light > 20, "the E key cap has light rim and label texels (%d)", opaque_light);
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

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "/tmp";
    test_rects(tmp);
    test_labels();
    test_paint();
    test_codes();
    test_symbols();
    test_icon();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
