/* A small list menu for mods (docs/MODDING.md "Menus and music"): the pure
 * parts. The state machine (navigation with auto-repeat and wrap, a debounced "hover" event so a
 * preview starts only once the selection rests, choose / cancel that wait for the buttons that
 * opened the menu to be released), the level fades of the music channel and the layout of the
 * panel as textured quads (a dimmed screen, a panel, an optional cover image, the title, the
 * scrolling list with its highlight and a footer line) in the guest's back-buffer pixels.
 *
 * Game-independent and not thread-safe (tested by tests/modmenu_test.c). The runtime glue lives in
 * src/hle_modmenu.c, the drawing in src/hle_d3d.c (hle_d3d_modmenu_draw). */
#ifndef MODMENU_H
#define MODMENU_H
#include <stdint.h>

#define MODMENU_ITEMS_MAX 64
#define MODMENU_TEXT_MAX 96       /* bytes per line, NUL included */
#define MODMENU_ROWS 9            /* list rows shown at once */
#define MODMENU_DEBOUNCE_MS 150.0 /* the selection rests this long before a hover event */
#define MODMENU_REPEAT_DELAY_MS 380.0
#define MODMENU_REPEAT_MS 110.0

#define MODMENU_VALUE_ROWS 10     /* rows shown at once in a value menu */
#define MODMENU_TABS_MAX 8
#define MODMENU_VALUE_MAX 40      /* bytes of a value, a badge or a tab name, NUL included */
#define MODMENU_NOTE_MAX 320      /* the note under the list: up to 3 wrapped lines */

/* Menu controls (modmenu_controls): bits of one input sample. */
enum {
    MM_UP = 1, MM_DOWN = 2, MM_LEFT = 4, MM_RIGHT = 8,   /* left / right: a page up / down (a value menu: adjust) */
    MM_OK = 16, MM_BACK = 32,
    MM_TAB_PREV = 64, MM_TAB_NEXT = 128                  /* only menus with tabs use them */
};
/* Events of modmenu_step. TAB: the tab changed (index = the new tab); ADJUST: left / right on a
 * value row (index = the row, ModMenu.adjust = -1 / +1); CONFIRM: the confirm dialog was answered
 * (index 1 = yes, 0 = no / BACK). */
enum { MM_EV_NONE, MM_EV_HOVER, MM_EV_CHOOSE, MM_EV_CANCEL, MM_EV_TAB, MM_EV_ADJUST, MM_EV_CONFIRM };

typedef struct {
    int open;
    char title[MODMENU_TEXT_MAX], footer[MODMENU_TEXT_MAX];
    int n;
    char item[MODMENU_ITEMS_MAX][MODMENU_TEXT_MAX];
    int sel, top;
    unsigned held;          /* controls of the previous sample */
    int armed;              /* 0 until OK and BACK were seen released after opening */
    double moved_ms;        /* when the selection last changed (or the menu opened) */
    double repeat_ms;       /* next auto-repeat step of a held direction */
    int hovered;            /* index of the last hover event, -1 none */
    /* A value menu (modmenu_values; the settings menu): every row shows a value column and a badge,
     * left / right adjust the selected row instead of paging, OK reports CHOOSE and keeps the menu
     * open, greyed rows report neither; note is the selected row's text under the list. */
    int values;
    char value[MODMENU_ITEMS_MAX][MODMENU_VALUE_MAX];
    char badge[MODMENU_ITEMS_MAX][MODMENU_VALUE_MAX];
    unsigned char grey[MODMENU_ITEMS_MAX];
    char note[MODMENU_NOTE_MAX];
    int adjust;             /* -1 / +1 of the last MM_EV_ADJUST */
    /* Tabs (modmenu_tabs): a strip under the title; MM_TAB_PREV / MM_TAB_NEXT switch (wrapping). */
    int ntabs, tab;
    char tabs[MODMENU_TABS_MAX][MODMENU_VALUE_MAX];
    /* The confirm dialog (modmenu_confirm): over the menu until answered. */
    int confirm_open, confirm_sel;   /* confirm_sel 0 = yes, 1 = no */
    char confirm[MODMENU_TEXT_MAX], confirm_yes[MODMENU_VALUE_MAX], confirm_no[MODMENU_VALUE_MAX];
    /* Photo mode (src/hle_photo.h): side_panel puts a value menu in a narrow panel at the left edge
     * over the undimmed game; hidden draws no panel at all (only what follows); bars (a fraction of the
     * height) draws black letterbox bars that high at the top and bottom first; progress (0..1, 0 none) a progress
     * bar at the bottom centre; toast (empty: none) a one-line hint in a small box at the bottom centre,
     * drawn even when hidden, at toast_alpha opacity (0..1). All 0 by default (modmenu_open clears them). */
    int side_panel, hidden;
    float bars, progress;
    char toast[MODMENU_TEXT_MAX];
    float toast_alpha;
} ModMenu;

