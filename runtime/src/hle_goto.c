/* --goto PLACE (see hle_goto.h). */
#include "hle_goto.h"
#include "bench.h"
#include <stdlib.h>
#include <string.h>

#define TITLE_INPUT "3200:start:10"
/* The Rumble menu opens about frame 4050 (hub loaded ~3860 + the 2.5 s of scheduled steps);
 * three A presses: Rumble -> Game Mode (1 on 1) -> Game Type (1P vs CPU) -> Choose Gangs. */
#define RUMBLE_INPUT BENCH_INPUT ",4700:a,5100:a,5500:a"

/* Armies of the Night: levelNames fields 4 (chapters) and 5 (mission 25). Not story missions,
 * so --warp does not list them. */
static const WarpLevel aotn_levels[] = {
    {"level60", 25, 3}, {"level61", 25, 3}, {"level62", 25, 3}, {"level63", 25, 3}, {"level64", 25, 4},
};

const WarpLevel *goto_find_aotn(int level) {
    for (size_t i = 0; i < sizeof aotn_levels / sizeof aotn_levels[0]; i++)
        if (atoi(aotn_levels[i].level + 5) == level) return &aotn_levels[i];
    return NULL;
}

/* Hub Lua (level95.lua): ENV == clubhouse once the hideout's script has run; player is the
 * player's human. That script runs while the level is still loading (and the hub flushes its
 * scheduled functions on the way), so a step acts at the first try 3 s of game time
 * (GetGameTime, ms) after the hideout is up, and returns 1 then. */
#define IN_HIDEOUT "ENV and clubhouse and ENV == clubhouse and player and HuIsAlive(player) and HuIsAPlayer(player)"
#define LATER(body) "__goto_t = __goto_t or GetGameTime() if GetGameTime() - __goto_t >= 3000 then " body " return 1 end end"
static const char LUA_CONEY[] =
    "if " IN_HIDEOUT " and env and env.LeaveClubhouse and fClubexit then "
    LATER("env.LeaveClubhouse(fClubexit[1], player)");
static const char LUA_GYM[] =
    "if " IN_HIDEOUT " and fStartWorkout then "
    LATER("TeleportToFlag(player, fStartWorkout[1], -1)");
/* menu.MenuAction's Rumble branch without its flag test, then the menu itself. */
static const char LUA_RUMBLE[] =
    "if " IN_HIDEOUT " and menu and menu.DoRumbleMenu then "
    LATER("UM_UnlockAll(true) menu.InMenu = true ScreenQueueEffect(1, 0.5) events.Mute(true, false) "
          "gangs.RemoveScaredChicks() DeactivateWarchief() setDetailFlag(2, 1) SndSetListener(1) "
          "ScheduleFunc(\"menu.DoRumbleMenu\", 500)");

typedef struct {
    const char *name, *args, *what, *leaves;
} GotoInfo;

static const GotoInfo places[] = {
    {"title", "", "main menu after the title screen (boot movies skipped)", "main menu, no profile loaded; save not written"},
    {"hideout", "[:C]", "the hub (level95) inside the hideout: Continue at the save's checkpoint, or hub checkpoint C 1-12 (warped)",
     "hideout, player free; save not written"},
    {"coney", "", "the hub, then out of the hideout door (env.LeaveClubhouse) onto the Coney streets",
     "Coney streets outside the hideout; save not written"},
    {"gym", "", "the hub, player teleported to the heavy-bag workout flag", "at the workout station (press A to start); save not written"},
    {"rumble", "", "the hub, UM_UnlockAll(true), the Rumble menu, then A x3: 1 on 1, 1P vs CPU",
     "Choose Gangs (about frame 5600), every gang unlocked in memory only; save not written"},
    {"aotn", "[:L[:C]]", "Armies of the Night level L 60-64 (default 60), chapter C (default 1; 1-3, level64 1-4)",
     "AotN level playing; save not written"},
    {"m", "<N>[:C]", "story mission N 1-23 at checkpoint C (the same as --warp N[:C])", "mission playing; save not written"},
};

void goto_list(FILE *f) {
    fprintf(f, "--goto targets (each run skips the boot movies and presses through the menus; the save on disk is never written):\n");
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++)
        fprintf(f, "  %s%-*s %s\n      leaves: %s\n", places[i].name, (int)(14 - strlen(places[i].name)), places[i].args,
                places[i].what, places[i].leaves);
}

