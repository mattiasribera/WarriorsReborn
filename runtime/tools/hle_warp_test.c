/* Game-independent --warp test: option parsing and validation, the mission -> level table,
 * the all-or-nothing signature check on a planted buffer, and the two stubs executed for real
 * in host executable memory: a fake MenuLoadLevel jumps to the load stub (one-shot redirect
 * with the checkpoint call, later loads unchanged, "menu" never redirected), and a fake caller
 * calls the save stub (nothing written, success returned, stack balanced), and the level watch
 * stub runs in front of a fake level load (name reported, the load unchanged). */
#include "../src/hle_warp.h"
#include "../src/platform.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int parse_ok(const char *s, const char *level, int cp) {
    WarpTarget t = {0};
    char err[160] = "";
    return warp_parse(s, &t, err, sizeof err) == 0 && t.level && !strcmp(t.level->level, level) && t.checkpoint == cp;
}
static int parse_bad(const char *s) {
    WarpTarget t = {0};
    char err[160] = "";
    return warp_parse(s, &t, err, sizeof err) == -1 && err[0];
}

/* Fakes called by the stubs (cdecl, from code with an unaligned stack: -mstackrealign). */
static char loaded[32];
static int loads, setcps, last_cp;
static void fake_load(const char *name) { snprintf(loaded, sizeof loaded, "%s", name); loads++; }
static void fake_setcp(int cp) { last_cp = cp; setcps++; }
static char watched_level[32];
static int watched, watched_mission;
static void watch_cb(const char *level, int mission) {
    snprintf(watched_level, sizeof watched_level, "%s", level);
    watched_mission = mission;
    watched++;
}

static char hook_name[32];
static int hook_calls;
static void level_seen(const char *name) {
    snprintf(hook_name, sizeof hook_name, "%s", name);
    hook_calls++;
}

