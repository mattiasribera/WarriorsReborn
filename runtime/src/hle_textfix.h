/* Text fixes (on by default, --no-text-fixes turns them off): known mistranslations in the
 * title's string tables, corrected in the data the title reads from its files. The files on disk
 * are never changed.
 *
 * Each fix names a file (base name, any directory, case-insensitive), the console language it is
 * for (XC_LANGUAGE value), the exact old bytes and new bytes of the same length, and optionally
 * the absolute file offsets where the old bytes are expected. With offsets only those places are
 * checked and replaced; without them every read of the file is scanned. A read that covers only
 * part of a match gets its part of the new bytes: the bytes of the match outside the read are
 * read from the file to check them. Bytes that differ from the old bytes are left as read.
 *
 * Keyboard/mouse control names (with --button-icons kbm or auto): the tutorial and HUD texts name
 * the sticks in words ("left thumbstick"), not glyphs. Those phrases (the Xbox wording of each
 * language) become nouns that fit the article before them: the left stick the keyboard with the
 * key map's movement keys ("keyboard (WASD)", "teclado (W A S D)"), the right stick the mouse,
 * and in the command tips that say to click the left stick, its key ("Click the C key"). A Lua
 * string constant cannot change length, so a shorter text is padded with an unknown tag such as
 * "<~~>", which the title's text renderer (0xe2580) skips without drawing anything; one byte short
 * is padded with a space; when the keys do not fit, the noun alone. One constant serves every
 * sentence of a script, so the click places are chosen per script (offsets in Main.wad0) by the
 * majority of its sentences; the minority sentences keep the other text. */
#ifndef HLE_TEXTFIX_H
#define HLE_TEXTFIX_H
#include <stddef.h>
#include <stdint.h>

#define TEXTFIX_LEN_MAX 32
#define TEXTFIX_OFFSETS_MAX 4
#define TEXTFIX_MAX 16

typedef struct {
    const char *name;
    const char *file;                        /* base name, e.g. "Main.wad0" */
    uint32_t language;                       /* XC_LANGUAGE value (5 = Spanish) */
    const char *old_text, *new_text;         /* same length, 1..TEXTFIX_LEN_MAX bytes */
    uint64_t offsets[TEXTFIX_OFFSETS_MAX];   /* expected absolute offsets of old_text */
    uint8_t noffsets;                        /* 0: scan every read of the file */
    int (*gate)(void);                       /* NULL: always; else asked once, at the first match of a
                                                gated fix after textfix_arm, and its answer kept: 0
                                                leaves the text as read */
} TextFix;

extern const TextFix textfix_table[];
extern const int textfix_count;

/* Nonzero while at least one fix is armed (the read hook does nothing otherwise). */
extern int textfix_active;

/* Reads len bytes of the file at off into dst; returns the count (short at the end) or < 0. */
typedef long (*TextFixReadAt)(void *ctx, uint64_t off, void *dst, size_t len);

/* Arms the valid fixes of t[0..n) for language (none when !enabled); t must outlive the arming.
 * Writes "[textfix] ..." (one line, no newline) into report. Returns the number armed. */
int textfix_arm(const TextFix *t, int n, int enabled, uint32_t language, char *report, size_t cap);

/* Applies the armed fixes to buf, the len bytes of the file at path read at absolute offset off.
 * Bytes of a match outside buf are fetched with rd(ctx, ...). Returns the number of matches
 * (whole or partial) written. Logs the first application of each place once. */
int textfix_apply(const char *path, uint64_t off, uint8_t *buf, size_t len, TextFixReadAt rd, void *ctx);

/* text padded to exactly n bytes into dst (n + 1 bytes): as is, + ' ' when one short, + "<~..~>"
 * when shorter. 0 when text is longer than n. */
int textfix_pad(char *dst, size_t n, const char *text);

/* The left stick for language (XC_LANGUAGE) in at most n bytes: the keyboard noun with the movement
 * keys keys[4] (forward, left, back, right; input_map.h key codes, -1 unbound) as key cap labels
 * (spaced, unspaced, short forms) or the arrows word, e.g. "keyboard (WASD)", "WASD-Block"; the noun
 * alone when a direction is unbound, an arrow is among other keys or nothing fits; "" when even
 * the noun does not fit. */
const char *textfix_move_text(uint32_t language, const int keys[4], size_t n, char *buf, size_t cap);

#define TEXTFIX_CONTROLS_MAX 8
/* The control-name fixes of language into out (static texts, rebuilt by each call), with gate:
 * the left-stick phrase -> textfix_move_text, the right-stick phrase -> the mouse noun, the click
 * places -> click_key's label (none, and no click verb, when click_key < 0). Returns the count. */
int textfix_controls_build(uint32_t language, const int move_keys[4], int click_key, int (*gate)(void), TextFix *out, int max);

enum { TEXTFIX_ICONS_XBOX, TEXTFIX_ICONS_KBM, TEXTFIX_ICONS_AUTO };
/* The fixes to arm: the title table (when enabled), then the control names for kbm icons (no gate)
 * or auto icons (through auto_gate); none for Xbox icons. Returns the count written to out. */
int textfix_compose(int enabled, uint32_t language, int icons, const int move_keys[4], int click_key, int (*auto_gate)(void),
                    TextFix *out, int max);

/* 1 when arg is a text-fix option (sets *enabled), 0 otherwise. */
int textfix_parse_arg(const char *arg, int *enabled);

/* Arms textfix_compose(...) for the console language and prints the [textfix] line. */
int hle_textfix_install(int enabled, uint32_t language, int icons, const int move_keys[4], int click_key, int (*auto_gate)(void));
#endif
