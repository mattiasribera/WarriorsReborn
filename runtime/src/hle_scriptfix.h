/* Script fixes (on by default, --no-script-fixes turns them off): small Lua chunks the runtime
 * runs right after the game loaded one of its level scripts, to keep a mission from soft-locking
 * where the title's own script can leave it stuck. The scripts on disc are never changed.
 *
 * Each fix names the script it follows (the chunk name without "=", folder and ".lua", as mods
 * see it: "level14") and Lua 4.0 source that wraps the title's functions by global name (a Lua
 * 4.0 function cannot read an outer local, so the originals are kept in SFX_* globals). A fix
 * changes nothing while the mission works: it only acts when the title's own path did not run.
 * It logs through ScriptFixLog(text) as `[scriptfix] <text>`.
 *
 * set-up (level14, "Set Up"): stealing the radio from CARS[1] runs GangSetup(TwoDudes()), whose
 * Constructor makes the machete guy brain-dead (BrDead true) and preloads the scene l14_t1_009
 * with StartPoizo as its callback; the scene's ReturnFunc TwoDudesAttacks (or the poizo camera's
 * ReversePoizo) makes him hostile. Players saw him standing there, never hostile, when that scene
 * did not start. 15 s after the Constructor, when neither has run and the player is in no scene,
 * the fix does what they would: BrDead(TwoDudes.TD01, false), PURSUE(GangTwoDudes). */
#ifndef HLE_SCRIPTFIX_H
#define HLE_SCRIPTFIX_H
#include "hle_devlua.h"
#include <stdint.h>

typedef struct {
    const char *name;     /* "set-up": the log prefix and the chunk name "=scriptfix/set-up" */
    const char *script;   /* the level script it follows, e.g. "level14" */
    const char *code;     /* Lua 4.0 source */
} ScriptFix;

extern const ScriptFix scriptfix_table[];
extern const int scriptfix_count;

/* --no-script-fixes (0) / --script-fixes (1): returns 1 when arg is one of them. */
int scriptfix_parse_arg(const char *arg, int *enabled);

/* Turns the pass on or off (on unless set) and prints one `[scriptfix] ...` line. */
void scriptfix_set_enabled(int enabled);
int scriptfix_enabled(void);

/* "=level14.lua" / "@x/level14.lua" / "level14" -> "level14" in out; 0 when it does not fit. */
int scriptfix_script_name(const char *name, char *out, size_t cap);

/* After the game loaded the script `name` in L: runs every fix for that script (none when the
 * pass is off). bind(L), when set, registers ScriptFixLog first. Returns the number run. */
int scriptfix_on_script(const DevLuaApi *api, uint32_t L, const char *name, void (*bind)(uint32_t L));
#endif
