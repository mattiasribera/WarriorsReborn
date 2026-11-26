/* Mod folders and their mod.ini (docs/MODDING.md). Plain C with no runtime dependencies: the
 * launcher builds this file too, so the runtime and the launcher read mods the same way.
 *
 * A mods folder holds one folder per mod; a folder without a mod.ini is ignored. mod.ini is
 * `key = value` lines (`;` or `#` starts a comment line, `[section]` lines are ignored):
 *
 *   id = extra-hats              required: a-z 0-9 - _ (1-40 characters), unique
 *   name = More hats fly         required
 *   name.es = ...                optional Spanish name (also description.es)
 *   description = ...
 *   version = 1.0                required
 *   author = ...
 *   saves = no                   yes = the mod changes what the game writes to the save
 *   boot = boot.lua              run once per Lua state, after the first script the game loads in it
 *   after.config_preload2 = hats.lua   run after the game loads that script (a trailing * matches a
 *                                      prefix: after.level* = every script whose name starts with level)
 *   meshes = local               a folder inside the mod's folder with mesh overrides (src/meshover.h),
 *                                loaded while the mod is enabled
 *   collision = cage.col         a collision patch file inside the mod's folder (src/colpatch.h): triangles
 *                                added to a level's collision while the mod is enabled
 *   min_runtime = 1.1            optional: the oldest mod runtime (MOD_RUNTIME_VERSION) the mod needs;
 *                                an older runtime does not load it (src/modcheck.h)
 *   files = notes.txt, data*     optional: more names the mod's folder may hold besides mod.ini, its Lua
 *                                files and its meshes folder (a trailing * matches a prefix); anything
 *                                else in the folder is reported as a leftover (src/modcheck.h)
 *   apply_to_all = no            any other key whose name is 1-40 of a-z 0-9 _ is one of the mod's own
 *                                options (at most MOD_OPTION_MAX; values up to 63 bytes), read by the
 *                                mod with ModOption(name)
 *
 * Lua files are plain names inside the mod's folder (no path separators, no "..").
 * Load order: folders sorted by name (byte order), then each mod's hooks in file order. */
#ifndef MODINI_H
#define MODINI_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The mod layer's own version, for min_runtime: raised whenever mods gain something an older
 * runtime lacks (1.0: hooks, ModLog, ModMeshSet; 1.1: min_runtime, files and the safety checks; 1.2:
 * ModSave/ModLoad, ModBlood and collision patches; 1.3: ModSpeed; 1.4: ModOption, ModKeep / ModKept,
 * ModDifficultyOption, ModDifficultyChosen; 1.5: ModSprint; 1.6: ModMenuOpen / ModMenuState / ModMenuClose,
 * ModMusicPlay / ModMusicPlaying / ModMusicMute; 1.7: own assets in assets.ini (.glb, PNG/DDS, WAV/OGG,
 * [sound], [collision]) and ModSound / ModSoundMove / ModSoundStop; 1.8: ModRumbleGangs; 1.9: ModRadarColor;
 * 1.10: ModComboSlowMo; 1.11: ModMenuValues, ModMenuKey). */
#define MOD_RUNTIME_VERSION "1.11"

#define MOD_MAX 64
#define MOD_HOOK_MAX 16
#define MOD_OPTION_MAX 8

typedef struct {
    char script[64];   /* script name, or a prefix ending in '*'; "" = boot */
    char file[64];
} ModHook;

typedef struct {
    char name[41];
    char value[64];
} ModOption;

typedef struct {
    char folder[128];   /* folder name inside the mods dir */
    char id[41];
    char name[96], name_es[96];
    char description[320], description_es[320];
    char version[24];
    char author[64];
    int saves;          /* 1 = changes save data */
    char meshes[64];    /* mesh overrides folder inside the mod's folder, or "" */
    char collision[64]; /* collision patch file inside the mod's folder, or "" */
    char min_runtime[24];   /* digits and dots, or "" */
    char files[256];    /* the files key: comma-separated names / prefixes*, or "" */
    ModHook hooks[MOD_HOOK_MAX];
    int nhooks;
    ModOption options[MOD_OPTION_MAX];   /* the mod's own keys (ModOption) */
    int noptions;
} ModInfo;

/* The value of the mod's own option `name`, or NULL. */
const char *modini_option(const ModInfo *m, const char *name);

/* Parses mod.ini text. 0 = ok; -1 = invalid (first problem in err). */
int modini_parse(const char *text, size_t len, ModInfo *m, char *err, size_t cap);

/* Whether a script name matches a hook pattern (exact, or prefix with a trailing '*'). */
int modini_match(const char *pattern, const char *script);

/* Valid mod id: 1-40 of a-z 0-9 - _. */
int modini_valid_id(const char *id);

/* Lists dir's mods sorted by folder name into out (at most max). Folders whose mod.ini is
 * invalid or whose id repeats an earlier one are skipped; each problem is reported through
 * warn (may be NULL). Returns the number of mods, or -1 when dir cannot be opened. */
int modini_scan(const char *dir, ModInfo *out, int max, void (*warn)(const char *msg, void *ud), void *ud);

#ifdef __cplusplus
}
#endif
#endif
