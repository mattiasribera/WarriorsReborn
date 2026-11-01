/* Text-fix test. Without arguments it is game-independent: a host buffer stands in for a file and
 * fake fixes are armed. It checks a match fully inside one read, a match straddling two reads (and
 * a read inside the match), no match, bytes that differ at an expected offset, a wrong language,
 * the kill switch, a scan-mode entry (no expected offsets) and the title's table entry.
 *
 * With a path to the title's Main.wad0 it also reads the real file through textfix_apply in chunks
 * of several sizes (Spanish armed) and checks that both expected offsets read "Abdominales" and
 * every other byte equals the file; with English armed every byte equals the file.
 *
 * The keyboard/mouse control names (textfix_controls_build): the padding, the movement-keys text
 * from a key map (letters, arrow keys, an unbound direction, long labels), the stick phrases of
 * every language, the icon-style gating (Xbox: none, kbm: always, auto: asked once) and the
 * replacement of a padded phrase in a fake file. */
#include "../src/hle_textfix.h"
#include "../src/input_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define XC_ENGLISH 1u
#define XC_SPANISH 5u

/* The fake file: a host buffer and a reader over it. */
typedef struct { const uint8_t *data; size_t size; int reads; } MemFile;
static long mem_read_at(void *ctx, uint64_t off, void *dst, size_t len) {
    MemFile *m = ctx;
    m->reads++;
    if (off >= m->size) return 0;
    if (len > m->size - off) len = (size_t)(m->size - off);
    memcpy(dst, m->data + off, len);
    return (long)len;
}

/* Reads [off, off+len) of m into out (a copy, as NtReadFile would) and applies the armed fixes. */
static int read_fixed(const char *path, MemFile *m, uint64_t off, size_t len, uint8_t *out) {
    memcpy(out, m->data + off, len);
    return textfix_apply(path, off, out, len, mem_read_at, m);
}

/* Reads the whole fake file in chunks of `chunk` bytes into out. */
static void read_all(const char *path, MemFile *m, size_t chunk, uint8_t *out) {
    for (size_t off = 0; off < m->size; off += chunk)
        read_fixed(path, m, off, off + chunk > m->size ? m->size - off : chunk, out + off);
}

/* Only [o, o+n) of a differs from b (n bytes equal to want there); the rest is identical. */
static int only_changed(const uint8_t *a, const uint8_t *b, size_t size, const uint64_t *o, int no, const char *want) {
    size_t n = strlen(want);
    for (size_t i = 0; i < size; i++) {
        int inside = -1;
        for (int k = 0; k < no; k++)
            if (i >= o[k] && i < o[k] + n) inside = (int)(i - o[k]);
        if (inside >= 0 ? a[i] != (uint8_t)want[inside] : a[i] != b[i]) return 0;
    }
    return 1;
}

/* The real file: a reader that seeks the stdio stream (the caller seeks back after each read). */
typedef struct { FILE *f; int reads; } FileReader;
static long file_read_at(void *ctx, uint64_t off, void *dst, size_t len) {
    FileReader *r = ctx;
    r->reads++;
    if (fseeko(r->f, (off_t)off, SEEK_SET)) return -1;
    return (long)fread(dst, 1, len, r->f);
}