/* Opens the menu: items is one line per item ('\n' separated; at most MODMENU_ITEMS_MAX, longer
 * lines cut at a character boundary); sel the selected item (clamped). Returns the item count. */
int modmenu_open(ModMenu *m, const char *title, const char *items, const char *footer, int sel, double now_ms);
void modmenu_close(ModMenu *m);
/* Value menu mode (see ModMenu.values); call after modmenu_open. */
void modmenu_values(ModMenu *m, int on);
/* Tabs: one name per line ('\n'), tab the selected one (clamped). */
void modmenu_tabs(ModMenu *m, const char *tabs, int tab);
/* Replaces the items of an open menu (values, badges and greyed flags cleared) and selects sel
 * (clamped); the buttons held stay held. Returns the item count (0 leaves the old items). */
int modmenu_set_items(ModMenu *m, const char *items, int sel);
/* Opens the confirm dialog: text, the yes and no labels; "yes" is selected. */
void modmenu_confirm(ModMenu *m, const char *text, const char *yes, const char *no);
/* One input sample (MM_* bits) at now_ms. Returns an MM_EV_* and the item in *index: HOVER once
 * the selection rested MODMENU_DEBOUNCE_MS on an item not hovered before (also the first item
 * after opening); CHOOSE on a new OK press, CANCEL on a new BACK press (both close the menu).
 * Up / down move by one (wrapping), left / right by a page (clamped); a held direction repeats
 * after MODMENU_REPEAT_DELAY_MS every MODMENU_REPEAT_MS. */
int modmenu_step(ModMenu *m, unsigned controls, double now_ms, int *index);

/* The rows a menu shows at once (MODMENU_ROWS, MODMENU_VALUE_ROWS in a value menu). */
int modmenu_rows(const ModMenu *m);
/* Controls of a pad sample: d-pad or left stick past half deflection for the directions, A, Y or
 * Start (analog > 0x40 / bit) for OK, B or Back for BACK. buttons: XINPUT wButtons, analog: the 8
 * analog buttons (A B X Y black white LT RT), ly / lx the left stick (+ = up / right). */
unsigned modmenu_controls(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);

/* A level that moves linearly to a target over a duration (the music channel's fades). */
typedef struct {
    float from, to;
    double t0_ms, dur_ms;
} ModFade;
void modfade_init(ModFade *f, float level);
void modfade_set(ModFade *f, float to, double dur_ms, double now_ms);   /* from the current level */
float modfade_level(const ModFade *f, double now_ms);
int modfade_done(const ModFade *f, double now_ms);
/* Equal-power shape of a 0..1 fade level (sin(x * pi / 2)): two crossfading voices keep their
 * summed power. */
float modfade_gain(float level);

/* ---- layout ---- */
/* A font: glyph rects (normalized UVs) of character codes first..first + n - 1 in an atlas of
 * atlas_w x atlas_h texels. */
typedef struct {
    const float (*rect)[4];   /* u0 v0 u1 v1 */
    int first, n;
    int atlas_w, atlas_h;
} ModFont;
enum { MM_TEX_WHITE, MM_TEX_FONT, MM_TEX_COVER };
typedef struct {
    float x0, y0, x1, y1;     /* guest back-buffer pixels, y down */
    float u0, v0, u1, v1;
    uint32_t color;           /* ARGB */
    int tex;                  /* MM_TEX_* */
} ModQuad;
#define MODMENU_QUADS_MAX 4096
/* The quads of the open menu on a w x h back buffer whose pixels are shown aspect times wider
 * than tall (1 = square pixels; the anamorphic 16:9 output of a 640x480 buffer: 4/3 * 9/16 ...).
 * cover: draw the cover panel (uv rect cover_uv). Returns the number written (at most max). */
int modmenu_layout(const ModMenu *m, int w, int h, float aspect, const ModFont *font, int cover, const float cover_uv[4],
                   ModQuad *out, int max);
/* Width in pixels of text at glyph height px with font f (characters without a glyph count as a
 * space of px / 3). */
float modmenu_text_width(const ModFont *f, const char *text, float px);
#endif