static const char *leaves_of(const char *name) {
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++)
        if (!strcmp(places[i].name, name)) return places[i].leaves;
    return "";
}

/* Decimal 1..4 digits. */
static int num(const char *s, size_t n, int *v) {
    if (n < 1 || n > 4) return 0;
    int x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (s[i] - '0');
    }
    *v = x;
    return 1;
}

static int plan(GotoPlan *p, const char *place, const char *input, const char *lua) {
    memset(p, 0, sizeof *p);
    p->place = place;
    p->input = input;
    p->lua = lua;
    p->leaves = leaves_of(place);
    return 0;
}

int goto_parse(const char *s, GotoPlan *p, char *err, size_t cap) {
    const char *colon = strchr(s, ':');
    size_t head = colon ? (size_t)(colon - s) : strlen(s);
    const char *rest = colon ? colon + 1 : NULL;
#define IS(w) (head == strlen(w) && !strncmp(s, w, head))
    if (IS("title") && !rest) return plan(p, "title", TITLE_INPUT, NULL);
    if (IS("coney") && !rest) return plan(p, "coney", BENCH_INPUT, LUA_CONEY);
    if (IS("gym") && !rest) return plan(p, "gym", BENCH_INPUT, LUA_GYM);
    if (IS("rumble") && !rest) return plan(p, "rumble", RUMBLE_INPUT, LUA_RUMBLE);
    if (IS("hideout")) {
        plan(p, "hideout", BENCH_INPUT, NULL);
        if (!rest) return 0;
        int cp;
        const WarpLevel *hub = warp_find_level("level95");
        if (!num(rest, strlen(rest), &cp) || cp < 1 || cp > hub->chapters) {
            snprintf(err, cap, "--goto %s: the hub checkpoint must be 1-%d", s, hub->chapters);
            return -1;
        }
        p->warp.level = hub;
        p->warp.checkpoint = cp;
        p->warp_on = 1;
        return 0;
    }
    if (IS("aotn")) {
        int lv = 60, cp = 1;
        if (rest) {
            const char *c2 = strchr(rest, ':');
            size_t n = c2 ? (size_t)(c2 - rest) : strlen(rest);
            const char *ln = rest;
            if (n > 5 && !strncmp(rest, "level", 5)) ln += 5, n -= 5;
            if (!num(ln, n, &lv) || !goto_find_aotn(lv)) {
                snprintf(err, cap, "--goto %s: Armies of the Night levels are 60-64 (or level60-level64)", s);
                return -1;
            }
            if (c2 && !num(c2 + 1, strlen(c2 + 1), &cp)) cp = 0;
        }
        const WarpLevel *l = goto_find_aotn(lv);
        if (cp < 1 || cp > l->chapters) {
            snprintf(err, cap, "--goto %s: %s has chapters 1-%d", s, l->level, l->chapters);
            return -1;
        }
        plan(p, "aotn", BENCH_INPUT, NULL);
        p->warp.level = l;
        p->warp.checkpoint = cp;
        p->warp_on = 1;
        return 0;
    }
    if (s[0] == 'm' && s[1] >= '0' && s[1] <= '9') {
        plan(p, "m", BENCH_INPUT, NULL);
        char e[192];
        if (warp_parse(s + 1, &p->warp, e, sizeof e)) {
            snprintf(err, cap, "--goto %s: story missions are m1-m23, with an optional checkpoint (e.g. m8:3)", s);
            return -1;
        }
        p->warp_on = 1;
        return 0;
    }
#undef IS
    snprintf(err, cap, "--goto %s: unknown place (title, hideout[:C], coney, gym, rumble, aotn[:L[:C]], m<N>[:C]; --goto list)", s);
    return -1;
}

int goto_parse_arg(int argc, char **argv, int *i, GotoPlan *p, int *on, int *allow_save, char *err, size_t cap) {
    if (!strcmp(argv[*i], "--goto-allow-save")) {
        *allow_save = 1;
        return 1;
    }
    if (strcmp(argv[*i], "--goto")) return 0;
    if (*i + 1 >= argc) {
        snprintf(err, cap, "--goto: missing value (a place; --goto list prints them)");
        return -1;
    }
    if (goto_parse(argv[++*i], p, err, cap)) return -1;
    *on = 1;
    return 1;
}