static int real_file_check(const char *path) {
    FILE *fi = fopen(path, "rb");
    if (!fi) { perror(path); return 1; }
    char report[512];
    textfix_arm(textfix_table, textfix_count, 1, XC_SPANISH, report, sizeof report);
    printf("%s\n", report);
    static const uint64_t want_at[] = {268301935u, 268996894u};
    enum { BUF = 1 << 20 };
    uint8_t *raw = malloc(BUF), *got = malloc(BUF);
    static const size_t chunks[] = {4093, 65536, 1 << 20};
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lang = pass ? XC_ENGLISH : XC_SPANISH;
        textfix_arm(textfix_table, textfix_count, 1, lang, report, sizeof report);
        for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            size_t chunk = chunks[c];
            uint64_t off = 0, diffs = 0, bad = 0;
            int fixed[2] = {0, 0};
            for (;;) {
                size_t n = fread(raw, 1, chunk, fi);
                if (!n) break;
                memcpy(got, raw, n);
                FileReader fr = {fi, 0};
                textfix_apply("D:\\Main.wad0", off, got, n, file_read_at, &fr);
                fseeko(fi, (off_t)(off + n), SEEK_SET);
                for (size_t i = 0; i < n; i++) {
                    uint64_t pos = off + i;
                    int k = pos >= want_at[0] && pos < want_at[0] + 11 ? 0 : pos >= want_at[1] && pos < want_at[1] + 11 ? 1 : -1;
                    diffs += got[i] != raw[i];
                    if (k >= 0 && pass == 0) fixed[k] += got[i] == (uint8_t)"Abdominales"[pos - want_at[k]];
                    else bad += got[i] != raw[i];
                }
                off += n;
            }
            rewind(fi);
            if (pass == 0)
                CHECK(bad == 0 && fixed[0] == 11 && fixed[1] == 11 && off == 304005120u,
                      "real Main.wad0, es, %zu-byte reads: %llu bytes read, both offsets 'Abdominales', "
                      "%llu bytes changed, %llu outside the matches", chunk, (unsigned long long)off,
                      (unsigned long long)diffs, (unsigned long long)bad);
            else
                CHECK(diffs == 0, "real Main.wad0, en, %zu-byte reads: byte-identical (%llu changed)", chunk,
                      (unsigned long long)diffs);
        }
    }
    /* Reads that start, end or lie inside a match. */
    textfix_arm(textfix_table, textfix_count, 1, XC_SPANISH, report, sizeof report);
    for (int k = 0; k < 2; k++) {
        static const int starts[] = {-5, 3, 7, 0};
        static const size_t lens[] = {9, 3, 10, 11};
        for (int s = 0; s < 4; s++) {
            uint64_t off = want_at[k] + starts[s];
            fseeko(fi, (off_t)off, SEEK_SET);
            size_t n = fread(got, 1, lens[s], fi);
            FileReader fr = {fi, 0};
            textfix_apply("D:\\Main.wad0", off, got, n, file_read_at, &fr);
            int ok = n == lens[s];
            for (size_t i = 0; i < n; i++) {
                int64_t at = starts[s] + (int64_t)i;
                if (at >= 0 && at < 11) ok &= got[i] == (uint8_t)"Abdominales"[at];
            }
            CHECK(ok, "real Main.wad0 offset %llu: %zu-byte read at %+d reads its part of 'Abdominales'",
                  (unsigned long long)want_at[k], lens[s], starts[s]);
        }
    }
    free(raw);
    free(got);
    fclose(fi);
    return 0;
}

/* ---- Keyboard/mouse control names ---- */

static int gate_calls, gate_answer;
static int fake_gate(void) { gate_calls++; return gate_answer; }

/* The armed controls fix whose old text is old (NULL: none). */
static const TextFix *find_fix(const TextFix *t, int n, const char *old) {
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].old_text, old)) return &t[i];
    return NULL;
}

