/* Text fixes in the title's string tables (see hle_textfix.h). */
#include "hle_textfix.h"
#include "input_map.h"
#include "kbm_glyphs.h"
#include <stdio.h>
#include <string.h>

/* Title fixes. Main.wad0 holds the menu string tables of every language. */
const TextFix textfix_table[] = {
    /* Spanish gym (the gang's hideout training): "Sit Ups" is translated "Sentadillas" (squats)
     * in both gym label lists, next to "Flexiones" (Push Ups). The file has no other occurrence. */
    {"sit ups", "Main.wad0", 5, "Sentadillas", "Abdominales", {268301935u, 268996894u}, 2, NULL},
};
const int textfix_count = (int)(sizeof textfix_table / sizeof textfix_table[0]);

int textfix_active;
static const TextFix *armed[TEXTFIX_MAX];
static int narmed;
/* Places already reported, per armed fix: bit k for offsets[k], bit 0 for a scan entry. */
static unsigned reported[TEXTFIX_MAX], refused[TEXTFIX_MAX];
static int gate_state = -1;   /* the gated fixes' answer: -1 not asked yet, 0 off, 1 on */

static void put(char *report, size_t cap, size_t *len, const char *s) {
    int n = snprintf(report + *len, cap - *len, "%s", s);
    if (n > 0) *len += (size_t)n < cap - *len ? (size_t)n : cap - *len - 1;
}

static int fix_valid(const TextFix *f) {
    size_t n = strlen(f->old_text);
    return n && n <= TEXTFIX_LEN_MAX && strlen(f->new_text) == n && f->noffsets <= TEXTFIX_OFFSETS_MAX && f->file[0];
}

int textfix_arm(const TextFix *t, int n, int enabled, uint32_t language, char *report, size_t cap) {
    size_t len = 0;
    narmed = 0;
    gate_state = -1;
    memset(reported, 0, sizeof reported);
    memset(refused, 0, sizeof refused);
    char done[384] = "", invalid[256] = "";
    size_t dlen = 0, ilen = 0;
    for (int i = 0; enabled && i < n; i++) {
        char item[96];
        if (!fix_valid(&t[i])) {
            snprintf(item, sizeof item, "%s%s", ilen ? ", " : "", t[i].name);
            put(invalid, sizeof invalid, &ilen, item);
            continue;
        }
        if (t[i].language != language || narmed == TEXTFIX_MAX) continue;
        armed[narmed++] = &t[i];
        if (t[i].noffsets)
            snprintf(item, sizeof item, "%s%s (%s, %d offset%s)", dlen ? ", " : "", t[i].name, t[i].file,
                     t[i].noffsets, t[i].noffsets == 1 ? "" : "s");
        else
            snprintf(item, sizeof item, "%s%s (%s, every read)", dlen ? ", " : "", t[i].name, t[i].file);
        put(done, sizeof done, &dlen, item);
    }
    textfix_active = narmed > 0;
    if (!cap) return narmed;
    report[0] = 0;
    if (!enabled) {
        put(report, cap, &len, "[textfix] disabled (--no-text-fixes): string tables read unchanged");
        return 0;
    }
    if (narmed) {
        put(report, cap, &len, "[textfix] armed: ");
        put(report, cap, &len, done);
    } else {
        char item[64];
        snprintf(item, sizeof item, "[textfix] none for XC_LANGUAGE %u", language);
        put(report, cap, &len, item);
    }
    if (ilen) {
        put(report, cap, &len, "; invalid: ");
        put(report, cap, &len, invalid);
    }
    return narmed;
}

/* Base name of a host or Xbox path ('/' or '\\'), compared without case. */
static int same_file(const char *path, const char *name) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    for (; *b && *name; b++, name++) {
        char x = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b, y = *name >= 'A' && *name <= 'Z' ? *name + 32 : *name;
        if (x != y) return 0;
    }
    return !*b && !*name;
}

/* The file bytes around one read: up to TEXTFIX_LEN_MAX-1 before it and after it, fetched on first
 * use. byte_at() returns -1 past the end of the file or when the file cannot be read. */
typedef struct {
    uint64_t off;
    const uint8_t *buf;
    size_t len;
    TextFixReadAt rd;
    void *ctx;
    uint8_t pre[TEXTFIX_LEN_MAX - 1], suf[TEXTFIX_LEN_MAX - 1];
    long npre, nsuf;   /* -2: not fetched yet */
    uint64_t pre_at;
} View;