int main(void) {
    char err[160];

    /* Table: the game's numbering (levelNames field 5) and chapter counts (field 4). */
    CHECK(warp_find_mission(1) && !strcmp(warp_find_mission(1)->level, "level99"), "mission 1 = level99");
    CHECK(warp_find_mission(5) && !strcmp(warp_find_mission(5)->level, "level2"), "mission 5 = level2");
    CHECK(warp_find_mission(8) && !strcmp(warp_find_mission(8)->level, "level81") && warp_find_mission(8)->chapters == 5,
          "mission 8 = level81, 5 chapters");
    CHECK(warp_find_mission(23) && !strcmp(warp_find_mission(23)->level, "level11"), "mission 23 = level11");
    CHECK(!warp_find_mission(0) && !warp_find_mission(24), "no mission 0 or 24");
    CHECK(warp_find_level("level95") && warp_find_level("level95")->mission == 0, "level95 (hub) by name only");
    CHECK(!warp_find_level("level7") && !warp_find_level("level8"), "unknown level names rejected");
    int dup = 0;
    for (int i = 0; i < warp_level_count; i++)
        for (int j = i + 1; j < warp_level_count; j++)
            if (!strcmp(warp_levels[i].level, warp_levels[j].level) ||
                (warp_levels[i].mission && warp_levels[i].mission == warp_levels[j].mission)) dup++;
    CHECK(warp_level_count == 24 && !dup, "24 entries, no duplicate name or mission");

    /* Parsing. */
    CHECK(parse_ok("8", "level81", 1), "8 -> level81 checkpoint 1 (default)");
    CHECK(parse_ok("8:3", "level81", 3), "8:3 -> level81 checkpoint 3");
    CHECK(parse_ok("level81:5", "level81", 5), "level81:5");
    CHECK(parse_ok("1", "level99", 1) && parse_ok("5:4", "level2", 4), "1, 5:4");
    CHECK(parse_ok("level95:12", "level95", 12), "hub by name with a checkpoint");
    CHECK(parse_bad("0") && parse_bad("24") && parse_bad("99"), "out-of-range missions rejected");
    CHECK(parse_bad("8:0") && parse_bad("8:6") && parse_bad("5:5"), "out-of-range checkpoints rejected");
    CHECK(parse_bad("") && parse_bad("abc") && parse_bad("8:") && parse_bad(":3") && parse_bad("8:3x") &&
          parse_bad("8x") && parse_bad("-8") && parse_bad("+8") && parse_bad("8:3:1") && parse_bad("level7") &&
          parse_bad("LEVEL81") && parse_bad("99999999999"), "malformed values rejected");
    WarpTarget t = {0};
    CHECK(warp_parse("level7", &t, err, sizeof err) == -1 && strstr(err, "level7"), "message names the value: %s", err);

    /* Option. */
    int en = 0, i = 1;
    char *av1[] = {"xbrun", "--warp", "8:3", NULL};
    CHECK(warp_parse_arg(3, av1, &i, &t, &en, err, sizeof err) == 1 && en && i == 2 && t.checkpoint == 3, "--warp 8:3");
    char *av2[] = {"xbrun", "--warp", NULL};
    i = 1; en = 0;
    CHECK(warp_parse_arg(2, av2, &i, &t, &en, err, sizeof err) == -1 && !en && err[0], "--warp without a value");
    char *av3[] = {"xbrun", "--warp", "x", NULL};
    i = 1;
    CHECK(warp_parse_arg(3, av3, &i, &t, &en, err, sizeof err) == -1 && !en, "--warp x invalid");
    char *av4[] = {"xbrun", "--warpx", NULL};
    i = 1;
    CHECK(warp_parse_arg(2, av4, &i, &t, &en, err, sizeof err) == 0 && i == 1, "other options untouched");

    /* Signatures: the title table, then all-or-nothing against a planted buffer. */
    int sites = 0;
    for (int k = 0; k < warp_sig_count; k++) {
        if (warp_sigs[k].va <= WARP_LOAD_SITE && WARP_LOAD_SITE + 6 <= warp_sigs[k].va + warp_sigs[k].len) sites |= 1;
        if (warp_sigs[k].va <= WARP_SAVE_SITE && WARP_SAVE_SITE + 6 <= warp_sigs[k].va + warp_sigs[k].len) sites |= 2;
        if (warp_sigs[k].va == WARP_SETCP) sites |= 4;
        if (warp_sigs[k].va == WARP_LOAD) sites |= 8;
    }
    CHECK(sites == 15, "signatures cover both patch sites and both called title functions");
    enum { BASE = 0x00010000u, SIZE = 0x00340000u };
    static uint8_t img[SIZE];
    intptr_t bias = (intptr_t)((uintptr_t)img - BASE);
    for (int k = 0; k < warp_sig_count; k++) memcpy(img + (warp_sigs[k].va - BASE), warp_sigs[k].orig, warp_sigs[k].len);
    char bad[256];
    CHECK(warp_check(warp_sigs, warp_sig_count, bias, bad, sizeof bad) == 0 && !bad[0], "planted title matches");
    img[WARP_SETCP - BASE + 3] ^= 0xff;
    CHECK(warp_check(warp_sigs, warp_sig_count, bias, bad, sizeof bad) == 1 && strstr(bad, "00337480"),
          "one mismatch reported: %s", bad);
    CHECK(hle_warp_install(0, NULL) == 0, "disabled: nothing armed");

    /* Executable test: a fake MenuLoadLevel and the load stub. */
    uint8_t *x = plat_alloc_exec(4096);
    CHECK(x != NULL, "executable memory");
    if (!x) return 1;
    static char hub[] = "level95", menu[] = "menu";
    uint8_t *stub = x + 256, *fn = x, *p = fn;
    /* fn(name): mov eax,[esp+4]; push 0; push 1; push eax (the name call's arguments);
     * jmp stub; ret_pt: add esp,0x10; xor eax,eax; ret */
    *p++ = 0x8b; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;
    *p++ = 0x6a; *p++ = 0x00; *p++ = 0x6a; *p++ = 0x01; *p++ = 0x50;
    uint8_t *site = p;
    p += 6;
    uint32_t ret_va = (uint32_t)(uintptr_t)p;
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x10; *p++ = 0x33; *p++ = 0xc0; *p++ = 0xc3;
    size_t n = warp_build_load_stub(stub, (uint32_t)(uintptr_t)warp_on_load, (uint32_t)(uintptr_t)fake_setcp,
                                    (uint32_t)(uintptr_t)fake_load, ret_va);
    CHECK(n > 0 && n <= WARP_STUB_MAX, "load stub built (%u bytes)", (unsigned)n);
    warp_write_jmp(site, (uint32_t)(uintptr_t)site, (uint32_t)(uintptr_t)stub, 6);
    CHECK(site[0] == 0xe9 && site[5] == 0x90, "site: jmp rel32 + nop");
    int (*call_fn)(const char *) = (int (*)(const char *))(void *)fn;

    warp_level_hook = level_seen;
    CHECK(call_fn(hub) == 0 && loads == 1 && !strcmp(loaded, "level95") && setcps == 0, "not armed: name unchanged");
    CHECK(!strcmp(hook_name, "level95") && hook_calls == 1, "not armed (level tracking only): the hook sees the name loaded");
    CHECK(!warp_level_pending(), "no game object: no pending load");
    WarpTarget w;
    warp_parse("8:3", &w, err, sizeof err);
    warp_arm(&w);
    CHECK(call_fn(menu) == 0 && !strcmp(loaded, "menu") && setcps == 0 && warp_redirects() == 0,
          "armed: \"menu\" is not redirected");
    CHECK(call_fn(hub) == 0 && loads == 3 && !strcmp(loaded, "level81") && setcps == 1 && last_cp == 3 && warp_redirects() == 1,
          "armed: first level load -> level81, SetCheckPoint(3)");
    CHECK(!strcmp(hook_name, "level81") && hook_calls == 3, "the hook sees the redirected name (%s)", hook_name);
    /* Game object stand-in: [g + 0x148] = 3 while the load is pending. */
    static uint8_t game[0x200];
    static uint32_t game_ptr;
    game_ptr = (uint32_t)(uintptr_t)game;
    warp_game_ptr = &game_ptr;
    game[WARP_STATE_OFF] = WARP_STATE_PENDING;
    CHECK(warp_level_pending(), "warp_level_pending: [g + 0x148] == 3");
    CHECK(call_fn(hub) == 0 && loads == 4 && !strcmp(loaded, "level81") && setcps == 2 && last_cp == 3 && warp_redirects() == 2,
          "a request replacing the pending one is redirected too");
    game[WARP_STATE_OFF] = 0;
    CHECK(call_fn(hub) == 0 && loads == 5 && !strcmp(loaded, "level95") && setcps == 2,
          "one-shot: once the load started, the next load (mission end -> hub) is unchanged");
    game[WARP_STATE_OFF] = WARP_STATE_PENDING;
    CHECK(call_fn(hub) == 0 && !strcmp(loaded, "level95") && setcps == 2, "a later pending load is not redirected");

    /* Save stub: fake caller `mov ecx,obj; push 7; call stub; mov eax? ret` keeps al. */
    static uint8_t obj[0x100];
    uint8_t *save = x + 512, *caller = x + 768;
    n = warp_build_save_stub(save, (uint32_t)(uintptr_t)warp_on_save);
    CHECK(n > 0 && n <= WARP_STUB_MAX, "save stub built (%u bytes)", (unsigned)n);
    p = caller;
    /* push esi; mov esi,esp; mov ecx,obj; xor eax,eax; push 7; call save; sub esi,esp
     * (0 when ret 4 balanced the stack); movzx: eax = al | esi << 8; pop esi; ret */
    *p++ = 0x56; *p++ = 0x89; *p++ = 0xe6;
    uint32_t o = (uint32_t)(uintptr_t)obj;
    *p++ = 0xb9; memcpy(p, &o, 4); p += 4;
    *p++ = 0x31; *p++ = 0xc0;
    *p++ = 0x6a; *p++ = 0x07;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)save - ((uint32_t)(uintptr_t)p + 5));
    *p++ = 0xe8; memcpy(p, &rel, 4); p += 4;
    *p++ = 0x29; *p++ = 0xe6;                    /* sub esi,esp */
    *p++ = 0xc1; *p++ = 0xe6; *p++ = 0x08;       /* shl esi,8 */
    *p++ = 0x0f; *p++ = 0xb6; *p++ = 0xc0;       /* movzx eax,al */
    *p++ = 0x09; *p++ = 0xf0;                    /* or eax,esi */
    *p++ = 0x5e; *p++ = 0xc3;
    int (*call_save)(void) = (int (*)(void))(void *)caller;
    int r = call_save();
    CHECK(r == 1 && obj[0xd5] == 1 && warp_saves_blocked() == 1, "save stub: returns 1, stack balanced, result byte set (r=%x)", r);

    game[WARP_STATE_OFF] = 0;
    warp_arm(NULL);
    CHECK(call_fn(hub) == 0 && !strcmp(loaded, "level95"), "disarmed: unchanged");

    /* Level watch: a fake level load whose 9-byte prologue moves to the stub, which reports the
     * name first; the load itself still runs with its argument. */
    uint8_t *wfn = x + 1024, *wstub = x + 1280;
    p = wfn;
    static const uint8_t prologue[WARP_WATCH_LEN] = {0x8b, 0x4c, 0x24, 0x04, 0x89, 0xc8, 0x90, 0x90, 0x90};
    memcpy(p, prologue, sizeof prologue);   /* mov ecx,[esp+4]; mov eax,ecx; nop x3 */
    p += sizeof prologue;
    uint32_t resume = (uint32_t)(uintptr_t)p, fl = (uint32_t)(uintptr_t)fake_load;
    *p++ = 0x51;                                       /* push ecx (the name) */
    *p++ = 0xb8; memcpy(p, &fl, 4); p += 4;            /* mov eax,fake_load */
    *p++ = 0xff; *p++ = 0xd0;                          /* call eax */
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04; *p++ = 0xc3; /* add esp,4; ret */
    n = warp_build_watch_stub(wstub, (uint32_t)(uintptr_t)warp_on_level_load, prologue, WARP_WATCH_LEN, resume);
    CHECK(n > 0 && n <= WARP_STUB_MAX, "watch stub built (%u bytes)", (unsigned)n);
    warp_write_jmp(wfn, (uint32_t)(uintptr_t)wfn, (uint32_t)(uintptr_t)wstub, WARP_WATCH_LEN);
    warp_level_cb = watch_cb;
    void (*call_load)(const char *) = (void (*)(const char *))(void *)wfn;
    static char m14[] = "level51", other[] = "frontend";
    int before = loads;
    call_load(m14);
    CHECK(watched == 1 && !strcmp(watched_level, "level51") && watched_mission == 14 && loads == before + 1 && !strcmp(loaded, "level51"),
          "level watch: level51 reported as mission 14, the load still runs with it");
    call_load(other);
    CHECK(watched == 2 && !strcmp(watched_level, "frontend") && watched_mission == -1 && !strcmp(loaded, "frontend"),
          "level watch: an unknown name is reported with mission -1");
    warp_level_cb = NULL;
    call_load(m14);
    CHECK(watched == 2 && loads == before + 3, "level watch without a callback: the load runs, nothing reported");

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
