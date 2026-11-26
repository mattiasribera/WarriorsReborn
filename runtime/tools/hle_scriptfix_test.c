/* Game-independent script-fix test: the --no-script-fixes / --script-fixes options, the chunk
 * name normalization, which fix runs after which script (only level14, only while the pass is
 * on, ScriptFixLog bound first) against a fake Lua C API, the stack left as found, and the set-up
 * chunk's contract (the title functions it wraps by global name, the 15 s check, the two calls it
 * makes and the log line). */
#include "../src/hle_scriptfix.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

/* ---- fake Lua: a stack of (type, string) and the two globals the runner swaps ---- */
typedef struct { int t; char s[64]; } Val;
static Val stack[64];
static int top;
static Val g_err = {5, "game-handler"}, g_devlua_err = {1, ""};
static char ran[512];      /* the chunk names run, space separated (not the devlua prelude) */
static const char *last_code;
static int binds;
static uint32_t bound_L;

static int f_gettop(uint32_t L) { (void)L; return top; }
static void f_settop(uint32_t L, int i) { (void)L; top = i; }
static Val *at(int i) { return &stack[i < 0 ? top + i + 1 : i]; }
static void f_pushvalue(uint32_t L, int i) { (void)L; stack[top + 1] = *at(i); top++; }
static int f_type(uint32_t L, int i) { (void)L; return at(i)->t; }
static const char *f_tostring(uint32_t L, int i) { (void)L; return at(i)->s; }
static Val *global(const char *n) { return !strcmp(n, "_ERRORMESSAGE") ? &g_err : !strcmp(n, "__devlua_err") ? &g_devlua_err : NULL; }
static void f_getglobal(uint32_t L, const char *n) { (void)L; Val *g = global(n); stack[++top] = g ? *g : (Val){1, ""}; }
static void f_setglobal(uint32_t L, const char *n) { (void)L; Val *g = global(n); if (g) *g = stack[top]; top--; }
static int f_dobuffer(uint32_t L, const char *b, size_t n, const char *name) {
    (void)L; (void)n;
    if (strstr(b, "_ERRORMESSAGE = function")) { g_err = (Val){5, "devlua-handler"}; return 0; }
    size_t l = strlen(ran);
    snprintf(ran + l, sizeof ran - l, "%s%s", l ? " " : "", name);
    last_code = b;
    stack[++top] = (Val){2, "1"};   /* a leftover result the runner must pop */
    return 0;
}
static const DevLuaApi api = {f_gettop, f_settop, f_pushvalue, f_type, f_tostring, f_getglobal, f_setglobal, f_dobuffer};
static void bind(uint32_t L) { binds++; bound_L = L; }

static void reset(void) { ran[0] = 0; last_code = NULL; binds = 0; bound_L = 0; top = 0; g_err = (Val){5, "game-handler"}; }

