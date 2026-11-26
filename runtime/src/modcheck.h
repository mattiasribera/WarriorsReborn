/* Mod safety checks (docs/MODDING.md "Safety"): min_runtime and the folder manifest. Plain C with
 * no runtime dependencies, built by the runtime (at load, hle_mods.c) and the launcher (the Mods
 * page's warnings), like modini.c.
 *
 * A mod's folder may hold: mod.ini, the Lua files its hooks name, its meshes folder, assets.ini and
 * the folder own/ (src/modassets.h) and the names
 * of its `files` key (exact, or a prefix with a trailing *). Hidden names (a leading dot) and the
 * desktop's Thumbs.db / desktop.ini are ignored. Anything else is a leftover: usually a file or
 * folder an older version of the mod had, left behind when a new version was copied over it. A
 * leftover is reported and the mod still loads; a missing Lua file or a too-new min_runtime keeps
 * the mod from loading. A missing meshes folder is the mod's own message (it is built by a tool). */
#ifndef MODCHECK_H
#define MODCHECK_H
#include "modini.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MODCHECK_LEFTOVER = 1, MODCHECK_MISSING_LUA = 2, MODCHECK_MISSING_FILE = 3 };

/* Compares dotted versions number by number ("1.10" > "1.9"; missing parts are 0): -1, 0, 1. */
int modcheck_version_cmp(const char *a, const char *b);
/* 1 when this runtime (MOD_RUNTIME_VERSION) is at least the mod's min_runtime (or it has none). */
int modcheck_runtime_ok(const ModInfo *m);
/* Lists the folder dir/<m->folder> against its manifest: fn(kind, name, ud) for each leftover
 * (in name order), each missing Lua file and each missing exact `files` name. Returns the number
 * of problems, or -1 when the folder cannot be opened. */
int modcheck_files(const char *dir, const ModInfo *m, void (*fn)(int kind, const char *name, void *ud), void *ud);
/* The load-time check: one message per problem through warn ("<id>: ...", may be NULL). Returns
 * 1 = load the mod, 0 = do not (too-new min_runtime, a missing Lua file). */
int modcheck_report(const char *dir, const ModInfo *m, void (*warn)(const char *msg, void *ud), void *ud);

#ifdef __cplusplus
}
#endif
#endif
