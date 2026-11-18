/* Game-independent --goto / developer Lua test: the goto table and its option parsing, the
 * --lua* options, and running chunks against a fake Lua C API (stack restore, results, errors,
 * _ERRORMESSAGE restore, --lua-at, --lua-retry, --lua-pipe). */
#include "../src/hle_goto.h"
#include "../src/hle_devlua.h"
#include "../src/bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

/* ---- fake Lua: a stack of (type, string) and a few globals ---- */
typedef struct { int t; char s[64]; } Val;
static Val stack[64];
static int top;
static Val g_err = {5, "game-handler"}, g_devlua_err = {1, ""};
static int runs;
static char last_code[256];

static int f_gettop(uint32_t L) { return top; }
static void f_settop(uint32_t L, int i) { top = i; }
static Val *at(int i) { return &stack[i < 0 ? top + i + 1 : i]; }
static void f_pushvalue(uint32_t L, int i) { stack[top + 1] = *at(i); top++; }
static int f_type(uint32_t L, int i) { return at(i)->t; }
static const char *f_tostring(uint32_t L, int i) { return at(i)->s; }
static Val *global(const char *n) { return !strcmp(n, "_ERRORMESSAGE") ? &g_err : !strcmp(n, "__devlua_err") ? &g_devlua_err : NULL; }
static void f_getglobal(uint32_t L, const char *n) { Val *g = global(n); stack[++top] = g ? *g : (Val){1, ""}; }
static void f_setglobal(uint32_t L, const char *n) { Val *g = global(n); if (g) *g = stack[top]; top--; }
static int f_dobuffer(uint32_t L, const char *b, size_t n, const char *name) {
    if (strstr(b, "_ERRORMESSAGE = function")) { g_err = (Val){5, "devlua-handler"}; g_devlua_err = (Val){1, ""}; return 0; }
    runs++;
    snprintf(last_code, sizeof last_code, "%.*s", (int)n, b);
    if (!strncmp(b, "ERR", 3)) { g_devlua_err = (Val){3, "boom"}; return 1; }
    if (!strncmp(b, "return ", 7)) {
        stack[++top] = (Val){3, ""};
        snprintf(stack[top].s, sizeof stack[top].s, "%.*s", (int)n - 7, b + 7);
        if (!strcmp(stack[top].s, "nil")) stack[top].t = 1;
        stack[++top] = (Val){2, "42"};
    }
    return 0;
}
static const DevLuaApi api = {f_gettop, f_settop, f_pushvalue, f_type, f_tostring, f_getglobal, f_setglobal, f_dobuffer};

static int parse(int argc, char **argv) {
    char err[192];
    for (int i = 1; i < argc; i++)
        if (devlua_parse_arg(argc, argv, &i, err, sizeof err) != 1) return -1;
    return 0;
}