int main(void) {
    int on = -1;
    CHECK(scriptfix_parse_arg("--no-script-fixes", &on) == 1 && on == 0, "--no-script-fixes parsed, disables");
    CHECK(scriptfix_parse_arg("--script-fixes", &on) == 1 && on == 1, "--script-fixes parsed, enables");
    CHECK(scriptfix_parse_arg("--no-text-fixes", &on) == 0 && on == 1, "other options are not taken");
    CHECK(scriptfix_enabled() == 1, "on by default");

    char s[32];
    CHECK(scriptfix_script_name("=level14.lua", s, sizeof s) && !strcmp(s, "level14"), "=level14.lua -> level14");
    CHECK(scriptfix_script_name("@scripts/level14.lua", s, sizeof s) && !strcmp(s, "level14"), "@scripts/level14.lua -> level14");
    CHECK(scriptfix_script_name("level14", s, sizeof s) && !strcmp(s, "level14"), "level14 stays");
    CHECK(!scriptfix_script_name("=.lua", s, sizeof s), "empty name refused");

    /* The table: set-up follows level14 only. */
    int setup = -1;
    for (int i = 0; i < scriptfix_count; i++)
        if (!strcmp(scriptfix_table[i].name, "set-up")) setup = i;
    CHECK(setup >= 0 && !strcmp(scriptfix_table[setup].script, "level14"), "set-up follows level14");

    /* Enabled: level14 runs the set-up chunk once, ScriptFixLog bound first, stack restored. */
    reset();
    CHECK(scriptfix_on_script(&api, 0x1000, "=level14.lua", bind) == 1, "level14: one fix run");
    CHECK(!strcmp(ran, "=scriptfix/set-up"), "level14: chunk =scriptfix/set-up");
    CHECK(binds == 1 && bound_L == 0x1000, "level14: ScriptFixLog bound in that state");
    CHECK(top == 0, "level14: stack restored");
    CHECK(!strcmp(g_err.s, "game-handler"), "level14: the game's _ERRORMESSAGE restored");

    /* Other scripts: nothing. */
    static const char *others[] = {"=level140.lua", "=level1.lua", "=level14_chapter2.lua", "=global.lua", "config_preload"};
    for (size_t i = 0; i < sizeof others / sizeof *others; i++) {
        reset();
        char what[96];
        snprintf(what, sizeof what, "%s: no fix run", others[i]);
        CHECK(scriptfix_on_script(&api, 0x1000, others[i], bind) == 0 && !ran[0] && !binds, what);
    }

    /* The set-up chunk: wraps by global name, checks after 15 s, makes him hostile, logs. */
    const char *c = scriptfix_table[setup].code;
    CHECK(strstr(c, "SFX_TwoDudes = TwoDudes") && strstr(c, "TwoDudes = SFX_TwoDudesW"), "set-up: wraps TwoDudes by global name");
    CHECK(strstr(c, "SFX_StartPoizo = StartPoizo") && strstr(c, "StartPoizo = SFX_StartPoizoW"), "set-up: wraps StartPoizo");
    CHECK(strstr(c, "SFX_TwoDudesAttacks = TwoDudesAttacks") && strstr(c, "TwoDudesAttacks = SFX_TwoDudesAttacksW"),
          "set-up: wraps TwoDudesAttacks");
    CHECK(strstr(c, "SFX_ReversePoizo = ReversePoizo") && strstr(c, "ReversePoizo = SFX_ReversePoizoW"), "set-up: wraps ReversePoizo");
    CHECK(strstr(c, "TwoDudes ~= SFX_TwoDudesW"), "set-up: a second run in the same state does not wrap a wrapper");
    CHECK(strstr(c, "ScheduleFunc(\"SFX_TDCheck\", 15000)"), "set-up: the check runs 15 s after the Constructor");
    CHECK(strstr(c, "HuIsInScene(player)"), "set-up: waits while a scene plays");
    CHECK(strstr(c, "BrDead(TwoDudes.TD01, false)") && strstr(c, "PURSUE(GangTwoDudes)"), "set-up: makes the machete guy hostile");
    CHECK(strstr(c, "\"set-up: machete guy made hostile (cutscene did not start)\""), "set-up: the log line");
    CHECK(!strchr(c, '%') && !strstr(c, "local function"), "set-up: Lua 4.0 (no upvalues, no local function)");

    /* Disabled: nothing runs, not even the bind. */
    scriptfix_set_enabled(0);
    CHECK(scriptfix_enabled() == 0, "disabled");
    reset();
    CHECK(scriptfix_on_script(&api, 0x1000, "=level14.lua", bind) == 0 && !ran[0] && !binds, "disabled: level14 runs nothing");
    scriptfix_set_enabled(1);
    reset();
    CHECK(scriptfix_on_script(&api, 0x2000, "=level14.lua", NULL) == 1 && !strcmp(ran, "=scriptfix/set-up"),
          "enabled again, no bind: runs");

    printf("%d checks, %s\n", checks, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
