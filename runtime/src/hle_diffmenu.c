/* One extra choice in the new-profile difficulty list (see hle_diffmenu.h). */
#include "hle_diffmenu.h"
#include "hle_devlua.h"
#include <stdio.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

void diffmenu_set(DiffMenu *m, const char *label, const char *question, const char *yes, const char *no) {
    copy_text(m->label, sizeof m->label, label);
    copy_text(m->question, sizeof m->question, question);
    copy_text(m->yes, sizeof m->yes, yes);
    copy_text(m->no, sizeof m->no, no);
    m->armed = m->label[0] != 0;
    if (!m->armed) m->chosen = -1;
}

DiffAction diffmenu_on_message(DiffMenu *m, uint32_t msg, uint32_t selected) {
    if (m->question_shown) {
        if (msg == 4 && selected == 0) {
            m->chosen = 1;
            return DIFF_ACT_YES;
        }
        if (msg == 4 || msg == 5) {
            m->pending = DIFF_PENDING_LIST;
            return DIFF_ACT_LIST;
        }
        return DIFF_ACT_GAME;
    }
    if (msg == 4 && m->armed && m->extra_index >= 0 && selected == (uint32_t)m->extra_index) {
        m->pending = DIFF_PENDING_QUESTION;
        return DIFF_ACT_ASK;
    }
    if (msg == 4) {
        m->chosen = 0;
        return DIFF_ACT_GAME_CLEAR;
    }
    return DIFF_ACT_GAME;
}

#ifndef HLE_DIFFMENU_TEST
/* ---- game side ---- */

static DiffMenu menu = {.extra_index = -1, .chosen = -1};

#define AT(va) ((uint8_t *)(uintptr_t)(va))
#define U32(va) (*(uint32_t *)(uintptr_t)(va))

typedef void(__attribute__((thiscall)) * VoidFn)(uint32_t self);
typedef uint8_t(__attribute__((thiscall)) * ListenFn)(uint32_t self, uint32_t msg);
typedef uint32_t(__attribute__((thiscall)) * UpdateFn)(uint32_t self);
typedef void(__attribute__((thiscall)) * SelectFn)(uint32_t grid, uint32_t index, uint32_t flag);
typedef void(__attribute__((thiscall)) * AddFn)(uint32_t grid, const char *text, uint32_t scale, uint32_t a,
                                                uint32_t b, uint32_t c, uint32_t d, uint32_t font, uint32_t e,
                                                uint32_t f);
typedef void(__attribute__((thiscall)) * SoundFn)(uint32_t self, uint32_t *out, uint32_t id);

static uint32_t grid_count(uint32_t grid) { return (U32(grid + 0x50) - U32(grid + 0x4c)) >> 2; }

static void ui_sound(uint32_t id) {
    uint32_t out;
    ((SoundFn)(uintptr_t)DIFF_UI_SOUND)(U32(0x0044d700u), &out, id);
}

__attribute__((thiscall, force_align_arg_pointer)) static void hook_build(uint32_t page) {
    const VoidFn build = (VoidFn)(uintptr_t)DIFF_BUILD;
    menu.question_shown = menu.pending == DIFF_PENDING_QUESTION;
    menu.pending = DIFF_PENDING_NONE;
    menu.extra_index = -1;
    if (!menu.question_shown) {
        /* The title sits above the grid at a height chosen for 3 rows ([DIFF_TITLE3]) or 4 (the
         * fury list, [DIFF_TITLE4]); with the extra row the build reads the next one up. */
        float *t3 = (float *)(uintptr_t)DIFF_TITLE3, *t4 = (float *)(uintptr_t)DIFF_TITLE4;
        const float keep3 = *t3, keep4 = *t4;
        if (menu.armed) *t3 = keep4, *t4 = keep4 + (keep4 - keep3);
        build(page);
        *t3 = keep3, *t4 = keep4;
        return;
    }
    /* One build as the question: title and the first two items swapped for the question and the
     * game's own YES / NO, the HARDCORE (and fury) items jumped over; the default stays item 1 (NO). */
    uint32_t *s = (uint32_t *)(uintptr_t)DIFF_STRINGS;
    const uint32_t keep[3] = {s[0x8f], s[0x90], s[0x91]};
    s[0x8f] = (uint32_t)(uintptr_t)menu.question;
    s[0x90] = menu.yes[0] ? (uint32_t)(uintptr_t)menu.yes : s[0x84];
    s[0x91] = menu.no[0] ? (uint32_t)(uintptr_t)menu.no : s[0x85];
    uint8_t code[5], jmp[5] = {0xe9};
    const uint32_t rel = DIFF_AFTER_ITEMS - (DIFF_HARD_ITEM + 5);
    memcpy(code, AT(DIFF_HARD_ITEM), 5);
    memcpy(jmp + 1, &rel, 4);
    memcpy(AT(DIFF_HARD_ITEM), jmp, 5);
    build(page);
    memcpy(AT(DIFF_HARD_ITEM), code, 5);
    s[0x8f] = keep[0], s[0x90] = keep[1], s[0x91] = keep[2];
}

