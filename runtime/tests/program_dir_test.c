/* Game-independent plat_program_dir test (Linux): the folder of the running program, and the
 * XBRUN_PROGRAM_DIR override the AppImage sets (tools/appimage/xbrun.sh). There xbrun starts through
 * the bundled ld-linux.so.2, so /proc/self/exe names the loader, not the game: the override names the
 * folder the game's own files (mods, photos, gamecontrollerdb.txt) live in. */
#include "../src/platform.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what, const char *got) {
    if (!ok) fails++;
    printf("%s %s (%s)\n", ok ? "ok  " : "FAIL", what, got);
}

int main(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) slash[1] = 0;

    unsetenv("XBRUN_PROGRAM_DIR");
    check(!strcmp(plat_program_dir(), exe), "unset: the folder of /proc/self/exe", plat_program_dir());
    setenv("XBRUN_PROGRAM_DIR", "", 1);
    check(!strcmp(plat_program_dir(), exe), "empty: the folder of /proc/self/exe", plat_program_dir());
    setenv("XBRUN_PROGRAM_DIR", "/opt/games/WarriorsReborn-data/", 1);
    check(!strcmp(plat_program_dir(), "/opt/games/WarriorsReborn-data/"), "set: the given folder", plat_program_dir());
    setenv("XBRUN_PROGRAM_DIR", "/opt/games/WarriorsReborn-data", 1);
    check(!strcmp(plat_program_dir(), "/opt/games/WarriorsReborn-data/"), "set without a slash: one added", plat_program_dir());
    printf(fails ? "program_dir_test: %d FAILED\n" : "program_dir_test: all passed\n", fails);
    return fails != 0;
}
