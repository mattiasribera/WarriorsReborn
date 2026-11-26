/* Lua mods (docs/MODDING.md): plain Lua files from a mods folder, run in the game's own Lua state
 * at fixed points. Off unless a mod is enabled with --mod.
 *
 *   --mods DIR        the mods folder (default: mods/ next to the program)
 *   --mod ID[,ID...]  enable these mods (repeatable; `all` = every mod found)
 *   --no-mods         run no mod (wins over --mod)
 *   --mods-trace      log every script the game loads ([mods] script NAME), to find hook names
 *   --rebuild-mod-assets [ID]  build every enabled mod's assets.ini again (or only mod ID's)
 *
 * Hook (title code): lua_dobuffer (0x233f80), through which the game runs every script it loads
 * (config_preload*, each level's scripts, the chunks a script loads with doFile / preLoadFile).
 * Its first 8 bytes jump to a host stub that runs the original call and then, after it returned,
 * calls mods_on_script() with the state and the script's name: the script's functions and tables
 * are defined at that point, and the hook is a C boundary (outside the VM), like lua_call's.
 * Mod chunks run with lua_dobuffer under a capturing _ERRORMESSAGE (as --lua does): a Lua error
 * logs `[mods] <id>: error ...` and turns that mod off for the rest of the run; the stack and the
 * game's _ERRORMESSAGE are restored either way. Nothing is added to the Lua state but the globals
 * MOD_ID / MOD_SCRIPT / MOD_MESHES / MOD_ASSETS (set before each mod chunk), ModLog(text) (writes
 * `[mods] <id>: text`), ModMeshSet(group, on) (mods_mesh_set), ModTexLayer(layer, on)
 * (mods_tex_layer), ModSave(key, value) / ModLoad(key) / ModOption(name) / ModKeep(key, value) /
 * ModKept(key) (mods_data_save / mods_data_load / mods_option / mods_keep / mods_kept, bound to the
 * mod whose file runs) and
 * ModDifficultyOption / ModDifficultyChosen (hle_diffmenu.h) and the menu and music functions ModMenuOpen /
 * ModMenuState / ModMenuClose / ModMusicPlay / ModMusicPlaying / ModMusicMute (hle_modmenu.h) and the
 * mod's own sounds ModSound / ModSoundMove / ModSoundStop (modsound.h, bound like ModSave) and
 * ModRumbleGangs(gang1, gang2) (mods_rumble_gangs).
 * The mods' Lua files are read once at start-up; the game's state gets no file or OS access. */
#ifndef HLE_MODS_H
#define HLE_MODS_H
#include "hle_devlua.h"
#include "modini.h"
#include <stdint.h>

#define MODS_DOBUFFER_LEN 8   /* mov eax,[esp+0x10]; mov ecx,[esp+0xc] (two whole instructions) */
#define MODS_PUSHCCLOSURE 0x0022ef00u   /* lua_pushcclosure(L, fn, n): jmp 0x23cdc0 */
#define MODS_PUSHNIL 0x0022ee00u        /* lua_pushnil(L) */
#define MODS_PUSHNUMBER 0x0022ee30u     /* lua_pushnumber(L, double) */
#define MODS_PUSHSTRING 0x0022eea0u     /* lua_pushstring(L, s): nil for NULL */

/* Mod data (ModSave / ModLoad, docs/MODDING.md): one JSON file per mod, <hdd>/mods-data/<id>.json,
 * {"<profile>": {"<key>": "<value>", ...}, ...}: string values per save profile (the container
 * name of the profile the game loaded, "none" before one). Keys and profiles: 1-64 of a-z A-Z 0-9
 * . _ -; values up to MODS_VALUE_MAX bytes; a file is at most MODS_DATA_MAX bytes. Every save
 * rewrites the file through <id>.json.tmp renamed over it. The game's save is never touched. */
#define MODS_DATA_MAX (64 * 1024)
#define MODS_VALUE_MAX 1024
#define MODS_KEY_MAX 64
/* hdd_dir: the --hdd folder (NULL: ModSave / ModLoad fail); profile_fn: the current profile's id
 * (NULL or "" = "none"). */
void mods_set_storage(const char *hdd_dir, const char *(*profile_fn)(void));
/* Stores value under key for mod id in the current profile (value NULL: removes the key).
 * 0, or -1 with the reason in err (bad key, value too long, file full, unreadable file kept as
 * it is, write failure). */
int mods_data_save(const char *id, const char *key, const char *value, char *err, size_t cap);
/* The value of key for mod id in the current profile: 1 (copied to out), 0 none, -1 error (err). */
int mods_data_load(const char *id, const char *key, char *out, size_t cap, char *err, size_t errcap);

