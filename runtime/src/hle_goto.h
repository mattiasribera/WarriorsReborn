/* --goto PLACE: named shortcuts to places developers keep needing, built on --skip-intro, the
 * headless menu input script (BENCH_INPUT: title -> main menu -> Continue the first profile),
 * --warp and developer Lua (hle_devlua.h). `--goto list` prints the table.
 *
 * Every target blocks the profile write for the session (the warp's save stub, or the save
 * guard when the level is not redirected), so a --goto run never changes the save on disk;
 * --goto-allow-save lifts the guard for the targets that do not redirect a level load (the
 * test-save generator uses it). Lua steps run at the lua_call hook, again once per frame until
 * their condition holds (they return 1), and log `[lua] goto:<place>: done at frame N`.
 *
 * Targets (hub = level95, the Warriors' hideout and the Coney streets around it):
 *   title          main menu after the title screen; no profile loaded
 *   hideout[:C]    the hub at the save's own checkpoint (Continue), or hub checkpoint C (1-12, warped)
 *   coney          the hub, then out of the hideout's door onto the Coney streets
 *   gym            the hub, player teleported to the heavy-bag workout flag (press A to start)
 *   rumble         the hub, every unlockable unlocked (UM_UnlockAll(true), in memory only), the
 *                  Rumble menu opened, then three A presses (input script, frames 4700/5100/5500):
 *                  1 on 1, 1P vs CPU -> Choose Gangs (about frame 5600)
 *   aotn[:L[:C]]   Armies of the Night level L (60-64, default 60) at chapter C (default 1)
 *   m<N>[:C]       story mission N (1-23) at checkpoint C: the same as --warp N[:C] */
#ifndef HLE_GOTO_H
#define HLE_GOTO_H
#include "hle_warp.h"
#include <stdio.h>

typedef struct {
    const char *place;      /* canonical target name */
    const char *input;      /* menu input script (NULL: none) */
    WarpTarget warp;        /* level redirect when warp_on */
    int warp_on;
    const char *lua;        /* Lua step retried until it returns non-nil (NULL: none) */
    const char *leaves;     /* one line: the state the run is left in */
} GotoPlan;

/* Parses a target ("rumble", "aotn:62:2", "m8:3"...). 0, or -1 with a message in err. */
int goto_parse(const char *s, GotoPlan *p, char *err, size_t cap);
/* "--goto VALUE" / "--goto-allow-save": 1 handled, 0 not ours, -1 invalid (err). *on is set by
 * --goto, *allow_save by --goto-allow-save. */
int goto_parse_arg(int argc, char **argv, int *i, GotoPlan *p, int *on, int *allow_save, char *err, size_t cap);
/* Writes the table (`--goto list`). */
void goto_list(FILE *f);
/* Armies of the Night levels (level60-64; mission 25 in levelNames). */
const WarpLevel *goto_find_aotn(int level);
#endif
