/* --warp: start a chosen mission when a profile is loaded (see hle_warp.h). */
#include "hle_warp.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Story missions by the game's number (levelNames field 5) with their chapter counts (field 4),
 * plus the Coney hub, which has no mission number and is reachable by name only. */
const WarpLevel warp_levels[] = {
    {"level99", 1, 3},  {"level80", 2, 4},  {"level87", 3, 5},  {"level34", 4, 5},
    {"level2", 5, 4},   {"level3", 6, 5},   {"level5", 7, 4},   {"level81", 8, 5},
    {"level86", 9, 4},  {"level93", 10, 6}, {"level31", 11, 6}, {"level14", 12, 3},
    {"level9", 13, 3},  {"level51", 14, 7}, {"level52", 15, 4}, {"level54", 16, 5},
    {"level55", 17, 3}, {"level84", 18, 3}, {"level82", 19, 4}, {"level92", 20, 3},
    {"level83", 21, 6}, {"level20", 22, 6}, {"level11", 23, 6}, {"level95", 0, 12},
};
const int warp_level_count = (int)(sizeof warp_levels / sizeof warp_levels[0]);

const WarpLevel *warp_find_mission(int mission) {
    for (int i = 0; i < warp_level_count; i++)
        if (mission > 0 && warp_levels[i].mission == mission) return &warp_levels[i];
    return NULL;
}

const WarpLevel *warp_find_level(const char *name) {
    for (int i = 0; i < warp_level_count; i++)
        if (!strcmp(warp_levels[i].level, name)) return &warp_levels[i];
    return NULL;
}

/* Decimal digits only, 1..4 of them: 1 with the value, else 0. */
static int parse_num(const char *s, size_t n, int *v) {
    if (n < 1 || n > 4) return 0;
    int x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (s[i] - '0');
    }
    *v = x;
    return 1;
}

int warp_parse(const char *s, WarpTarget *t, char *err, size_t cap) {
    const char *colon = strchr(s, ':');
    size_t head = colon ? (size_t)(colon - s) : strlen(s);
    char name[16];
    int mission = 0, cp = 1;
    const WarpLevel *l = NULL;
    if (head < sizeof name) { memcpy(name, s, head); name[head] = 0; } else name[0] = 0;
    if (!strncmp(name, "level", 5)) {
        l = warp_find_level(name);
        if (!l) {
            snprintf(err, cap, "--warp %s: unknown level '%s' (story levels: the mission list in the README, or level95)", s, name);
            return -1;
        }
    } else if (parse_num(s, head, &mission)) {
        l = warp_find_mission(mission);
        if (!l) {
            snprintf(err, cap, "--warp %s: no story mission %d (missions are 1-%d)", s, mission, 23);
            return -1;
        }
    } else {
        snprintf(err, cap, "--warp %s: expected MISSION[:CHECKPOINT] (e.g. 8 or 8:3) or a level name (e.g. level81:3)", s);
        return -1;
    }
    if (colon && !parse_num(colon + 1, strlen(colon + 1), &cp)) {
        snprintf(err, cap, "--warp %s: the checkpoint must be a number (1-%d for %s)", s, l->chapters, l->level);
        return -1;
    }
    if (cp < 1 || cp > l->chapters) {
        snprintf(err, cap, "--warp %s: %s has checkpoints 1-%d", s, l->level, l->chapters);
        return -1;
    }
    t->level = l;
    t->checkpoint = cp;
    return 0;
}

const char *warp_scene;

int warp_scene_parse(const char *id, WarpTarget *t, char *err, size_t cap) {
    char level[32];
    size_t n = 0;
    if (id[0] == 'l')
        while (id[1 + n] >= '0' && id[1 + n] <= '9' && n < 3) n++;
    if (!n || id[1 + n] != '_' || strlen(id) > 48) {
        snprintf(err, cap, "--scene: \"%s\" is not a scene id such as l84_c7 (l<level>_<name>)", id);
        return -1;
    }
    snprintf(level, sizeof level, "level%.*s", (int)n, id + 1);
    if (warp_parse(level, t, err, cap)) return -1;
    t->checkpoint = 1;
    return 0;
}

