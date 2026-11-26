/* The Mods page's model: the mods found in the profile's mods folder (read with the runtime's own
 * mod.ini parser, runtime/src/modini.c) and the profile's enabled list (the `mods` key, comma
 * separated ids in the mods folder's order; --mod). */
#ifndef LAUNCHER_MODS_H
#define LAUNCHER_MODS_H
#include "../../runtime/src/modcheck.h"
#include "../../runtime/src/modini.h"
#include "profile.h"
#include <string>
#include <vector>

namespace launcher {

/* A mod's safety check (runtime/src/modcheck.h), shown as a warning under it on the Mods page. */
struct ModCheck {
    bool runtime_ok = true;                  /* min_runtime is not newer than MOD_RUNTIME_VERSION */
    std::vector<std::string> leftovers;      /* names in its folder its version does not use */
    std::vector<std::string> missing;        /* files its mod.ini names that are not there */
};

struct ModList {
    bool folder_ok = false;              /* the folder exists */
    std::vector<ModInfo> mods;           /* in load order */
    std::vector<ModCheck> checks;        /* one per mod, same order */
    std::vector<std::string> problems;   /* skipped folders, one line each */
};

ModList mods_scan(const std::string &dir);
std::vector<std::string> mods_enabled(const Profile &p);
bool mod_enabled(const Profile &p, const std::string &id);
/* Turns id on or off; the list is kept in the order of `found` (ids not found keep their place at
 * the end, so a mod missing for a while stays enabled). */
void mod_set_enabled(Profile &p, const std::string &id, bool on, const std::vector<ModInfo> &found);

/* The save backups the runtime makes before a game with save-changing mods
 * (runtime/src/modsafe.h): <hdd>/mods-backups, in the layout of backup.h (list_backups,
 * restore_backup read them). */
std::string mods_backup_root(const std::string &hdd_dir);

/* The mods a crash happened with: the ids on the crash dump's "[crash-context] mods: id ver, ..."
 * line (runtime/src/diaglog.h) after a "[crash-context] reason:" line in a log's last lines;
 * empty when the log does not end in a crash with mods on. */
std::vector<std::string> crash_mods(const std::vector<std::string> &tail);
/* Home's "The game closed with mods on" notice: due while the game is not running, the last log
 * crashed with mods (mods non-empty) and that log was not dismissed (crash_seen, the [launcher]
 * key set by Dismiss and by Play once without mods). */
bool crash_notice_due(bool running, const std::string &last_log, const std::string &crash_seen,
                      const std::vector<std::string> &mods);

/* ---- "Preparing mods": the enabled mods' assets.ini built before the game starts ----
 * The launcher runs the runtime's own engine (runtime/src/modassets.h) on a worker thread, so the
 * game starts on a warm cache; the game would build the same files at load otherwise. */
struct ModAssetsJob {
    std::string id, name, name_es, folder, version;   /* folder: the mod's folder path */
};
struct ModAssetsResult {
    std::string id;
    bool ok = false;      /* built (or already current) */
    bool built = false;   /* built now */
    std::string error;    /* "<file>:<line>: reason" when it failed */
    double ms = 0;
};
/* The enabled mods (found in mods_dir, in load order) with an assets.ini whose cache for this disc
 * is not complete: the ones a build is due for. Empty when every cache is current. */
std::vector<ModAssetsJob> mods_assets_due(const ModList &found, const std::vector<std::string> &enabled, const std::string &mods_dir,
                                          const std::string &disc, const std::string &hdd);
/* Builds one mod's assets (the runtime's modassets_prepare: cache hit, or a build into
 * <hdd>/mods-cache/<id>/). */
ModAssetsResult mods_assets_build(const ModAssetsJob &job, const std::string &disc, const std::string &hdd);

} // namespace launcher
#endif