/* Replaces the build's call of OptionGrid::Select: the extra item goes last, in a row of its own,
 * then the default. The grid lays its items out by rows: items per row at [grid+0x84 + 4 * row]
 * (DIFF_GRID_ROWS at most; the build passes 1,1,1,0, or 1,1,1,1 with the fury item) and the grid's
 * height at [grid+0x10] (the build's 3-row value [0x45c0e4] or 4-row [0x45c0e8]). */
__attribute__((thiscall, force_align_arg_pointer)) static void hook_select(uint32_t grid, uint32_t index, uint32_t flag) {
    if (menu.armed && !menu.question_shown) {
        const uint32_t n = grid_count(grid);
        uint32_t used = 0;
        int row = 0;
        while (row < DIFF_GRID_ROWS && used < n) used += U32(grid + 0x84 + 4 * row), row++;
        if (row < DIFF_GRID_ROWS && used == n) {
            ((AddFn)(uintptr_t)DIFF_GRID_ADD)(grid, menu.label, 0x3f933333u, 0, 0, 4, 1, U32(0x00459260u), 6, 1);
            if (grid_count(grid) == n + 1) {
                const float h3 = *(const float *)(uintptr_t)DIFF_HEIGHT3, h4 = *(const float *)(uintptr_t)DIFF_HEIGHT4;
                U32(grid + 0x84 + 4 * row) = 1;
                if (row >= 3) *(float *)(uintptr_t)(grid + 0x10) = h4 + (float)(row - 3) * (h4 - h3);
                menu.extra_index = (int)n;
            }
        } else {
            static int warned;
            if (!warned++) printf("[diffmenu] no row left for \"%s\" (%u items); not added\n", menu.label, n);
        }
    }
    ((SelectFn)(uintptr_t)DIFF_GRID_SELECT)(grid, index, flag);
}

__attribute__((thiscall, force_align_arg_pointer)) static uint8_t hook_listen(uint32_t listener, uint32_t msg) {
    const uint32_t page = listener - 0x2c, grid = U32(page + 0x64);
    const uint32_t selected = grid ? U32(grid + 0x78) : 0;
    const ListenFn game = (ListenFn)(uintptr_t)DIFF_LISTEN;
    switch (diffmenu_on_message(&menu, msg, selected)) {
    case DIFF_ACT_ASK:
        ui_sound(9);
        return 1;
    case DIFF_ACT_LIST:
        ui_sound(0xf);
        return 1;
    case DIFF_ACT_YES: {
        const uint8_t r = game(listener, msg);
        const uint32_t profile = U32(DIFF_PROFILE);
        if (profile) AT(profile + DIFF_PROFILE_DIFFICULTY)[0] = DIFF_HARD;
        menu.question_shown = 0;
        printf("[diffmenu] \"%s\" confirmed: profile difficulty %d (Hard)\n", menu.label, DIFF_HARD);
        fflush(stdout);
        return r;
    }
    default:
        return game(listener, msg);
    }
}

/* The page's per-frame step: a rebuild asked for by the listener runs here, outside the grid's own
 * input code. */
__attribute__((thiscall, force_align_arg_pointer)) static uint32_t hook_update(uint32_t iface) {
    if (menu.pending != DIFF_PENDING_NONE) {
        const uint32_t page = iface - 0x48;
        ((VoidFn)(uintptr_t)DIFF_TEARDOWN)(page);
        hook_build(page);
    }
    return ((UpdateFn)(uintptr_t)DIFF_UPDATE)(iface);
}