int main(void) {
    GotoPlan p;
    char err[192];
    /* goto table */
    CHECK(goto_parse("title", &p, err, sizeof err) == 0 && !p.warp_on && !p.lua && p.input && !strcmp(p.place, "title"), "title");
    CHECK(goto_parse("hideout", &p, err, sizeof err) == 0 && !p.warp_on && !p.lua && !strcmp(p.input, BENCH_INPUT), "hideout");
    CHECK(goto_parse("hideout:5", &p, err, sizeof err) == 0 && p.warp_on && !strcmp(p.warp.level->level, "level95") && p.warp.checkpoint == 5, "hideout:5");
    CHECK(goto_parse("hideout:13", &p, err, sizeof err) == -1 && strstr(err, "1-12"), "hideout:13 invalid");
    CHECK(goto_parse("coney", &p, err, sizeof err) == 0 && p.lua && strstr(p.lua, "LeaveClubhouse") && !p.warp_on, "coney");
    CHECK(goto_parse("gym", &p, err, sizeof err) == 0 && p.lua && strstr(p.lua, "fStartWorkout[1]"), "gym");
    CHECK(goto_parse("rumble", &p, err, sizeof err) == 0 && p.lua && strstr(p.lua, "UM_UnlockAll(true)") && strstr(p.lua, "menu.DoRumbleMenu"), "rumble");
    CHECK(goto_parse("aotn", &p, err, sizeof err) == 0 && p.warp_on && !strcmp(p.warp.level->level, "level60") && p.warp.checkpoint == 1 && p.warp.level->mission == 25, "aotn");
    CHECK(goto_parse("aotn:62:2", &p, err, sizeof err) == 0 && !strcmp(p.warp.level->level, "level62") && p.warp.checkpoint == 2, "aotn:62:2");
    CHECK(goto_parse("aotn:level64:4", &p, err, sizeof err) == 0 && !strcmp(p.warp.level->level, "level64") && p.warp.checkpoint == 4, "aotn:level64:4");
    CHECK(goto_parse("aotn:63:4", &p, err, sizeof err) == -1 && strstr(err, "1-3"), "aotn:63:4 invalid");
    CHECK(goto_parse("aotn:65", &p, err, sizeof err) == -1 && strstr(err, "60-64"), "aotn:65 invalid");
    CHECK(goto_parse("m8:3", &p, err, sizeof err) == 0 && p.warp_on && !strcmp(p.warp.level->level, "level81") && p.warp.checkpoint == 3, "m8:3 = --warp 8:3");
    CHECK(goto_parse("m24", &p, err, sizeof err) == -1 && strstr(err, "m1-m23"), "m24 invalid");
    CHECK(goto_parse("moon", &p, err, sizeof err) == -1 && strstr(err, "unknown place"), "unknown place");
    CHECK(goto_parse("gym:2", &p, err, sizeof err) == -1, "gym takes no argument");
    CHECK(!warp_find_level("level60"), "--warp's table unchanged (no AotN levels)");
    {
        int i = 1, on = 0, allow = 0;
        char *av[] = {"xbrun", "--goto", "rumble", "--goto-allow-save", "--goto", NULL};
        CHECK(goto_parse_arg(5, av, &i, &p, &on, &allow, err, sizeof err) == 1 && on && i == 2, "--goto rumble");
        i = 3;
        CHECK(goto_parse_arg(5, av, &i, &p, &on, &allow, err, sizeof err) == 1 && allow, "--goto-allow-save");
        i = 4;
        CHECK(goto_parse_arg(5, av, &i, &p, &on, &allow, err, sizeof err) == -1 && strstr(err, "missing"), "--goto without value");
        char *av2[] = {"xbrun", "--warp", "8", NULL};
        i = 1;
        CHECK(goto_parse_arg(3, av2, &i, &p, &on, &allow, err, sizeof err) == 0 && i == 1, "--warp is not ours");
    }
    {
        char buf[4096];
        FILE *f = fmemopen(buf, sizeof buf, "w");
        goto_list(f);
        fclose(f);
        CHECK(strstr(buf, "rumble") && strstr(buf, "aotn") && strstr(buf, "m<N>[:C]") && strstr(buf, "leaves:"), "--goto list");
    }

    /* --lua* options */
    devlua_reset();
    CHECK(!devlua_wanted(), "nothing asked: no hook");
    {
        char *av[] = {"xbrun", "--lua-exec", "return a", "--lua-at", "100", "--lua-retry", "--lua-exec", "return nil", NULL};
        CHECK(parse(8, av) == 0 && devlua_wanted() && devlua_pending() == 2, "--lua-exec / --lua-at / --lua-retry");
        char *bad[] = {"xbrun", "--lua-at", "x", NULL};
        int i = 1;
        CHECK(devlua_parse_arg(3, bad, &i, err, sizeof err) == -1 && strstr(err, "frame"), "--lua-at x invalid");
        char *miss[] = {"xbrun", "--lua", "/nonexistent/x.lua", NULL};
        i = 1;
        CHECK(devlua_parse_arg(3, miss, &i, err, sizeof err) == -1 && strstr(err, "cannot read"), "--lua missing file");
    }
    /* running: the first chunk runs at the first call, the stack is restored */
    top = 3;
    devlua_tick(&api, 0x1000, 0);
    CHECK(runs == 1 && top == 3 && devlua_pending() == 1, "chunk ran once, stack restored");
    CHECK(!strcmp(g_err.s, "game-handler"), "_ERRORMESSAGE restored");
    devlua_tick(&api, 0x1000, 50);
    CHECK(runs == 1, "--lua-at 100: not before frame 100");
    devlua_tick(&api, 0x1000, 100);
    devlua_tick(&api, 0x1000, 100);
    CHECK(runs == 2 && devlua_pending() == 1, "--lua-retry: once per frame and state, still pending on nil");
    devlua_tick(&api, 0x2000, 100);
    CHECK(runs == 3, "--lua-retry: another state tries in the same frame");
    devlua_tick(&api, 0x1000, 101);
    CHECK(runs == 4 && top == 3, "--lua-retry: next frame again");
    /* devlua_run: results, '=' shorthand, errors */
    {
        int t;
        top = 0;
        CHECK(devlua_run(&api, 1, "=GetCheckPoint()", 16, "t", &t) == 0 && t && !strcmp(last_code, "return GetCheckPoint()") && top == 0, "'=EXPR' is return EXPR");
        CHECK(devlua_run(&api, 1, "ERR", 3, "t", &t) == 1 && !t && top == 0 && !strcmp(g_err.s, "game-handler"), "error: status, handler restored");
        CHECK(devlua_run(&api, 1, "x = 1", 5, "t", &t) == 0 && !t, "no results: not truthy");
    }
    /* --lua-pipe: complete lines run once, an unfinished line waits */
    {
        devlua_reset();
        char path[] = "/tmp/hle_goto_test_pipe_XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0, "temp file");
        FILE *f = fdopen(fd, "w");
        fputs("return p1\nreturn p2\nreturn p3", f);
        fflush(f);
        char *av[] = {"xbrun", "--lua-pipe", path, NULL};
        CHECK(parse(3, av) == 0 && devlua_wanted(), "--lua-pipe");
        runs = 0;
        devlua_tick(&api, 1, 10);
        CHECK(runs == 2, "pipe: two complete lines");
        devlua_tick(&api, 1, 10);
        CHECK(runs == 2, "pipe: polled once per frame");
        fputs("\n", f);
        fflush(f);
        devlua_tick(&api, 1, 11);
        CHECK(runs == 3 && !strcmp(last_code, "return p3"), "pipe: the finished line runs");
        devlua_tick(&api, 1, 12);
        CHECK(runs == 3, "pipe: nothing new");
        fclose(f);
        unlink(path);
    }
    devlua_reset();
    printf("%s\n", fails ? "hle_goto_test: FAILED" : "hle_goto_test: OK");
    return fails != 0;
}