static void test_controls(void) {
    char buf[64], pad[TEXTFIX_LEN_MAX + 1];
    /* Padding: exact, one short (a space), shorter (an unknown tag the title's text renderer skips), too long. */
    CHECK(textfix_pad(pad, 4, "WASD") && !strcmp(pad, "WASD"), "pad: exact length kept ('%s')", pad);
    CHECK(textfix_pad(pad, 5, "WASD") && !strcmp(pad, "WASD "), "pad: one short -> a space ('%s')", pad);
    CHECK(textfix_pad(pad, 6, "WASD") && !strcmp(pad, "WASD<>"), "pad: two short -> '<>' ('%s')", pad);
    CHECK(textfix_pad(pad, 15, "W A S D") && !strcmp(pad, "W A S D<~~~~~~>"), "pad: 'W A S D' in 15 ('%s')", pad);
    CHECK(!textfix_pad(pad, 3, "WASD"), "pad: longer than the phrase refused");

    /* The left stick as a noun that fits the sentence's article, with the keys in parentheses
     * (forward, left, back, right) when they fit, else the noun alone. */
    const int wasd[4] = {'w', 'a', 's', 'd'}, arrows[4] = {INPUT_KEY_UP, INPUT_KEY_LEFT, INPUT_KEY_DOWN, INPUT_KEY_RIGHT};
    const int unbound[4] = {'w', -1, 's', 'd'}, longk[4] = {INPUT_KEY_LSHIFT, INPUT_KEY_LCTRL, ' ', '\t'};
    const int mixed[4] = {INPUT_KEY_UP, 'a', 's', 'd'}, zqsd[4] = {'z', 'q', 's', 'd'};
    const struct { uint32_t lang; const int *keys; size_t n; const char *want; } mv[] = {
        {1, wasd, 15, "keyboard (WASD)"}, {1, wasd, 25, "keyboard (W A S D)"}, {1, arrows, 15, "keyboard"},
        {1, unbound, 15, "keyboard"}, {1, mixed, 15, "keyboard"}, {1, longk, 25, "keyboard (SHFCTLSPCTAB)"},
        {5, wasd, 25, "teclado (W A S D)"}, {5, arrows, 25, "teclado (flechas)"}, {5, unbound, 25, "teclado"},
        {4, zqsd, 23, "clavier (Z Q S D)"}, {4, arrows, 23, "clavier (fl\xe8" "ches)"},
        {3, wasd, 15, "WASD-Block"}, {3, arrows, 15, "Tastenblock"}, {3, longk, 15, "Tastenblock"},
        {6, wasd, 16, "tastiera (WASD)"}, {6, unbound, 16, "tastiera"}, {6, wasd, 5, ""},
    };
    for (size_t i = 0; i < sizeof mv / sizeof *mv; i++)
        CHECK(!strcmp(textfix_move_text(mv[i].lang, mv[i].keys, mv[i].n, buf, sizeof buf), mv[i].want),
              "move text XC %u in %zu: '%s' (want '%s')", mv[i].lang, mv[i].n, buf, mv[i].want);

    /* The phrases of every language: same length, the left stick -> the noun, the right stick -> a
     * noun that fits its article, the click-the-left-stick sentences -> the click key at their places. */
    static const struct { uint32_t lang; const char *left, *move, *right, *look, *click; } langs[] = {
        {1, "left thumbstick", "keyboard (WASD)", "right thumbstick", "mouse", "C key"},
        {5, "stick anal\xf3gico izquierdo", "teclado (W A S D)", "stick anal\xf3gico derecho", "mouse", "C"},
        {4, "stick analogique gauche", "clavier (W A S D)", "stick analogique droit", "mouvement de la souris", "C"},
        {3, "Ministick links", "WASD-Block", "Ministick rechts", "Mauszeiger", NULL},
        {6, "levetta sinistra", "tastiera (WASD)", "levetta destra", "mano sul mouse", "C"},
    };
    TextFix fx[TEXTFIX_CONTROLS_MAX];
    for (size_t l = 0; l < sizeof langs / sizeof *langs; l++) {
        int n = textfix_controls_build(langs[l].lang, wasd, 'c', NULL, fx, TEXTFIX_CONTROLS_MAX);
        const TextFix *mf = NULL, *lf = NULL, *cf = NULL;
        int same = 1, click_first = 1;
        for (int i = 0; i < n; i++) {
            same &= strlen(fx[i].old_text) == strlen(fx[i].new_text) && fx[i].language == langs[l].lang && !strcmp(fx[i].file, "Main.wad0");
            if (!strcmp(fx[i].old_text, langs[l].left)) {
                if (fx[i].noffsets) { cf = &fx[i]; click_first &= !mf; }
                else mf = &fx[i];
            }
            if (!strcmp(fx[i].old_text, langs[l].right)) lf = &fx[i];
        }
        char want[TEXTFIX_LEN_MAX + 1];
        int ok = same && mf && lf && textfix_pad(want, strlen(langs[l].left), langs[l].move) && !strcmp(mf->new_text, want) &&
                 textfix_pad(want, strlen(langs[l].right), langs[l].look) && !strcmp(lf->new_text, want) && !mf->noffsets && !lf->noffsets;
        if (langs[l].click) ok &= cf && click_first && cf->noffsets >= 3 && textfix_pad(want, strlen(langs[l].left), langs[l].click) && !strcmp(cf->new_text, want);
        CHECK(ok, "XC %u: %d fixes, same lengths, '%s' -> '%s', '%s' -> '%s', click places -> '%s'", langs[l].lang, n, langs[l].left,
              mf ? mf->new_text : "?", langs[l].right, lf ? lf->new_text : "?", cf ? cf->new_text : "-");
    }
    int n = textfix_controls_build(5, wasd, 'c', NULL, fx, TEXTFIX_CONTROLS_MAX);
    const TextFix *c = find_fix(fx, n, "hacer clic en el");
    CHECK(c && !strcmp(c->new_text, "oprimir la tecla") && c->noffsets == 4, "es: 'hacer clic en el' -> '%s' at the click sentences",
          c ? c->new_text : "?");
    n = textfix_controls_build(3, wasd, 'c', NULL, fx, TEXTFIX_CONTROLS_MAX);
    c = find_fix(fx, n, "Linken Ministick klicken");
    CHECK(c && !strncmp(c->new_text, "C-Taste dr\xfc" "cken<", 16) && find_fix(fx, n, "linken Ministick klicken"),
          "de: 'Linken/linken Ministick klicken' -> '%s'", c ? c->new_text : "?");
    n = textfix_controls_build(3, wasd, -1, NULL, fx, TEXTFIX_CONTROLS_MAX);
    CHECK(!find_fix(fx, n, "Linken Ministick klicken"), "de: no click key -> the click phrase is left as read");
    n = textfix_controls_build(5, wasd, -1, NULL, fx, TEXTFIX_CONTROLS_MAX);
    CHECK(n == 2 && !find_fix(fx, n, "hacer clic en el"), "es: no click key -> no click places, the sentences stay (%d)", n);
    CHECK(textfix_controls_build(2, wasd, 'c', NULL, fx, TEXTFIX_CONTROLS_MAX) == 0, "a language without phrases: none");

    /* Gating by the icon style: Xbox none, kbm always (no gate), auto through the gate. */
    TextFix all[TEXTFIX_MAX];
    CHECK(textfix_compose(1, 1, TEXTFIX_ICONS_XBOX, wasd, 'c', fake_gate, all, TEXTFIX_MAX) == 0, "Xbox icons, English: no fixes");
    n = textfix_compose(1, 1, TEXTFIX_ICONS_KBM, wasd, 'c', fake_gate, all, TEXTFIX_MAX);
    CHECK(n == 3 && !all[0].gate && !all[2].gate, "kbm icons, English: 3 fixes, not gated (%d)", n);
    n = textfix_compose(1, 1, TEXTFIX_ICONS_AUTO, wasd, 'c', fake_gate, all, TEXTFIX_MAX);
    CHECK(n == 3 && all[0].gate == fake_gate, "auto icons, English: 3 fixes through the gate (%d)", n);
    n = textfix_compose(1, 5, TEXTFIX_ICONS_KBM, wasd, 'c', fake_gate, all, TEXTFIX_MAX);
    CHECK(n == 5 && !strcmp(all[0].old_text, "Sentadillas"), "kbm icons, Spanish: the title fix first, then 4 (%d)", n);
    n = textfix_compose(0, 5, TEXTFIX_ICONS_KBM, wasd, 'c', fake_gate, all, TEXTFIX_MAX);
    CHECK(n == 4, "--no-text-fixes keeps the control names (%d)", n);

    /* Applied: a fake Main.wad0 with the English phrases, read in pieces (the click places are
     * offsets of the real file, so here every left-stick phrase is the movement noun). */
    static const char text[] = "....Move the <COLOR 505050FF>left thumbstick<COLOR B2B2B2FF> to aim....with the "
                               "right thumbstick.....";
    char exp[sizeof text];
    MemFile m = {(const uint8_t *)text, sizeof text - 1, 0};
    uint8_t got[sizeof text];
    char report[640];
    n = textfix_compose(1, 1, TEXTFIX_ICONS_KBM, wasd, 'c', NULL, all, TEXTFIX_MAX);
    textfix_arm(all, n, 1, 1, report, sizeof report);
    snprintf(exp, sizeof exp, "....Move the <COLOR 505050FF>keyboard (WASD)<COLOR B2B2B2FF> to aim....with the "
                              "mouse<~~~~~~~~~>.....");
    int ok = 1;
    for (size_t chunk = 1; chunk <= 40; chunk += 7) {
        memset(got, 0, sizeof got);
        read_all("D:\\Main.wad0", &m, chunk, got);
        ok &= !memcmp(got, exp, sizeof text - 1);
    }
    CHECK(ok, "kbm, English: every phrase replaced at every split (%.*s)", (int)(sizeof text - 1), (char *)got);
    CHECK(strstr(report, "left stick") != NULL, "report names the control phrases: '%s'", report);

    /* Auto: the gate is asked once, at the first match; 0 leaves the text as read. */
    n = textfix_compose(1, 1, TEXTFIX_ICONS_AUTO, wasd, 'c', fake_gate, all, TEXTFIX_MAX);
    textfix_arm(all, n, 1, 1, report, sizeof report);
    gate_calls = 0;
    gate_answer = 0;
    read_all("D:\\Main.wad0", &m, 1000, got);
    read_all("D:\\Main.wad0", &m, 9, got);
    CHECK(!memcmp(got, text, sizeof text - 1) && gate_calls == 1, "auto, controller: text as read, gate asked once (%d)", gate_calls);
    textfix_arm(all, n, 1, 1, report, sizeof report);
    gate_calls = 0;
    gate_answer = 1;
    read_all("D:\\Main.wad0", &m, 9, got);
    gate_answer = 0;
    read_all("D:\\Main.wad0", &m, 13, got);
    CHECK(!memcmp(got, exp, sizeof text - 1) && gate_calls == 1, "auto, no controller: replaced, the answer kept (%d calls)", gate_calls);
    textfix_arm(NULL, 0, 0, 1, report, sizeof report);
}

