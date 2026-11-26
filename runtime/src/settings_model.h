/* The settings model: one table of every launcher profile setting,
 * for the in-game settings menu and the launcher sync. Plain C with no runtime dependencies: the
 * launcher builds this file too (like modini.c), so both sides agree on the keys, their values, the
 * command line each becomes and how each one applies.
 *
 * Each SmSetting row is one key of launcher/src/profile.cpp's [profile NAME] section:
 *   key        the profile key, exactly as profile.cpp writes it
 *   type       bool / int / float / enum / string / path
 *   dflt       the value of a plain launcher Profile(), as the ini writes it
 *   domain     int: "LO..HI" or "A|B|C"; float: "LO..HI" (a negative value also means "left out",
 *              the launcher's -1); enum: "a|b|c" (an empty first choice: "" allowed)
 *   cls        how a change applies in a running game (the plan's Inventory): SM_LIVE (a, at a frame
 *              boundary), SM_PASS (b, one pass rebuilt: short pause), SM_REINIT (c, renderer re-init:
 *              ~1 s black), SM_RESTART (d, on the next start), SM_BOOT ('-', start-up only, never changed
 *              from the game). sm_change_class refines it against the start values.
 *   tab        the in-game menu tab, SM_TAB_NONE for keys the menu never shows
 *   flags      SM_MENU (shown in the menu), SM_PRESET (a launcher preset sets it), SM_DEPTH / SM_COLOR
 *              (reads the depth / colour target), SM_BOOT_PACK (needs a pack layered at start-up)
 *   requires   the menu prerequisite (a greyed row when not met), as an expression (below); "" = none
 *   label/desc ids of launcher/src/i18n_strings.h (X(id, en, es)); NULL when there is none
 *   arg/flag/param/arg_requires  the command line launcher/src/command.cpp build_argv makes of it, in
 *              build_argv's order (the table's order); sm_build_argv gives the same argv
 *
 * Expressions (requires, arg_requires): clauses joined by '&', each clause alternatives joined by
 * '|'; an atom is KEY (true: a bool true, any other value non-empty and not 0/off/false),
 * KEY=V1/V2 (equals one of the values; "KEY=" is empty) or KEY!=V1/V2 (equals none of them).
 *
 * To add a setting: one row in settings_model.c at its build_argv position, its profile.cpp key and
 * its i18n ids; the launcher's settings test fails until profile.cpp and build_argv agree. */
#ifndef SETTINGS_MODEL_H
#define SETTINGS_MODEL_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- launcher <-> game sync (W7) ---- */
/* Environment the launcher sets for the game it starts (the runtime's argv parser is untouched). */
#define SM_ENV_INI "WARRIORS_PROFILE_INI"          /* the launcher's profiles file */
#define SM_ENV_PROFILE "WARRIORS_PROFILE"          /* the profile's name */
#define SM_ENV_PENDING "WARRIORS_SETTINGS_PENDING" /* where the game writes its changed keys */
#define SM_ENV_UI_LANGUAGE "WARRIORS_UI_LANGUAGE"  /* the launcher's language: en|es */
#define SM_ENV_PRESETS "WARRIORS_PRESETS"          /* the launcher's presets, [preset KEY] sections */
#define SM_ENV_RESTART "WARRIORS_RESTART"          /* 1: the launcher waits and relaunches on SM_EXIT_RESTART */
/* Test hook: a file the game reads (and deletes) at start: "after SECONDS", "KEY = VALUE" lines and
 * "restart"; after SECONDS it sets those keys (the pending file) and, with "restart", exits with
 * SM_EXIT_RESTART. */
#define SM_ENV_TEST "WARRIORS_SETTINGS_TEST"
/* The game's exit code for "restart me with the pending settings" (0-4 and crash codes are taken). */
#define SM_EXIT_RESTART 86
/* The pending file's first line; then "[profile NAME]" and "KEY = VALUE" lines (changed keys only). */
#define SM_PENDING_MAGIC "# warriors pending settings 1"

typedef enum { SM_BOOL, SM_INT, SM_FLOAT, SM_ENUM, SM_STR, SM_PATH } SmType;
typedef enum { SM_TAB_NONE, SM_TAB_DISPLAY, SM_TAB_EFFECTS, SM_TAB_AUDIO, SM_TAB_CONTROLS, SM_TAB_GAME, SM_TAB_MODS } SmTab;
enum { SM_LIVE = 'a', SM_PASS = 'b', SM_REINIT = 'c', SM_RESTART = 'd', SM_BOOT = '-' };
enum { SM_MENU = 1, SM_PRESET = 2, SM_DEPTH = 4, SM_COLOR = 8, SM_BOOT_PACK = 16 };