static int byte_at(View *v, uint64_t pos) {
    if (pos >= v->off && pos - v->off < v->len) return v->buf[pos - v->off];
    if (pos < v->off) {
        if (v->npre == -2) {
            v->pre_at = v->off > sizeof v->pre ? v->off - sizeof v->pre : 0;
            v->npre = v->rd ? v->rd(v->ctx, v->pre_at, v->pre, (size_t)(v->off - v->pre_at)) : -1;
        }
        return pos >= v->pre_at && (long)(pos - v->pre_at) < v->npre ? v->pre[pos - v->pre_at] : -1;
    }
    if (v->nsuf == -2) v->nsuf = v->rd ? v->rd(v->ctx, v->off + v->len, v->suf, sizeof v->suf) : -1;
    uint64_t i = pos - v->off - v->len;
    return (long)i < v->nsuf ? v->suf[i] : -1;
}

/* The old bytes are at file position p; then their part inside the read becomes the new bytes
 * (unless the fix is gated and its gate says no: then 0, as for a miss). */
static int inside(const View *v, uint64_t pos) { return pos >= v->off && pos - v->off < v->len; }
static int try_at(View *v, const TextFix *f, size_t n, uint64_t p, uint8_t *buf) {
    for (size_t i = 0; i < n; i++)   /* the bytes in the read first: no file access on a miss */
        if (inside(v, p + i) && v->buf[p + i - v->off] != (uint8_t)f->old_text[i]) return 0;
    for (size_t i = 0; i < n; i++)
        if (!inside(v, p + i) && byte_at(v, p + i) != (uint8_t)f->old_text[i]) return 0;
    if (f->gate) {
        if (gate_state < 0) {
            gate_state = f->gate() ? 1 : 0;
            printf("[textfix] control names: %s (decided at the first stick phrase read)\n",
                   gate_state ? "keyboard/mouse" : "kept (a controller is connected or was used last)");
        }
        if (!gate_state) return 0;
    }
    for (size_t i = 0; i < n; i++)
        if (inside(v, p + i)) buf[p + i - v->off] = (uint8_t)f->new_text[i];
    return 1;
}

int textfix_apply(const char *path, uint64_t off, uint8_t *buf, size_t len, TextFixReadAt rd, void *ctx) {
    int count = 0;
    for (int a = 0; a < narmed && len; a++) {
        const TextFix *f = armed[a];
        if (!same_file(path, f->file)) continue;
        size_t n = strlen(f->old_text);
        View v = {.off = off, .buf = buf, .len = len, .rd = rd, .ctx = ctx, .npre = -2, .nsuf = -2};
        uint64_t lo = off > n - 1 ? off - (n - 1) : 0, hi = off + len;   /* match starts that touch the read */
        if (f->noffsets) {
            for (int k = 0; k < f->noffsets; k++) {
                uint64_t p = f->offsets[k];
                if (p < lo || p >= hi) continue;
                if (try_at(&v, f, n, p, buf)) {
                    count++;
                    if (!(reported[a] & 1u << k))
                        printf("[textfix] %s: %s+%llu '%s' -> '%s'\n", f->name, f->file, (unsigned long long)p,
                               f->old_text, f->new_text);
                    reported[a] |= 1u << k;
                } else if (!(refused[a] & 1u << k)) {
                    printf("[textfix] %s: %s+%llu does not hold '%s', left unchanged\n", f->name, f->file,
                           (unsigned long long)p, f->old_text);
                    refused[a] |= 1u << k;
                }
            }
            continue;
        }
        for (uint64_t p = lo; p < hi; p++) {
            if (p >= off && p + n <= hi) {   /* fully inside: skip ahead to the next first byte */
                const uint8_t *q = memchr(buf + (p - off), (uint8_t)f->old_text[0], (size_t)(hi - n + 1 - p));
                if (!q) { p = hi - n; continue; }
                p = off + (uint64_t)(q - buf);
            }
            if (try_at(&v, f, n, p, buf)) {
                count++;
                if (!reported[a])
                    printf("[textfix] %s: %s+%llu '%s' -> '%s' (first match; all are replaced)\n", f->name, f->file,
                           (unsigned long long)p, f->old_text, f->new_text);
                reported[a] = 1;
            }
        }
    }
    return count;
}

int textfix_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--no-text-fixes")) { *enabled = 0; return 1; }
    if (!strcmp(arg, "--text-fixes")) { *enabled = 1; return 1; }
    return 0;
}

/* ---- Keyboard/mouse control names ---- */

