/* One extra choice in the new-profile difficulty list, with a Yes/No question (mods version 1.4;
 * docs/MODDING.md, ModDifficultyOption). Off until a mod calls ModDifficultyOption(label, question).
 *
 * The list is C++, not Lua: level100.lua calls ShowProfileManager (0x2809b0), a screen flow
 * (PM_Controller 0x12d900) Create (name) -> Difficulty -> Light -> Subtitles. The Difficulty page
 * (GUI\ProfileManagementGUI\PM_Difficulty.cpp, page P):
 *   build    0x12f540 (vtable slot 0x42c158, this = P): title TextWidget [P+0x60] = string 0x8f,
 *            OptionGrid [P+0x64] items 0x90-0x92 (+0x93 "UNLEASH THE FURY" once unlocked) added by
 *            OptionGrid::Add 0xedde0 (thiscall: text, 1.15f, 0, 0, 4, 1, font [0x459260], 6, 1),
 *            then OptionGrid::Select 0xed1b0 (grid; item 1, or 3 with the fury item; 0) at 0x12f7cc.
 *   listener 0x12f400 (slot 0x42c0f8, this = P+0x2c; msg): 4 = A on an item: the profile's
 *            difficulty byte [*0x5b9f78 + 0x423] = the selected item ([grid+0x78]), result 0
 *            (-> Light); 5 = back. Returns 1 when handled.
 *   update   0x12f4a0 (slot 0x42c0f0, this = P+0x48): the page's per-frame step.
 *   teardown 0x12f870 (this = P).
 * Strings: char *[id] at 0x4d5b78 (0x84 "YES", 0x85 "NO" in the game's language).
 *
 * The hooks (vtable slots and the one call at 0x12f7cc, all checked first):
 *   - the list gets the extra item last, in a row of its own (the grid's row table and height
 *     follow), before the default selection, which stays;
 *   - A on it does not commit: the page is rebuilt on the next update as a question: title = the
 *     mod's question (one line: the title widget ignores newlines and pushes a line wider than about
 *     40 characters off the screen), items = the mod's answers or the game's own YES / NO (the
 *     HARDCORE and fury items are jumped over for that one build), the second (NO) selected;
 *   - YES runs the game's own select with the difficulty byte then set to 2 (Hard) and marks the
 *     choice for ModDifficultyChosen (1); NO or back rebuild the list; a retail item notes 0. */
#ifndef HLE_DIFFMENU_H
#define HLE_DIFFMENU_H
#include <stddef.h>
#include <stdint.h>

#define DIFF_SLOT_BUILD 0x0042c158u
#define DIFF_SLOT_LISTEN 0x0042c0f8u
#define DIFF_SLOT_UPDATE 0x0042c0f0u
#define DIFF_BUILD 0x0012f540u
#define DIFF_LISTEN 0x0012f400u
#define DIFF_UPDATE 0x0012f4a0u
#define DIFF_TEARDOWN 0x0012f870u
#define DIFF_SELECT_CALL 0x0012f7ccu   /* call OptionGrid::Select (e8 rel32) */
#define DIFF_HARD_ITEM 0x0012f765u     /* mov edx,[0x459260]: the HARDCORE item starts here */
#define DIFF_AFTER_ITEMS 0x0012f7c6u   /* push 0; push edi (1): the default selection */
#define DIFF_GRID_ADD 0x000edde0u
#define DIFF_GRID_SELECT 0x000ed1b0u
#define DIFF_UI_SOUND 0x000269a0u      /* (this [0x44d700]; &out, id): 9 = select, 0xf = back */
#define DIFF_STRINGS 0x004d5b78u
#define DIFF_GRID_ROWS 5               /* items per row at [grid+0x84 + 4 * row] */
#define DIFF_HEIGHT3 0x0045c0e4u       /* float: the grid's height for 3 rows (0x12d0a0) */
#define DIFF_HEIGHT4 0x0045c0e8u       /* ... for 4 rows (0x12d0b0, the fury list) */
#define DIFF_TITLE3 0x0045c0f4u        /* float: the title's height for 3 rows (0x12d0e0) */
#define DIFF_TITLE4 0x0045c0f8u        /* ... for 4 rows (0x12d0f0) */
#define DIFF_PROFILE 0x005b9f78u
#define DIFF_PROFILE_DIFFICULTY 0x423
#define DIFF_HARD 2
#define DIFF_LABEL_MAX 48
#define DIFF_QUESTION_MAX 200

/* The page's state between hooks (pure; tests drive it without the game). */
typedef enum { DIFF_PENDING_NONE, DIFF_PENDING_QUESTION, DIFF_PENDING_LIST } DiffPending;
typedef enum {
    DIFF_ACT_GAME,        /* the game's own handler */
    DIFF_ACT_GAME_CLEAR,  /* the game's own handler; a retail difficulty was chosen: clear the mark */
    DIFF_ACT_ASK,         /* A on the extra item: rebuild as the question */
    DIFF_ACT_YES,         /* the game's own select, then difficulty = Hard and the mark */
    DIFF_ACT_LIST,        /* NO or back on the question: rebuild the list */
} DiffAction;

typedef struct {
    int armed;                 /* a mod set an option */
    char label[DIFF_LABEL_MAX + 1], question[DIFF_QUESTION_MAX + 1];
    char yes[DIFF_QUESTION_MAX + 1], no[DIFF_QUESTION_MAX + 1];   /* "" = the game's YES / NO */
    int question_shown;        /* the page shows the question */
    int extra_index;           /* the extra item's index in the list, -1 when not shown */
    DiffPending pending;
    int chosen;                /* since the last ModDifficultyChosen: 1 YES, 0 a retail pick, -1 none */
} DiffMenu;

/* msg 4 (A) / 5 (back) with item `selected`: what the listener does. Sets pending / chosen. */
DiffAction diffmenu_on_message(DiffMenu *m, uint32_t msg, uint32_t selected);
/* Sets (label not NULL / "") or removes the option; texts are cut to their maximum. */
void diffmenu_set(DiffMenu *m, const char *label, const char *question, const char *yes, const char *no);

/* The game-side hooks: signature checks and writes, once. 1 = installed (or already), 0 = not
 * (logged `[diffmenu] not available: ...`). */
int hle_diffmenu_install(void);
/* Lua: ModDifficultyOption(label, question[, yes, no]) -> 1 or nil; ModDifficultyChosen() -> 1 after YES, 0 after
 * one of the game's own difficulties, nil when no new profile chose since the last call. Called by the game's VM (hle_mods registers them). */
void diffmenu_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L));
int hle_diffmenu_lua_option(uint32_t L);
int hle_diffmenu_lua_chosen(uint32_t L);
#endif
