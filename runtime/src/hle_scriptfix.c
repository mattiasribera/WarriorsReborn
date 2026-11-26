/* Script fixes: Lua chunks run after a level script loads (see hle_scriptfix.h). */
#include "hle_scriptfix.h"
#include <stdio.h>
#include <string.h>

/* level14 "Set Up": the machete guy (TwoDudes.TD01) stays brain-dead when the scene l14_t1_009
 * never starts. SFX_TDDone: TwoDudesAttacks or ReversePoizo ran (he is hostile, nothing to do);
 * SFX_TDStarted: StartPoizo ran (the scene began). The check waits while the player is in a scene
 * and gives a scene that began 2 s more after it ends, for its ReturnFunc. */
static const char SET_UP[] =
    "function SFX_TwoDudesW()\n"
    "  local g, c = SFX_TwoDudes()\n"
    "  if not c then\n"
    "    return g\n"
    "  end\n"
    "  SFX_TDCtor = c\n"
    "  return g, SFX_TDGuard\n"
    "end\n"
    "function SFX_TDGuard(p0)\n"
    "  SFX_TDDone = nil\n"
    "  SFX_TDStarted = nil\n"
    "  SFX_TDGrace = nil\n"
    "  SFX_TDCtor(p0)\n"
    "  ScheduleFunc(\"SFX_TDCheck\", 15000)\n"
    "end\n"
    "function SFX_StartPoizoW(p0)\n"
    "  SFX_TDStarted = 1\n"
    "  return SFX_StartPoizo(p0)\n"
    "end\n"
    "function SFX_TwoDudesAttacksW(p0)\n"
    "  SFX_TDDone = 1\n"
    "  return SFX_TwoDudesAttacks(p0)\n"
    "end\n"
    "function SFX_ReversePoizoW()\n"
    "  SFX_TDDone = 1\n"
    "  return SFX_ReversePoizo()\n"
    "end\n"
    "function SFX_TDCheck()\n"
    "  if SFX_TDDone then\n"
    "    return\n"
    "  end\n"
    "  if HuIsInScene(player) then\n"
    "    ScheduleFunc(\"SFX_TDCheck\", 1000)\n"
    "    return\n"
    "  end\n"
    "  if SFX_TDStarted and not SFX_TDGrace then\n"
    "    SFX_TDGrace = 1\n"
    "    ScheduleFunc(\"SFX_TDCheck\", 2000)\n"
    "    return\n"
    "  end\n"
    "  SFX_TDDone = 1\n"
    "  if type(TwoDudes) == \"table\" and TwoDudes.TD01 then\n"
    "    BrDead(TwoDudes.TD01, false)\n"
    "  end\n"
    "  PURSUE(GangTwoDudes)\n"
    "  if ScriptFixLog then\n"
    "    if SFX_TDStarted then\n"
    "      ScriptFixLog(\"set-up: machete guy made hostile (cutscene did not finish)\")\n"
    "    else\n"
    "      ScriptFixLog(\"set-up: machete guy made hostile (cutscene did not start)\")\n"
    "    end\n"
    "  end\n"
    "end\n"
    "if type(TwoDudes) == \"function\" and TwoDudes ~= SFX_TwoDudesW and type(StartPoizo) == \"function\" and "
    "type(TwoDudesAttacks) == \"function\" and type(ReversePoizo) == \"function\" then\n"
    "  SFX_TwoDudes = TwoDudes\n"
    "  TwoDudes = SFX_TwoDudesW\n"
    "  SFX_StartPoizo = StartPoizo\n"
    "  StartPoizo = SFX_StartPoizoW\n"
    "  SFX_TwoDudesAttacks = TwoDudesAttacks\n"
    "  TwoDudesAttacks = SFX_TwoDudesAttacksW\n"
    "  SFX_ReversePoizo = ReversePoizo\n"
    "  ReversePoizo = SFX_ReversePoizoW\n"
    "end\n";

const ScriptFix scriptfix_table[] = {
    {"set-up", "level14", SET_UP},
};
const int scriptfix_count = (int)(sizeof scriptfix_table / sizeof scriptfix_table[0]);

static int enabled = 1;

int scriptfix_parse_arg(const char *arg, int *on) {
    if (!strcmp(arg, "--no-script-fixes")) { *on = 0; return 1; }
    if (!strcmp(arg, "--script-fixes")) { *on = 1; return 1; }
    return 0;
}

void scriptfix_set_enabled(int on) {
    enabled = on != 0;
    if (!enabled) {
        printf("[scriptfix] disabled (--no-script-fixes): level scripts run as shipped\n");
    } else {
        char list[256];
        size_t n = 0;
        list[0] = 0;
        for (int i = 0; i < scriptfix_count && n + 1 < sizeof list; i++) {
            int w = snprintf(list + n, sizeof list - n, "%s%s (after %s)", i ? ", " : "", scriptfix_table[i].name,
                             scriptfix_table[i].script);
            if (w > 0) n += (size_t)w < sizeof list - n ? (size_t)w : sizeof list - n - 1;
        }
        printf("[scriptfix] on: %s\n", list);
    }
    fflush(stdout);
}

int scriptfix_enabled(void) { return enabled; }

int scriptfix_script_name(const char *name, char *out, size_t cap) {
    if (!name || !cap) return 0;
    if (*name == '=' || *name == '@') name++;
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    size_t n = strlen(name);
    if (n >= 4 && !strcmp(name + n - 4, ".lua")) n -= 4;
    if (!n || n >= cap) return 0;
    memcpy(out, name, n);
    out[n] = 0;
    return 1;
}

int scriptfix_on_script(const DevLuaApi *api, uint32_t L, const char *name, void (*bind)(uint32_t L)) {
    char script[64];
    if (!enabled || !L || !scriptfix_script_name(name, script, sizeof script)) return 0;
    int ran = 0, bound = 0;
    for (int i = 0; i < scriptfix_count; i++) {
        const ScriptFix *f = &scriptfix_table[i];
        if (strcmp(f->script, script)) continue;
        if (bind && !bound) bind(L), bound = 1;
        char chunk[64], err[384];
        int truthy;
        snprintf(chunk, sizeof chunk, "=scriptfix/%s", f->name);
        if (devlua_run_ex(api, L, f->code, strlen(f->code), chunk, NULL, &truthy, err, sizeof err)) {
            printf("[scriptfix] %s: error after %s: %s\n", f->name, script, err);
            fflush(stdout);
        }
        ran++;
    }
    return ran;
}