int textfix_pad(char *dst, size_t n, const char *text) {
    size_t len = strlen(text);
    if (len > n) return 0;
    memcpy(dst, text, len);
    if (n - len == 1) dst[len++] = ' ';
    else if (n > len) {
        memset(dst + len, '~', n - len);
        dst[len] = '<';
        dst[n - 1] = '>';
    }
    dst[n] = 0;
    return 1;
}

/* Nouns per language (XC_LANGUAGE 1 en, 3 de, 4 fr, 5 es, 6 it), Latin-1 like the title's text,
 * chosen for the article the sentences put before the stick phrase: the movement keys as
 * move_fmt with the keys (the noun alone when they do not fit), the right stick as look, the click
 * places as click_fmt with the stick-click key. */
typedef struct { uint32_t language; const char *noun, *move_fmt, *arrows, *look, *click_fmt; int compound; } Words;
static const Words words[] = {
    {1, "keyboard", "keyboard (%s)", "arrows", "mouse", "%s key", 0},             /* the ... */
    {3, "Tastenblock", "%s-Block", NULL, "Mauszeiger", "%s-Taste dr\xfc" "cken", 1},   /* den/dem/der ... */
    {4, "clavier", "clavier (%s)", "fl\xe8" "ches", "mouvement de la souris", "%s", 0},  /* le/du ... */
    {5, "teclado", "teclado (%s)", "flechas", "mouse", "%s", 0},                   /* el ... */
    {6, "tastiera", "tastiera (%s)", "frecce", "mano sul mouse", "%s", 0},         /* la ... */
};
static const Words *words_for(uint32_t language) {
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (words[i].language == language) return &words[i];
    return NULL;
}

static const char *fit(const char *s, size_t n, char *buf, size_t cap) {
    if (strlen(s) > n) return NULL;
    snprintf(buf, cap, "%s", s);
    return buf;
}

const char *textfix_move_text(uint32_t language, const int keys[4], size_t n, char *buf, size_t cap) {
    const Words *w = words_for(language);
    if (!w) w = &words[0];
    static const int arrows[4] = {INPUT_KEY_UP, INPUT_KEY_LEFT, INPUT_KEY_DOWN, INPUT_KEY_RIGHT};
    int all_arrows = 1, ok = 1;
    for (int i = 0; i < 4; i++) {
        all_arrows &= keys[i] == arrows[i];
        const char *l = kbm_key_label(keys[i], 0);
        ok &= l && (unsigned char)l[0] >= ' ';   /* bound to a key with a printable label */
    }
    char k[64], t[96];
    if (all_arrows && w->arrows) {
        snprintf(t, sizeof t, w->move_fmt, w->arrows);
        if (fit(t, n, buf, cap)) return buf;
    }
    for (int form = 0; ok && !all_arrows && form < 4; form++) {   /* full, full unspaced, short, short unspaced */
        int spaced = !(form & 1) && !w->compound;
        if (!(form & 1) && w->compound) continue;
        size_t len = 0;
        for (int i = 0; i < 4; i++)
            len += (size_t)snprintf(k + len, sizeof k - len, "%s%s", i && spaced ? " " : "", kbm_key_label(keys[i], form >= 2));
        snprintf(t, sizeof t, w->move_fmt, k);
        if (fit(t, n, buf, cap)) return buf;
    }
    if (fit(w->noun, n, buf, cap)) return buf;
    buf[0] = 0;
    return buf;
}

/* The stick phrases as the Xbox branch of the scripts words them (one constant per script). The
 * left stick's constant is shared by every sentence of a script; in the scripts where most of its
 * sentences say to click it (the gang command tips: level31, level34, level80, and the English
 * command help, whose sentence has its own constant), the click places come first, at their
 * offsets in Main.wad0, and the scan entry covers the rest. Spanish "hacer clic en el" (only in
 * those sentences) becomes a verb for a key. */
