/* The game's side of the in-game settings sync with the launcher
 * (the launcher's side: launcher/src/settings_sync.h). The launcher passes at start, in the
 * environment (settings_model.h SM_ENV_*): its profiles file, the profile, the pending file, its
 * UI language, the presets file and whether it relaunches the game. The game never writes the
 * launcher's profiles file: it reads the profile's values from it once (the start values) and
 * writes the keys the player changed to the pending file (atomic replace) in profile.cpp's key
 * names; the launcher merges them. Without the environment (a game started by hand) changes stay
 * for the session and settings_sync_saved() is 0.
 *
 * Restart: settings_sync_restart() writes the pending file and quits the game with SM_EXIT_RESTART
 * (through settings_sync_quit, hle_d3d's quit request); the launcher relaunches the same profile with
 * --skip-intro when settings_sync_restart_ok(). */
#ifndef SETTINGS_SYNC_H
#define SETTINGS_SYNC_H
#include "settings_model.h"

/* Reads the environment (and the test hook, SM_ENV_TEST). Returns 1 when changes are saved for
 * the launcher. Logs one [settings] line. */
int settings_sync_init(void);
int settings_sync_saved(void);
int settings_sync_restart_ok(void);
const char *settings_sync_ui_language(void);   /* "en", "es" or "" (not passed) */
const char *settings_sync_presets_path(void);  /* "" when not passed */
const char *settings_sync_profile(void);       /* "" when not passed */
/* The profile's values at start (from the profiles file; the defaults for a key it lacks) and now. */
const SmValues *settings_sync_start(void);
const SmValues *settings_sync_now(void);

/* Sets one setting (profile.cpp key and value) and rewrites the pending file. 0 done; -1 unknown
 * key; -2 a value outside its domain (nothing changes); -3 the pending file could not be written
 * (the value is kept for the session); -4 not initialized. */
int settings_sync_set(const char *key, const char *value);
/* The pending file from the start and current values: 0, -1 when it cannot be written, 1 when there
 * is no pending path (a game started by hand). */
int settings_sync_write(void);
/* Writes the pending file and quits with SM_EXIT_RESTART. */
void settings_sync_restart(void);
/* The quit request (main.c: hle_d3d_request_quit); NULL: the test's stub or nothing. */
extern void (*settings_sync_quit)(int exit_code);

/* SM_ENV_TEST's file: "after SECONDS", "KEY = VALUE" lines, "restart"; '#' comments. */
#define SETTINGS_TEST_MAX 16
typedef struct SettingsTestPlan {
    double after;
    int restart;
    int n;
    char key[SETTINGS_TEST_MAX][64];
    char value[SETTINGS_TEST_MAX][256];
} SettingsTestPlan;
/* 0, or -1 for a bad line (*plan is then unusable). */
int settings_sync_test_parse(const char *text, SettingsTestPlan *plan);
/* Runs a parsed plan's changes and restart now (the hook's thread after its delay). */
void settings_sync_test_run(const SettingsTestPlan *plan);

/* For the tests: forget everything (settings_sync_init reads the environment again). */
void settings_sync_reset(void);

#endif
