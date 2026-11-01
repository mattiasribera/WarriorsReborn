/* Save backups: before each PLAY the launcher copies the game's save folder (the title's UDATA
 * folder on the emulated hard disk, <hdd>/partition1/UDATA/5454009b) into a per-user backup
 * folder, one timestamped copy per backup, and keeps the newest N. A save that did not change
 * since the newest backup (same file names, sizes and modification times) is not copied again.
 * Restore puts a backup back after backing up the current save, so it can be undone.
 *
 * Only the title's save folder is read, and only it is replaced (by a confirmed restore); the
 * backup folder is the only place anything is deleted (rotation, unfinished copies). Folders are
 * walked without following symbolic links. */
#ifndef LAUNCHER_BACKUP_H
#define LAUNCHER_BACKUP_H
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

#define SAVE_TITLE_ID "5454009b" /* The Warriors' title ID (its folder under UDATA) */
constexpr int BACKUP_COUNT_DEFAULT = 5, BACKUP_COUNT_MIN = 1, BACKUP_COUNT_MAX = 20;

/* <hdd>/partition1/UDATA/5454009b (hdd as the game sees it, already resolved). */
std::string save_folder(const std::string &hdd_dir);
/* The backups of one save folder: <data_dir>/backups/save-<hash of save_dir>. */
std::string backup_root(const std::string &data_dir, const std::string &save_dir);

/* A save folder's file list: count, bytes, and a signature of the relative paths, sizes and
 * modification times (64-bit FNV-1a; 0 when the folder is missing or empty). */
struct SaveScan {
    bool exists = false;
    int files = 0;
    uint64_t bytes = 0;
    uint64_t signature = 0;
};
SaveScan scan_save(const std::string &save_dir);

/* One backup: its folder name (the time it was made, YYYYmmdd-HHMMSS[-N]) and its backup.txt. */
struct BackupInfo {
    std::string name, path;
    std::string kind;          /* "play" (before PLAY), "restore" (the save before a restore) */
    int files = 0;
    uint64_t bytes = 0;
    uint64_t signature = 0;
};
/* The finished backups under root, newest first (unfinished copies are left out). */
std::vector<BackupInfo> list_backups(const std::string &root);
/* "2026-10-31 21:05:12" from a backup name; the name itself when it is not a timestamp. */
std::string backup_date(const std::string &name);

enum class BackupResult { Done, Unchanged, NoSave, Failed };
/* Copies save_dir into root/<stamp> (stamp: timestamp(); -2, -3 ... when taken), unless the save
 * is missing or empty (NoSave) or its signature equals the newest backup's (Unchanged, unless
 * force). Then deletes the oldest backups beyond keep (keep <= 0: none). made = the new backup's
 * folder. */
BackupResult backup_save(const std::string &save_dir, const std::string &root, const std::string &stamp, int keep,
                         const char *kind, bool force, std::string *made, std::string *err);

/* Replaces save_dir with backup b: first backs up the current save (kind "restore", never
 * rotated here; *undo = its folder, empty when there was no save), copies b next to save_dir,
 * then swaps the folders. On any failure the current save is left in place. */
bool restore_backup(const std::string &save_dir, const std::string &root, const BackupInfo &b, const std::string &stamp,
                    std::string *undo, std::string *err);

} // namespace launcher
#endif