int warp_parse_arg(int argc, char **argv, int *i, WarpTarget *t, int *enabled, char *err, size_t cap) {
    if (!strcmp(argv[*i], "--scene")) {
        if (*i + 1 >= argc) {
            snprintf(err, cap, "--scene: missing value (a scene id such as l84_c7)");
            return -1;
        }
        if (warp_scene_parse(argv[++*i], t, err, cap)) return -1;
        warp_scene = argv[*i];
        *enabled = 1;
        return 1;
    }
    if (strcmp(argv[*i], "--warp")) return 0;
    if (*i + 1 >= argc) {
        snprintf(err, cap, "--warp: missing value (MISSION[:CHECKPOINT] or levelNN[:CHECKPOINT])");
        return -1;
    }
    if (warp_parse(argv[++*i], t, err, cap)) return -1;
    *enabled = 1;
    return 1;
}

/* ---- stubs ---- */

static uint8_t *put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }

/* pushal; cld; mov ebp,esp; and esp,-16; sub esp,12; push ebp; mov eax,hostfn; call eax;
 * mov esp,ebp; popal (eax/ecx as the host function left them in the frame). */
static uint8_t *frame_call(uint8_t *p, uint32_t hostfn) {
    static const uint8_t head[] = {0x60, 0xfc, 0x89, 0xe5, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x0c, 0x55, 0xb8};
    memcpy(p, head, sizeof head);
    p = put32(p + sizeof head, hostfn);
    *p++ = 0xff; *p++ = 0xd0;   /* call eax */
    *p++ = 0x89; *p++ = 0xec;   /* mov esp,ebp */
    *p++ = 0x61;                /* popal */
    return p;
}

size_t warp_build_load_stub(uint8_t *buf, uint32_t hostfn, uint32_t setcp, uint32_t load, uint32_t ret_va) {
    uint8_t *p = frame_call(buf, hostfn);
    *p++ = 0x85; *p++ = 0xc9;              /* test ecx,ecx */
    *p++ = 0x74; *p++ = 13;                /* je over the checkpoint call (13 bytes) */
    *p++ = 0x50;                           /* push eax (name) */
    *p++ = 0x51;                           /* push ecx (checkpoint) */
    *p++ = 0xb8; p = put32(p, setcp);      /* mov eax,setcp */
    *p++ = 0xff; *p++ = 0xd0;              /* call eax */
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04; /* add esp,4 */
    *p++ = 0x58;                           /* pop eax */
    *p++ = 0x50;                           /* push eax: the level load's argument */
    *p++ = 0xb9; p = put32(p, load);       /* mov ecx,load */
    *p++ = 0xff; *p++ = 0xd1;              /* call ecx */
    *p++ = 0x68; p = put32(p, ret_va);     /* push ret_va */
    *p++ = 0xc3;                           /* ret */
    return (size_t)(p - buf);
}

size_t warp_build_save_stub(uint8_t *buf, uint32_t hostfn) {
    uint8_t *p = frame_call(buf, hostfn);
    static const uint8_t tail[] = {0xc6, 0x81, 0xd5, 0x00, 0x00, 0x00, 0x01,  /* mov byte [ecx+0xd5],1 */
                                   0xb0, 0x01,                                /* mov al,1 */
                                   0xc2, 0x04, 0x00};                         /* ret 4 */
    memcpy(p, tail, sizeof tail);
    return (size_t)(p + sizeof tail - buf);
}

void warp_write_jmp(uint8_t *site, uint32_t site_va, uint32_t target, int len) {
    site[0] = 0xe9;
    put32(site + 1, target - (site_va + 5u));
    for (int i = 5; i < len; i++) site[i] = 0x90;
}

/* ---- host side ---- */

static WarpTarget armed;
static int is_armed, redirects, window, saves_blocked, loads_logged;
const volatile uint32_t *warp_game_ptr;
void (*warp_lua_pushstring)(uint32_t L, const char *s);
void (*warp_lua_setglobal)(uint32_t L, const char *name);

/* The redirected load is still pending: [game + 0x148] is the 3 the level load stored. */
static int load_pending(void) {
    uint32_t g = warp_game_ptr ? *warp_game_ptr : 0;
    return g && *(const volatile uint32_t *)(uintptr_t)(g + WARP_STATE_OFF) == WARP_STATE_PENDING;
}

void warp_arm(const WarpTarget *t) {
    is_armed = t != NULL;
    if (t) armed = *t;
}

int warp_redirects(void) { return redirects; }
int warp_saves_blocked(void) { return saves_blocked; }

