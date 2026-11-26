/* Live game control for development (Linux builds only; off unless an option below is given).
 *
 *   --control PATH       a Unix domain socket at PATH: one command per line, one JSON reply per line
 *                        (src/gamectl_proto.h lists the commands; tools/xbctl is the client)
 *   --control-idle SEC   with --control: quit after SEC seconds without a command (default 600,
 *                        10-86400), so a forgotten session never holds the run slot
 *   --fast-boot          run unpaced (no 60 Hz sleep; the frame-locked clock still moves one period
 *                        per frame, hle_xapi_unthrottled) until the first frame of the level the
 *                        --goto / --warp target loads, then pace normally
 *
 * Threads: the socket thread accepts one client at a time and queues each line (GcQueue); the
 * game thread runs the commands at its Swap (hle_d3d_pace_hook), one at a time in arrival order,
 * Lua chunks at the next lua_call (devlua_host_tick); captures and renderer options run on the host
 * thread after a presented frame (hle_d3d_host_frame_hook). A watchdog thread quits an idle session
 * and ends the process when the game stops presenting frames or a quit does not finish.
 * Every failure is a JSON error reply; nothing a client sends can stop the game except quit.
 *
 * Windows: the options are recognized and refused ("Linux developer builds only"). */
#ifndef GAMECTL_H
#define GAMECTL_H
#include <stddef.h>
#include <stdint.h>

/* One option at argv[*i] (consuming its value): 1 handled, 0 not ours, -1 invalid (message in err). */
int gamectl_parse_arg(int argc, char **argv, int *i, char *err, size_t cap);
/* --control or --fast-boot given. */
int gamectl_wanted(void);
/* What --fast-boot waits for: the last frame of the --goto menu input (0: none), the --goto Lua step
 * finished (need_lua) and a level's first frame (need_level; 0 for the title target). */
void gamectl_boot_plan(uint32_t input_end, int need_lua, int need_level);
/* Sets the hooks and starts the socket and watchdog threads; before hle_devlua_install. 0, or -1
 * with a message in err (the socket could not be created). Nothing when not wanted. */
int gamectl_install(char *err, size_t cap);
/* The level watch (hle_warp.h warp_level_cb): a level load was requested. */
void gamectl_level_loaded(const char *level);
#endif