static const struct {
    const char *what;
    uint32_t va;
    uint8_t n, sig[12];
} sigs[] = {
    {"build", DIFF_BUILD, 6, {0x83, 0xec, 0x44, 0x56, 0x8b, 0xf1}},
    {"listener", DIFF_LISTEN, 7, {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x05}},
    {"profile write", 0x0012f438u, 12, {0x8b, 0x0d, 0x78, 0x9f, 0x5b, 0x00, 0x88, 0x81, 0x23, 0x04, 0x00, 0x00}},
    {"update", DIFF_UPDATE, 6, {0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x47}},
    {"teardown", DIFF_TEARDOWN, 4, {0x56, 0x8b, 0xf1, 0x8a}},
    {"select call", DIFF_SELECT_CALL, 5, {0xe8, 0xdf, 0xd9, 0xfb, 0xff}},
    {"hardcore item", DIFF_HARD_ITEM, 6, {0x8b, 0x15, 0x60, 0x92, 0x45, 0x00}},
    {"default selection", DIFF_AFTER_ITEMS, 3, {0x6a, 0x00, 0x57}},
};

int hle_diffmenu_install(void) {
    static int state;   /* 0 not tried, 1 installed, -1 refused */
    if (state) return state > 0;
    const char *bad = NULL;
    uint32_t bad_va = 0;
    for (size_t i = 0; !bad && i < sizeof sigs / sizeof sigs[0]; i++)
        if (memcmp(AT(sigs[i].va), sigs[i].sig, sigs[i].n)) bad = sigs[i].what, bad_va = sigs[i].va;
    if (!bad && U32(DIFF_SLOT_BUILD) != DIFF_BUILD) bad = "build slot", bad_va = DIFF_SLOT_BUILD;
    if (!bad && U32(DIFF_SLOT_LISTEN) != DIFF_LISTEN) bad = "listener slot", bad_va = DIFF_SLOT_LISTEN;
    if (!bad && U32(DIFF_SLOT_UPDATE) != DIFF_UPDATE) bad = "update slot", bad_va = DIFF_SLOT_UPDATE;
    if (bad) {
        printf("[diffmenu] not available: %s at %08x is not the shipped code (nothing written)\n", bad, bad_va);
        fflush(stdout);
        state = -1;
        return 0;
    }
    U32(DIFF_SLOT_BUILD) = (uint32_t)(uintptr_t)hook_build;
    U32(DIFF_SLOT_LISTEN) = (uint32_t)(uintptr_t)hook_listen;
    U32(DIFF_SLOT_UPDATE) = (uint32_t)(uintptr_t)hook_update;
    const uint32_t rel = (uint32_t)(uintptr_t)hook_select - (DIFF_SELECT_CALL + 5);
    memcpy(AT(DIFF_SELECT_CALL + 1), &rel, 4);
    printf("[diffmenu] difficulty list hooks at %08x (build, listener, update) and %08x\n", DIFF_BUILD, DIFF_SELECT_CALL);
    fflush(stdout);
    state = 1;
    return 1;
}

static void (*push_number)(uint32_t L, double n);
static void (*push_nil)(uint32_t L);

void diffmenu_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L)) {
    push_number = pushnumber;
    push_nil = pushnil;
}

/* ModDifficultyOption(label, question, yes, no): label a string sets the option (question: a string,
 * else the label; yes / no: the two answers, else the game's YES / NO), nil removes it. 1, or nil when the list cannot be hooked. */
__attribute__((force_align_arg_pointer, cdecl)) int hle_diffmenu_lua_option(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *label = a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const char *question = a->type(L, 2) == 3 ? a->tostring(L, 2) : label;
    const char *yes = a->type(L, 3) == 3 ? a->tostring(L, 3) : NULL;
    const char *no = a->type(L, 4) == 3 ? a->tostring(L, 4) : NULL;
    int ok = 1;
    if (label && *label) ok = hle_diffmenu_install();
    if (ok) {
        const int was = menu.armed;
        char before[DIFF_LABEL_MAX + 1];
        copy_text(before, sizeof before, menu.label);
        diffmenu_set(&menu, label, question, yes, no);
        if (was != menu.armed || strcmp(before, menu.label)) {
            if (menu.armed) printf("[diffmenu] extra difficulty \"%s\"\n", menu.label);
            else printf("[diffmenu] extra difficulty removed\n");
            fflush(stdout);
        }
    }
    if (ok && push_number) push_number(L, 1);
    else if (push_nil) push_nil(L);
    return 1;
}