void (*warp_level_hook)(const char *name);
int warp_level_pending(void) { return load_pending(); }

static void warp_decide(WarpFrame *f);
void warp_on_load(WarpFrame *f) {
    warp_decide(f);
    if (warp_level_hook) warp_level_hook((const char *)(uintptr_t)f->eax);   /* the name the title loads */
}

static void warp_decide(WarpFrame *f) {
    const char *req = (const char *)(uintptr_t)f->eax;
    f->ecx = 0;
    if (!is_armed) return;
    /* The first level request, and any that replaces it before the load starts (loading a
     * profile requests the hub twice in a row). */
    if (redirects && window && !load_pending()) window = 0;   /* the redirected load has started */
    if (req && !strncmp(req, "level", 5) && (!redirects || window)) {
        printf("[warp] MenuLoadLevel(\"%.31s\") -> %s, checkpoint %d (mission %d)%s\n", req, armed.level->level,
               armed.checkpoint, armed.level->mission, redirects ? ", again: the pending load was replaced" : "");
        redirects++;
        window = 1;
        fflush(stdout);
        f->eax = (uint32_t)(uintptr_t)armed.level->level;
        f->ecx = (uint32_t)armed.checkpoint;
        return;
    }
    if (loads_logged++ < 16) {
        printf("[warp] MenuLoadLevel(\"%.31s\") (not redirected)\n", req ? req : "(null)");
        fflush(stdout);
    }
}

/* GetCheckPoint (Lua C function) entry: level scripts call it right before their
 * `if not SCENETEST then Main() RunMission() else <level>_scenetest end` test, in the level's
 * own Lua state, so the scene global is set there. [esp at entry + 4] = the Lua state. */
void warp_on_checkpoint(WarpFrame *f) {
    static uint32_t last;
    if (!warp_scene || !warp_lua_setglobal) return;
    uint32_t L = *(const uint32_t *)(uintptr_t)(f->esp + 4);
    warp_lua_pushstring(L, warp_scene);
    warp_lua_setglobal(L, "SCENETEST");
    if (L != last) {
        last = L;
        printf("[warp] SCENETEST = \"%s\" (Lua state %08x)\n", warp_scene, L);
        fflush(stdout);
    }
}

size_t warp_build_checkpoint_stub(uint8_t *buf, uint32_t hostfn) {
    uint8_t *p = frame_call(buf, hostfn);
    *p++ = 0x83; *p++ = 0xec; *p++ = 0x08;                 /* sub esp,8 (the replaced prologue) */
    *p++ = 0xb8; p = put32(p, WARP_GETCP_CALLEE);          /* mov eax,0x335f40 */
    *p++ = 0xff; *p++ = 0xd0;                              /* call eax */
    *p++ = 0x68; p = put32(p, WARP_GETCP + 8);             /* push 0x28a3b8 */
    *p++ = 0xc3;                                           /* ret */
    return (size_t)(p - buf);
}

void warp_on_save(WarpFrame *f) {
    saves_blocked++;
    if (saves_blocked <= 16) {
        /* The slot argument is above the stub's return address: [esp at entry + 4]. pushal stored
         * the esp from before pushal, which is the entry esp. */
        uint32_t slot = *(const uint32_t *)(uintptr_t)(f->esp + 4);
        printf("[warp] save blocked: profile slot %u not written (warped session; the save on disk is unchanged)\n", slot);
        fflush(stdout);
    }
}

/* ---- level watch ---- */

void (*warp_level_cb)(const char *level, int mission);

void warp_on_level_load(WarpFrame *f) {
    /* The level name argument: [entry esp + 4] (pushal stored the entry esp). */
    const char *name = *(const char *const *)(uintptr_t)(f->esp + 4);
    if (!warp_level_cb || !name) return;
    char lv[32];
    snprintf(lv, sizeof lv, "%s", name);
    const WarpLevel *l = warp_find_level(lv);
    warp_level_cb(lv, l ? l->mission : -1);
}

size_t warp_build_watch_stub(uint8_t *buf, uint32_t hostfn, const uint8_t *orig, int orig_len, uint32_t resume_va) {
    uint8_t *p = frame_call(buf, hostfn);
    memcpy(p, orig, (size_t)orig_len);
    p += orig_len;
    *p++ = 0x68; p = put32(p, resume_va);   /* push resume_va */
    *p++ = 0xc3;                            /* ret */
    return (size_t)(p - buf);
}