enum { PH_CLICK, PH_MOVE, PH_LOOK, PH_VERB };
static const struct {
    uint32_t language;
    int role;
    const char *phrase, *fixed;
    uint64_t at[TEXTFIX_OFFSETS_MAX];
    uint8_t nat;
} phrases[] = {
    {1, PH_CLICK, "left thumbstick", NULL, {25060472u, 245102971u, 246055320u, 255376901u}, 4},
    {1, PH_MOVE, "left thumbstick", NULL, {0}, 0}, {1, PH_LOOK, "right thumbstick", NULL, {0}, 0},
    {3, PH_CLICK, "Linken Ministick klicken", NULL, {0}, 0}, {3, PH_CLICK, "linken Ministick klicken", NULL, {0}, 0},
    {3, PH_MOVE, "Ministick links", NULL, {0}, 0}, {3, PH_LOOK, "Ministick rechts", NULL, {0}, 0},
    {4, PH_CLICK, "stick analogique gauche", NULL, {245107757u, 246059986u, 255382517u}, 3},
    {4, PH_MOVE, "stick analogique gauche", NULL, {0}, 0}, {4, PH_LOOK, "stick analogique droit", NULL, {0}, 0},
    {5, PH_VERB, "hacer clic en el", "oprimir la tecla", {245106168u, 246058439u, 255378801u, 255379041u}, 4},
    {5, PH_CLICK, "stick anal\xf3gico izquierdo", NULL, {245106190u, 246058461u, 255378823u}, 3},
    {5, PH_MOVE, "stick anal\xf3gico izquierdo", NULL, {0}, 0}, {5, PH_LOOK, "stick anal\xf3gico derecho", NULL, {0}, 0},
    {6, PH_CLICK, "levetta sinistra", NULL, {245109251u, 246061340u, 255384182u}, 3},
    {6, PH_MOVE, "levetta sinistra", NULL, {0}, 0}, {6, PH_LOOK, "levetta destra", NULL, {0}, 0},
};

int textfix_controls_build(uint32_t language, const int move_keys[4], int click_key, int (*gate)(void), TextFix *out, int max) {
    static char texts[TEXTFIX_CONTROLS_MAX][TEXTFIX_LEN_MAX + 1];
    static const char *const names[] = {"left stick click -> key", "left stick -> keyboard", "right stick -> mouse",
                                        "click verb -> key verb"};
    const Words *w = words_for(language);
    const char *click_label = kbm_key_label(click_key, 0);
    int has_click = click_label && (unsigned char)click_label[0] >= ' ';
    int count = 0;
    for (size_t i = 0; w && i < sizeof phrases / sizeof *phrases && count < max && count < TEXTFIX_CONTROLS_MAX; i++) {
        if (phrases[i].language != language) continue;
        size_t n = strlen(phrases[i].phrase);
        char t[96];
        const char *text = t;
        if (phrases[i].role == PH_MOVE) text = textfix_move_text(language, move_keys, n, t, sizeof t);
        else if (phrases[i].role == PH_LOOK) text = w->look;
        else if (!has_click) continue;
        else if (phrases[i].role == PH_VERB) text = phrases[i].fixed;
        else {
            snprintf(t, sizeof t, w->click_fmt, click_label);
            if (strlen(t) > n) snprintf(t, sizeof t, w->click_fmt, kbm_key_label(click_key, 1));
        }
        if (!text[0] || !textfix_pad(texts[count], n, text)) continue;
        TextFix f = {names[phrases[i].role], "Main.wad0", language, phrases[i].phrase, texts[count], {0}, phrases[i].nat, gate};
        memcpy(f.offsets, phrases[i].at, sizeof f.offsets);
        out[count++] = f;
    }
    return count;
}

int textfix_compose(int enabled, uint32_t language, int icons, const int move_keys[4], int click_key, int (*auto_gate)(void),
                    TextFix *out, int max) {
    int n = 0;
    for (int i = 0; enabled && i < textfix_count && n < max; i++)
        if (textfix_table[i].language == language) out[n++] = textfix_table[i];
    if (icons == TEXTFIX_ICONS_KBM || icons == TEXTFIX_ICONS_AUTO)
        n += textfix_controls_build(language, move_keys, click_key, icons == TEXTFIX_ICONS_AUTO ? auto_gate : NULL, out + n, max - n);
    return n;
}

int hle_textfix_install(int enabled, uint32_t language, int icons, const int move_keys[4], int click_key, int (*auto_gate)(void)) {
    static TextFix fixes[TEXTFIX_MAX];
    char report[640];
    int n = textfix_compose(enabled, language, icons, move_keys, click_key, auto_gate, fixes, TEXTFIX_MAX);
    if (!enabled && n) {   /* the control names are not a text fix: --no-text-fixes leaves them on */
        n = textfix_arm(fixes, n, 1, language, report, sizeof report);
        printf("[textfix] disabled (--no-text-fixes); %s\n", report + 10);
        return n;
    }
    n = textfix_arm(fixes, n, enabled, language, report, sizeof report);
    printf("%s\n", report);
    return n;
}