/* ModDifficultyChosen(): what the last new profile chose since the previous call: 1 the extra choice
 * (YES), 0 one of the game's own, nil none. */
__attribute__((force_align_arg_pointer, cdecl)) int hle_diffmenu_lua_chosen(uint32_t L) {
    const int c = menu.chosen;
    menu.chosen = -1;
    if (c >= 0 && push_number) push_number(L, c);
    else if (push_nil) push_nil(L);
    return 1;
}

#else
/* ---- game-independent test: the page's decisions ---- */
#include <stdlib.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } \
    } while (0)

int main(void) {
    DiffMenu m = {.extra_index = -1, .chosen = -1};
    /* Not armed: every message is the game's; a retail pick is noted as 0. */
    m.chosen = 1;
    CHECK(diffmenu_on_message(&m, 4, 3) == DIFF_ACT_GAME_CLEAR && m.chosen == 0);
    CHECK(diffmenu_on_message(&m, 5, 0) == DIFF_ACT_GAME);

    diffmenu_set(&m, "WARRIORS", "Sure?", "YES - harder", NULL);
    CHECK(m.armed && !strcmp(m.label, "WARRIORS") && !strcmp(m.question, "Sure?") && !strcmp(m.yes, "YES - harder") &&
          !m.no[0]);
    m.extra_index = 3;
    /* The retail items stay the game's. */
    CHECK(diffmenu_on_message(&m, 4, 1) == DIFF_ACT_GAME_CLEAR && m.pending == DIFF_PENDING_NONE);
    CHECK(diffmenu_on_message(&m, 5, 3) == DIFF_ACT_GAME);   /* back on the list: the game's */
    /* A on the extra item asks. */
    m.chosen = -1;
    CHECK(diffmenu_on_message(&m, 4, 3) == DIFF_ACT_ASK && m.pending == DIFF_PENDING_QUESTION && m.chosen == -1);
    m.pending = DIFF_PENDING_NONE, m.question_shown = 1, m.extra_index = -1;
    /* NO and back go back to the list; nothing chosen. */
    CHECK(diffmenu_on_message(&m, 4, 1) == DIFF_ACT_LIST && m.pending == DIFF_PENDING_LIST && m.chosen == -1);
    m.pending = DIFF_PENDING_NONE;
    CHECK(diffmenu_on_message(&m, 5, 0) == DIFF_ACT_LIST && m.pending == DIFF_PENDING_LIST);
    m.pending = DIFF_PENDING_NONE;
    CHECK(diffmenu_on_message(&m, 7, 0) == DIFF_ACT_GAME);   /* other messages: the game's */
    /* YES. */
    CHECK(diffmenu_on_message(&m, 4, 0) == DIFF_ACT_YES && m.chosen == 1);
    /* Back on the list (from the next page), a retail pick clears it again. */
    m.question_shown = 0, m.extra_index = 3;
    CHECK(diffmenu_on_message(&m, 4, 2) == DIFF_ACT_GAME_CLEAR && m.chosen == 0);
    /* Removing the option. */
    m.chosen = 1;
    diffmenu_set(&m, NULL, NULL, NULL, NULL);
    CHECK(!m.armed && m.chosen == -1 && !m.label[0]);
    /* Over-long texts are cut. */
    char longs[400];
    memset(longs, 'x', sizeof longs - 1);
    longs[sizeof longs - 1] = 0;
    diffmenu_set(&m, longs, longs, longs, longs);
    CHECK(strlen(m.label) == DIFF_LABEL_MAX && strlen(m.question) == DIFF_QUESTION_MAX &&
          strlen(m.yes) == DIFF_QUESTION_MAX && strlen(m.no) == DIFF_QUESTION_MAX);
    printf(fails ? "hle_diffmenu_test: %d FAILED\n" : "hle_diffmenu_test: ALL OK\n", fails);
    return fails ? 1 : 0;
}
#endif