int hle_warp_watch_install(void) {
    const WarpSig *load = NULL;
    for (int i = 0; i < warp_sig_count; i++)
        if (warp_sigs[i].va == WARP_LOAD) load = &warp_sigs[i];
    if (!load || warp_check(load, 1, 0, NULL, 0)) {
        printf("[warp] level watch: off (the level load at %08x does not match)\n", WARP_LOAD);
        return 0;
    }
    uint8_t *stub = plat_alloc_exec(WARP_STUB_MAX);
    if (!stub) return 0;
    warp_build_watch_stub(stub, (uint32_t)(uintptr_t)warp_on_level_load, load->orig, WARP_WATCH_LEN, WARP_LOAD + WARP_WATCH_LEN);
    warp_write_jmp((uint8_t *)(uintptr_t)WARP_LOAD, WARP_LOAD, (uint32_t)(uintptr_t)stub, WARP_WATCH_LEN);
    return 1;
}

/* ---- title ---- */

const WarpSig warp_sigs[] = {
    /* MenuLoadLevel: mov eax,[esp+4]; push 0; push 1; push eax; call 0x324f10; push eax;
     * call 0x7bca0; add esp,0x10; xor eax,eax; ret */
    {"MenuLoadLevel", 0x0027fd90u, 26,
     {0x8b, 0x44, 0x24, 0x04, 0x6a, 0x00, 0x6a, 0x01, 0x50, 0xe8, 0x72, 0x51, 0x0a, 0x00, 0x50, 0xe8, 0xfc, 0xbe, 0xdf,
      0xff, 0x83, 0xc4, 0x10, 0x33, 0xc0, 0xc3}},
    /* level load: mov ecx,[esp+4]; mov eax,[0x5b9f78]; push ecx; mov ecx,0x4b0ec8;
     * mov dword [eax+0x148],3 (the load-pending state warp_on_load reads) */
    {"level load", WARP_LOAD, 25,
     {0x8b, 0x4c, 0x24, 0x04, 0xa1, 0x78, 0x9f, 0x5b, 0x00, 0x51, 0xb9, 0xc8, 0x0e, 0x4b, 0x00, 0xc7, 0x80, 0x48, 0x01,
      0x00, 0x00, 0x03, 0x00, 0x00, 0x00}},
    /* SetCheckPoint(cp): mov eax,[0x5b9f78]; push esi; mov esi,[esp+8]; lea ecx,[eax+0x458];
     * mov [eax+0x332],si */
    {"SetCheckPoint", WARP_SETCP, 23,
     {0xa1, 0x78, 0x9f, 0x5b, 0x00, 0x56, 0x8b, 0x74, 0x24, 0x08, 0x8d, 0x88, 0x58, 0x04, 0x00, 0x00, 0x66, 0x89, 0xb0,
      0x32, 0x03, 0x00, 0x00}},
    /* profile write FUN_0033f510: sub esp,0x238; mov eax,[0x463080] (security cookie) */
    {"profile write", WARP_SAVE_SITE, 11, {0x81, 0xec, 0x38, 0x02, 0x00, 0x00, 0xa1, 0x80, 0x30, 0x46, 0x00}},
    /* lua_pushstring(L, s): mov edx,[esp+8]; test edx,edx; push esi; jne ...; mov esi,[esp+..] */
    /* GetCheckPoint: sub esp,8; call 0x335f40; mov ecx,[esp+0xc] */
    {"GetCheckPoint", WARP_GETCP, 12, {0x83, 0xec, 0x08, 0xe8, 0x88, 0xbb, 0x0a, 0x00, 0x8b, 0x4c, 0x24, 0x0c}},
    {"lua_pushstring", WARP_LUA_PUSHSTRING, 12, {0x8b, 0x54, 0x24, 0x08, 0x85, 0xd2, 0x56, 0x75, 0x0e, 0x8b, 0x74, 0x24}},
    /* lua_setglobal(L, name), sets name = the value on top: mov eax,[esp+8]; push esi;
     * mov esi,[esp+8]; push edi; mov edi,[esi] */
    {"lua_setglobal", WARP_LUA_SETGLOBAL, 12, {0x8b, 0x44, 0x24, 0x08, 0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0x3e}},
};
const int warp_sig_count = (int)(sizeof warp_sigs / sizeof warp_sigs[0]);