typedef enum {
    SM_ARG_NONE,     /* no command line (preset) */
    SM_ARG_HEAD,     /* xbrun / disc: argv[0..2] = xbrun, <disc>/default.xbe, disc */
    SM_ARG_ON,       /* bool: flag when true (then param, when not empty) */
    SM_ARG_OFF,      /* bool: flag when false */
    SM_ARG_VALUE,    /* flag value, when the value is not empty and not param */
    SM_ARG_LISTED,   /* flag value, only for the values listed in param ("kbm|auto") */
    SM_ARG_ALWAYS,   /* flag value: the value when listed in param, else param's last choice */
    SM_ARG_MAP,      /* the flag of the value in param ("on=--a|off=--no-a") */
    SM_ARG_INT,      /* flag N when N > param */
    SM_ARG_INT_CLAMP,/* flag N when N > param, N clamped to the domain's top */
    SM_ARG_INT_NE,   /* flag N when N > 0 and N is not the default */
    SM_ARG_VOLUME,   /* flag N when N != 0, clamped to the domain */
    SM_ARG_FLOAT,    /* flag F when F >= 0 */
    SM_ARG_FLOAT_NE, /* flag F when F is not the default */
    SM_ARG_GLYPHS,   /* --texture-pack <glyph_packs>/<button_icons> for the PlayStation icons */
    SM_ARG_ICONS,    /* --button-icons kbm|auto, or ps-* when no glyph_packs folder is set */
    SM_ARG_FPS_ITEMS,/* --fps-overlay-items LIST unless it is fps + ms */
    SM_ARG_DRAW_DISTANCE, /* --draw-distance F --prop-detail F for 2-4 */
    SM_ARG_EXTRA     /* the typed extra arguments, split like the launcher's split_args */
} SmArg;

typedef struct SmSetting {
    const char *key;
    SmType type;
    const char *dflt;
    const char *domain;
    char cls;
    SmTab tab;
    unsigned flags;
    const char *requires;
    const char *label, *desc;
    SmArg arg;
    const char *flag, *param;
    const char *arg_requires;
} SmSetting;

int sm_count(void);
const SmSetting *sm_setting(int i);   /* NULL outside 0..sm_count()-1 */
int sm_find(const char *key);          /* its index, -1 for an unknown key */
const char *sm_tab_key(SmTab tab);     /* display|effects|audio|controls|game|mods|none */

/* 1 when value is in the setting's domain (a path or string: anything). */
int sm_valid(const SmSetting *s, const char *value);

/* Every setting's value as the ini writes it (bools true/false, ints %d, floats %.3g). */
typedef struct SmValues {
    char **v;   /* sm_count() strings, malloc'd */
} SmValues;
int sm_values_init(SmValues *vals);    /* the defaults; -1 out of memory */
int sm_values_copy(SmValues *dst, const SmValues *src);
void sm_values_free(SmValues *vals);
/* 0 set (normalized), -1 unknown key, -2 a value outside the domain (nothing changes), -3 out of memory. */
int sm_set(SmValues *vals, const char *key, const char *value);
const char *sm_get(const SmValues *vals, const char *key);   /* NULL for an unknown key */
int sm_truthy(const char *value);
/* 1 when the expression holds for vals ("" holds); an unknown key in it is false. */
int sm_expr(const SmValues *vals, const char *expr);
/* The menu row's prerequisite is met (the row is not greyed). */
int sm_available(const SmValues *vals, int i);

/* The command line build_argv(Profile) makes of these values (argv[0] = the xbrun value). */
typedef struct SmArgv {
    char **a;
    int n, cap;
} SmArgv;
int sm_build_argv(const SmValues *vals, SmArgv *out);   /* 0, -1 out of memory; out starts empty */
void sm_argv_free(SmArgv *argv);

/* How setting i changing from its start value (start: the values the game started with) to value
 * applies: the row's class, raised to SM_REINIT when it would be the first reader of the depth target
 * (SM_DEPTH: none was on at start) or of the colour target (SM_COLOR: neither bloom nor a glass HUD at
 * start), to SM_RESTART when it needs a start-up pack it did not have (SM_BOOT_PACK turned on); a
 * temporal upscaling mode change while it was on at start and stays on is SM_LIVE. SM_LIVE also for an
 * unchanged value. */
char sm_change_class(const SmValues *start, int i, const char *value);
/* The heaviest class of every setting that differs between start and now (SM_LIVE when none). */
char sm_changes_class(const SmValues *start, const SmValues *now);
int sm_class_rank(char cls);   /* a 0, b 1, c 2, d 3, '-' 4 */

/* The pending file the game writes: SM_PENDING_MAGIC, [profile NAME], then "KEY = VALUE" for each
 * SM_MENU setting whose value in now differs from start. Returns the length written (buf always
 * terminated; truncated text is never valid: compare with n), or -1 for a profile name with a ']' or
 * line break. */
int sm_pending_format(char *buf, size_t n, const char *profile, const SmValues *start, const SmValues *now);

#ifdef __cplusplus
}
#endif
#endif