/* The real Main.wad0 with the keyboard/mouse names (default keys): the click places, the Spanish
 * click verb and a movement sentence hold the expected text after a chunked read. */
static void real_controls_check(const char *path) {
    FILE *fi = fopen(path, "rb");
    if (!fi) return;
    const int wasd[4] = {'w', 'a', 's', 'd'};
    static const struct { uint32_t lang; uint64_t at; const char *want; } places[] = {
        {1, 245102971u, "C key<"}, {1, 25060472u, "C key<"}, {1, 270659135u, "keyboard (WASD)"},
        {5, 245106168u, "oprimir la tecla"}, {5, 245106190u, "C<"}, {5, 270666467u, "teclado (W A S D)<"},
        {4, 245107757u, "C<"}, {4, 270681044u, "clavier (W A S D)<"}, {6, 245109251u, "C<"},
        {6, 270687308u, "tastiera (WASD) "}, {3, 270674448u, "WASD-Block<"},
    };
    TextFix all[TEXTFIX_MAX];
    char report[640];
    enum { CHUNK = 1 << 20 };
    uint8_t *got = malloc(CHUNK);
    for (size_t p = 0; p < sizeof places / sizeof *places; p++) {
        int n = textfix_compose(0, places[p].lang, TEXTFIX_ICONS_KBM, wasd, 'c', NULL, all, TEXTFIX_MAX);
        textfix_arm(all, n, 1, places[p].lang, report, sizeof report);
        uint64_t off = places[p].at - places[p].at % 4096 - 4096 * 3;   /* a read that starts before the place */
        fseeko(fi, (off_t)off, SEEK_SET);
        size_t len = fread(got, 1, 4096 * 7 + 1, fi);
        FileReader fr = {fi, 0};
        textfix_apply("D:\\Main.wad0", off, got, len, file_read_at, &fr);
        size_t k = (size_t)(places[p].at - off), w = strlen(places[p].want);
        CHECK(len > k + w && !memcmp(got + k, places[p].want, w), "real Main.wad0 XC %u +%llu reads '%s' (got %.18s)", places[p].lang,
              (unsigned long long)places[p].at, places[p].want, (char *)got + k);
    }
    textfix_arm(NULL, 0, 0, 1, report, sizeof report);
    free(got);
    fclose(fi);
}