int warp_check(const WarpSig *t, int n, intptr_t bias, char *bad, size_t cap) {
    int nbad = 0;
    size_t len = 0;
    if (cap) bad[0] = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].len && t[i].len <= sizeof t[i].orig &&
            !memcmp((const void *)(uintptr_t)(t[i].va + bias), t[i].orig, t[i].len)) continue;
        if (cap && len + 1 < cap) {
            int w = snprintf(bad + len, cap - len, "%s%s (%08x)", nbad ? ", " : "", t[i].name, t[i].va);
            if (w > 0) len += (size_t)w < cap - len ? (size_t)w : cap - len - 1;
        }
        nbad++;
    }
    return nbad;
}

int hle_warp_track_install(void) {
    char bad[256];
    if (warp_check(warp_sigs, 2, 0, bad, sizeof bad)) {   /* MenuLoadLevel and the level load */
        printf("[level] level loads not tracked: signature mismatch at %s (the perf lines say level ?)\n", bad);
        return 0;
    }
    uint8_t *load = plat_alloc_exec(WARP_STUB_MAX);
    if (!load) return 0;
    warp_build_load_stub(load, (uint32_t)(uintptr_t)warp_on_load, WARP_SETCP, WARP_LOAD, WARP_LOAD_RET);
    warp_game_ptr = (const volatile uint32_t *)(uintptr_t)WARP_GAME;
    warp_write_jmp((uint8_t *)(uintptr_t)WARP_LOAD_SITE, WARP_LOAD_SITE, (uint32_t)(uintptr_t)load, 6);
    printf("[level] level loads tracked at MenuLoadLevel (%08x), not redirected\n", WARP_LOAD_SITE);
    return 1;
}

int hle_warp_install(int enabled, const WarpTarget *t) {
    if (!enabled || !t || !t->level) return 0;
    char bad[256];
    if (warp_check(warp_sigs, warp_sig_count, 0, bad, sizeof bad)) {
        printf("[warp] applied: none; skipped: %s (signature mismatch: nothing written, no warp)\n", bad);
        return 0;
    }
    uint8_t *stubs = plat_alloc_exec(3 * WARP_STUB_MAX);
    if (!stubs) {
        printf("[warp] applied: none (no executable memory for the stubs)\n");
        return 0;
    }
    uint8_t *load = stubs, *save = stubs + WARP_STUB_MAX;
    warp_build_load_stub(load, (uint32_t)(uintptr_t)warp_on_load, WARP_SETCP, WARP_LOAD, WARP_LOAD_RET);
    warp_build_save_stub(save, (uint32_t)(uintptr_t)warp_on_save);
    warp_game_ptr = (const volatile uint32_t *)(uintptr_t)WARP_GAME;
    warp_lua_pushstring = (void (*)(uint32_t, const char *))(uintptr_t)WARP_LUA_PUSHSTRING;
    warp_lua_setglobal = (void (*)(uint32_t, const char *))(uintptr_t)WARP_LUA_SETGLOBAL;
    warp_arm(t);
    warp_write_jmp((uint8_t *)(uintptr_t)WARP_LOAD_SITE, WARP_LOAD_SITE, (uint32_t)(uintptr_t)load, 6);
    warp_write_jmp((uint8_t *)(uintptr_t)WARP_SAVE_SITE, WARP_SAVE_SITE, (uint32_t)(uintptr_t)save, 6);
    if (warp_scene) {
        uint8_t *cp = stubs + 2 * WARP_STUB_MAX;
        warp_build_checkpoint_stub(cp, (uint32_t)(uintptr_t)warp_on_checkpoint);
        warp_write_jmp((uint8_t *)(uintptr_t)WARP_GETCP, WARP_GETCP, (uint32_t)(uintptr_t)cp, 8);
    }
    printf("[warp] applied: MenuLoadLevel (%08x), profile write (%08x); skipped: none\n", WARP_LOAD_SITE, WARP_SAVE_SITE);
    printf("[warp] armed: mission %d = %s, checkpoint %d; the first level the menus load (load a profile) goes there\n",
           t->level->mission, t->level->level, t->checkpoint);
    printf("[warp] WARNING: saving is disabled for this session (profile.dat is not written), so the warp cannot change "
           "the save progress\n");
    return 1;
}