/* Parses one option at argv[*i]. 1 = handled, 0 = not ours, -1 = invalid (message in err). */
int mods_parse_arg(int argc, char **argv, int *i, char *err, size_t cap);
/* ModKeep(key, value) / ModKept(key): a mod's values kept in memory for the rest of the run, across
 * every Lua state (key and value rules as ModSave; value NULL removes). Never written anywhere. */
#define MODS_KEPT_MAX 256
int mods_keep(const char *id, const char *key, const char *value, char *err, size_t cap);
const char *mods_kept(const char *id, const char *key);
/* ModRumbleGangs(gang1, gang2): the two gangs of the game's Rumble menu (CfgRumbleGang ids, the pak
 * numbers rumble_full_gang_<id>, 1..MODS_RUMBLE_GANG_MAX). The menu (C++ RM_ChooseGangs) keeps its
 * setup as 23 u16 at MODS_RUMBLE_DATA, the values GetRumbleModeData copies into RM_LuaData (0x11cbc0:
 * [1] mode, [2] type, [3] size, [4]/[5] the gangs' ids minus 1, then the characters). A rumble
 * arena's script takes its setup from that Lua table, but the native intro ShowRumbleModeIntro
 * (0x280890 -> 0xd8ee0) picks the DJ's gang line from the menu itself: 0x1244d5 reads the u16 at
 * MODS_RUMBLE_DATA + 6 + side * 2 and plays "vags/character/voices/dj/rumblemode/dj_gang_<u16 + 1>"
 * (0x124480). So a mod that starts a rumble with its own setup sets the gangs here before the intro.
 * Writes the two u16 (ids minus 1) when both code sites still read that block (host = va + bias):
 * 0, or -1 with the reason in err (an id out of range, other bytes at a site: nothing written). */
#define MODS_RUMBLE_DATA 0x004fbd84u
#define MODS_RUMBLE_GANG_MAX 64
int mods_rumble_gangs(intptr_t bias, int gang1, int gang2, char *err, size_t cap);
/* ModOption(name) of mod id: the value of its own mod.ini key `name`, or NULL. */
const char *mods_option(const char *id, const char *name);
/* The disc and hdd folders a mod's assets.ini is built from and cached in (src/modassets.h): set
 * before mods_load (without them a mod with an assets.ini loads without its assets). */
void mods_set_assets(const char *disc, const char *hdd);
/* Clears every option and loaded mod (tests). */
void mods_reset(void);
/* Scans the mods folder (--mods, else default_dir), keeps the enabled mods in load order and reads
 * their Lua files. Logs `[mods] loaded N: id1 id2 ...` (or why none). Returns N. */
int mods_load(const char *default_dir);
/* Enabled mods kept by mods_load. */
int mods_count(void);
const ModInfo *mods_get(int i);
/* 1 when the mod was turned off by an error. */
int mods_failed(const char *id);
/* ModMeshSet(group, on): turns a mod's mesh-override group ("<id>/<subfolder>" of its meshes
 * folder, src/meshover.h) on or off, logging `[mods] mesh group G on|off (N overrides)` when it
 * changes; the number of overrides in it, -1 (logged) when there is no such group. */
int mods_mesh_set(const char *group, int on);
/* ModMeshPoint(group, key, x, y, z): sets (p not NULL) or removes (p NULL) a world point of a mesh
 * group under key (src/meshover.h, meshover_point_set). The group's number of points, -1 (logged)
 * for no such group, -2 (logged) when it is full. Every point is removed when a new Lua state
 * boots (its keys are handles of the old one). */
int mods_mesh_point(const char *group, const char *key, const float *p);

/* ModTexLayer(layer, on): turns a mod's texture layer ("<id>/<label>", files "<tex>@<label>.png" of
 * its meshes folder, src/texlayer.h) on or off, logging `[mods] texture layer L on|off (N textures)`
 * when it changes; the number of textures it covers, -1 (logged) when there is no such layer. */
int mods_tex_layer(const char *layer, int on);

/* After the game loaded the script named `name` in L (the chunk name: "level95.lua"; a leading = or @,
 * a folder and the .lua suffix are dropped, so hooks and MOD_SCRIPT say "level95"): the boot hooks of a state seen for the first time,
 * then every mod hook whose pattern matches. */
void mods_on_script(const DevLuaApi *api, uint32_t L, const char *name);

/* Signature-checks lua_dobuffer, patches the hook. Returns 1 when installed; nothing is written
 * when no mod is loaded. */
int hle_mods_install(void);
#endif
