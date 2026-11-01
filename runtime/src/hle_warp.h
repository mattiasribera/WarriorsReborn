/* --warp MISSION[:CHECKPOINT] | levelNN[:CHECKPOINT]: the first story level the menus load
 * (loading a profile loads the Coney hub through Lua runNextMission -> MenuLoadLevel) goes to
 * the chosen mission and checkpoint instead. Off by default: without the option nothing is
 * patched.
 *
 * Mechanism (title code):
 *   - MenuLoadLevel, the Lua C function at 0x27fd90, reads its name argument (call 0x324f10 at
 *     0x27fd99) and loads the level with `push eax; call 0x7bca0` at 0x27fd9e. Those 6 bytes jump
 *     to a host stub that asks warp_on_load() which name to load: the warp target on the first
 *     call that loads a "level..." name, and on any call that replaces that request while it is
 *     still pending ([[0x5b9f78]+0x148] == 3, set by the level load; loading a profile runs
 *     runNextMission twice in a row); the requested name afterwards (one-shot, so finishing or
 *     failing the mission continues normally). When it redirects, the stub first calls the
 *     title's SetCheckPoint (0x337480, what Lua SetCheckPoint calls; runNextMission has just set
 *     the hub checkpoint), then the level load, then returns to 0x27fda4.
 *   - Save progress: the profile write FUN_0033f510 (this = profile manager, arg = slot; the only
 *     code that creates the save container and writes profile.dat) jumps to a host stub that
 *     logs and reports success without writing. It is blocked for the whole warped session, so
 *     a warped run never changes the save on disk.
 * Every site is signature-checked first; on any mismatch nothing is written (no warp) and the
 * mismatching sites are reported.
 *
 * Mission numbers are the game's own (field 5 of levelNames in config_preload3.lua; field 4 is
 * the chapter count). That table is Lua 4.0 bytecode inside Main.wad0 and its native copy
 * (CfgLevelName) is only built after boot, too late to validate the option, so warp_levels
 * holds the level identifiers and chapter counts only. */
#ifndef HLE_WARP_H
#define HLE_WARP_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *level;   /* "level81" */
    int mission;         /* story mission number, 0 = not a mission (the Coney hub) */
    int chapters;        /* valid checkpoints 1..chapters */
} WarpLevel;

extern const WarpLevel warp_levels[];
extern const int warp_level_count;

typedef struct {
    const WarpLevel *level;
    int checkpoint;      /* 1..level->chapters */
} WarpTarget;

/* Parses "8", "8:3", "level81" or "level81:3" into *t. Returns 0, or -1 with a message in err. */
int warp_parse(const char *s, WarpTarget *t, char *err, size_t cap);
const WarpLevel *warp_find_mission(int mission);
const WarpLevel *warp_find_level(const char *name);

/* Register frame written by pushal (lowest address first), as the stubs pass it. */
typedef struct { uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; } WarpFrame;

#define WARP_STUB_MAX 64
/* Load stub (reached by a jmp from the MenuLoadLevel site, eax = requested name, the name
 * call's 3 arguments on the stack): calls hostfn(frame); then eax = name to load and, when ecx
 * is non-zero, setcp(ecx) is called first; then load(eax) and a jump to ret_va. Returns its
 * length. All addresses are absolute, so the stub is position independent. */
size_t warp_build_load_stub(uint8_t *buf, uint32_t hostfn, uint32_t setcp, uint32_t load, uint32_t ret_va);
/* Save stub (replaces the profile write, thiscall with 1 stack argument): calls hostfn(frame),
 * sets the result byte [ecx+0xd5] = 1 and returns al = 1 (ret 4). */
size_t warp_build_save_stub(uint8_t *buf, uint32_t hostfn);

/* Host side of the stubs. warp_arm() sets the one-shot target (NULL disarms). */
void warp_arm(const WarpTarget *t);
/* --scene ID (e.g. l84_c7): warps to the scene's level (checkpoint 1) and sets the Lua global
 * SCENETEST = ID in the level's Lua state, so the level script plays only that scene
 * (Rockstar's scene test mode, the *_scenetest.lua chunks). The global is set on every
 * GetCheckPoint call, in the caller's Lua state (see warp_on_checkpoint). warp_scene_parse fills
 * t from ID. */
extern const char *warp_scene;
int warp_scene_parse(const char *id, WarpTarget *t, char *err, size_t cap);
/* The title's lua_pushstring / lua_setglobal (set at install; NULL in tests). */
extern void (*warp_lua_pushstring)(uint32_t L, const char *s);
extern void (*warp_lua_setglobal)(uint32_t L, const char *name);
void warp_on_load(WarpFrame *f);
void warp_on_checkpoint(WarpFrame *f);
size_t warp_build_checkpoint_stub(uint8_t *buf, uint32_t hostfn);
void warp_on_save(WarpFrame *f);
int warp_redirects(void);     /* level loads redirected so far */
int warp_saves_blocked(void);

typedef struct {
    const char *name;
    uint32_t va;
    uint8_t len;
    uint8_t orig[32];
} WarpSig;

extern const WarpSig warp_sigs[];
extern const int warp_sig_count;
#define WARP_LOAD_SITE 0x0027fd9eu   /* push eax; call 0x7bca0 (6 bytes) */
#define WARP_LOAD_RET 0x0027fda4u
#define WARP_SETCP 0x00337480u
#define WARP_LOAD 0x0007bca0u
#define WARP_SAVE_SITE 0x0033f510u   /* sub esp,0x238 (6 bytes) */
#define WARP_LUA_PUSHSTRING 0x0022eea0u
#define WARP_GETCP 0x0028a3b0u          /* GetCheckPoint: sub esp,8; call 0x335f40 (8 bytes) */
#define WARP_GETCP_CALLEE 0x00335f40u
#define WARP_LUA_SETGLOBAL 0x0022f210u
#define WARP_GAME 0x005b9f78u        /* pointer to the game object */
#define WARP_STATE_OFF 0x148u         /* level state; the level load sets 3 = load pending */
#define WARP_STATE_PENDING 3u
/* Where warp_on_load reads the game object pointer (install: WARP_GAME; tests: a fake). */
extern const volatile uint32_t *warp_game_ptr;

/* Checks t[0..n) against memory where guest va is at host va + bias. Returns the number of
 * mismatching entries and lists them ("name (va)", comma separated) in bad. */
int warp_check(const WarpSig *t, int n, intptr_t bias, char *bad, size_t cap);
/* Writes `jmp target` + nop padding (len >= 5 bytes) at host address site. */
void warp_write_jmp(uint8_t *site, uint32_t site_va, uint32_t target, int len);

/* Parses "--warp VALUE": returns 1 when argv[*i] is --warp (consuming the value; *t filled),
 * -1 on an invalid or missing value (message in err), 0 for other options. */
int warp_parse_arg(int argc, char **argv, int *i, WarpTarget *t, int *enabled, char *err, size_t cap);

/* Signature-checks and patches the loaded title, prints the [warp] lines. Returns 1 when the
 * warp is armed. Nothing is written (and nothing printed) when enabled is 0. */
int hle_warp_install(int enabled, const WarpTarget *t);
#endif