int main(int argc, char **argv) {
    char report[512];
    enum { SIZE = 4096 };
    static uint8_t file[SIZE], got[SIZE];
    memset(file, '.', sizeof file);
    memcpy(file + 1000, "Sentadillas", 11);
    memcpy(file + 3000, "Sentadillas", 11);
    memcpy(file + 2000, "Sentadillos", 11);   /* near miss, not at an expected offset */
    memcpy(file + 3500, "Sentadillo!", 11);   /* differs at an expected offset */
    MemFile m = {file, sizeof file, 0};
    const TextFix fakes[] = {
        {"sit ups", "Main.wad0", XC_SPANISH, "Sentadillas", "Abdominales", {1000, 3000, 3500}, 3, NULL},
        {"scan", "other.bin", XC_SPANISH, "Sentadillas", "Abdominales", {0}, 0, NULL},
    };
    const uint64_t both[] = {1000, 3000};

    /* Options. */
    int enabled = 1;
    CHECK(textfix_parse_arg("--no-text-fixes", &enabled) == 1 && enabled == 0, "--no-text-fixes parsed, disables");
    CHECK(textfix_parse_arg("--text-fixes", &enabled) == 1 && enabled == 1, "--text-fixes parsed, enables");
    CHECK(textfix_parse_arg("--language", &enabled) == 0 && enabled == 1, "other options not consumed");

    /* Kill switch: nothing armed, every read unchanged. */
    CHECK(textfix_arm(fakes, 2, 0, XC_SPANISH, report, sizeof report) == 0 && !textfix_active, "disabled: 0 armed");
    CHECK(!strncmp(report, "[textfix] disabled", 18), "disabled report: '%s'", report);
    read_all("D:\\Main.wad0", &m, 512, got);
    CHECK(!memcmp(got, file, SIZE), "disabled: file reads unchanged");

    /* Wrong language: nothing armed. */
    CHECK(textfix_arm(fakes, 2, 1, XC_ENGLISH, report, sizeof report) == 0 && !textfix_active, "English: 0 armed");
    CHECK(strstr(report, "[textfix] none for") != NULL, "English report: '%s'", report);
    read_all("D:\\Main.wad0", &m, 512, got);
    CHECK(!memcmp(got, file, SIZE), "English: file reads unchanged");

    /* Spanish: armed; a match fully inside one read is replaced, the rest is unchanged. */
    CHECK(textfix_arm(fakes, 2, 1, XC_SPANISH, report, sizeof report) == 2 && textfix_active, "Spanish: 2 armed");
    CHECK(strstr(report, "[textfix] armed: sit ups (Main.wad0, 3 offsets)") != NULL, "Spanish report: '%s'", report);
    read_all("D:\\Main.wad0", &m, SIZE, got);
    CHECK(only_changed(got, file, SIZE, both, 2, "Abdominales"), "one read: both matches replaced, nothing else");
    CHECK(!memcmp(got + 3500, "Sentadillo!", 11), "differing bytes at an expected offset left unchanged");
    CHECK(!memcmp(got + 2000, "Sentadillos", 11), "near miss left unchanged");

    /* Every chunk size, so the matches straddle reads at every split point. */
    int all = 1;
    for (size_t chunk = 1; chunk <= 64; chunk++) {
        memset(got, 0, sizeof got);
        read_all("D:\\Main.wad0", &m, chunk, got);
        all &= only_changed(got, file, SIZE, both, 2, "Abdominales");
    }
    CHECK(all, "reads of 1..64 bytes: both matches replaced at every split, nothing else changed");

    /* A read inside the match and one ending inside it. */
    uint8_t part[16];
    read_fixed("D:\\Main.wad0", &m, 1003, 4, part);
    CHECK(!memcmp(part, "omin", 4), "read inside the match: 'omin' (got %.4s)", part);
    read_fixed("D:\\Main.wad0", &m, 995, 8, part);
    CHECK(!memcmp(part, ".....Abd", 8), "read ending inside the match: '.....Abd' (got %.8s)", part);

    /* The file name: case-insensitive base name; other files are not touched by this entry. */
    read_fixed("/disc/MAIN.WAD0", &m, 990, 30, got);
    CHECK(!memcmp(got + 10, "Abdominales", 11), "base name matched case-insensitively, any directory");
    read_fixed("D:\\Main.wad1", &m, 990, 30, got);
    CHECK(!memcmp(got + 10, "Sentadillas", 11), "other file name: unchanged");

    /* Scan mode (no expected offsets): every occurrence in other.bin, also across reads. */
    all = 1;
    for (size_t chunk = 1; chunk <= 40; chunk += 3) {
        read_all("hdd/other.bin", &m, chunk, got);
        all &= only_changed(got, file, SIZE, both, 2, "Abdominales");
    }
    CHECK(all, "scan mode: both occurrences replaced at every split, near misses unchanged");

    /* No match: a read away from the strings is returned as read, without extra file reads. */
    m.reads = 0;
    CHECK(read_fixed("D:\\Main.wad0", &m, 100, 400, got) == 0 && !memcmp(got, file + 100, 400) && m.reads == 0,
          "no match: unchanged, no extra reads (%d)", m.reads);

    /* The title's table: the Spanish gym label, same length, both offsets. */
    CHECK(textfix_count >= 1, "title table has %d fixes", textfix_count);
    const TextFix *t = &textfix_table[0];
    CHECK(!strcmp(t->file, "Main.wad0") && t->language == XC_SPANISH && !strcmp(t->old_text, "Sentadillas") &&
          !strcmp(t->new_text, "Abdominales") && t->noffsets == 2 && t->offsets[0] == 268301935u &&
          t->offsets[1] == 268996894u, "title table: Main.wad0 (es) 'Sentadillas' -> 'Abdominales' at both offsets");
    /* Entries of different lengths are refused when armed. */
    const TextFix bad[] = {{"bad", "x", XC_SPANISH, "abc", "abcd", {0}, 0, NULL}};
    CHECK(textfix_arm(bad, 1, 1, XC_SPANISH, report, sizeof report) == 0 && strstr(report, "invalid: bad"),
          "different lengths refused: '%s'", report);

    test_controls();
    if (argc > 1) real_file_check(argv[1]);
    if (argc > 1) real_controls_check(argv[1]);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
