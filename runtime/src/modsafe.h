/* The mod safety layer's runtime side (docs/MODDING.md "Safety"):
 *
 * Save backup: when an enabled mod has `saves = yes`, the game's save folder
 * (<hdd>/partition1/UDATA/5454009b, the title's saves and profiles) is copied to
 * <hdd>/mods-backups/<YYYYmmdd-HHMMSS>/ before the title starts, and the newest MODSAFE_KEEP
 * backups are kept. The layout is the launcher's own (launcher/src/backup.h): the copy is in
 * save/, backup.txt says `kind = mods`, files, bytes and a signature of the names, sizes and
 * modification times, so the launcher's Mods page lists and restores these backups with its
 * save-backup code. A save that did not change since the newest backup is not copied again,
 * so starting the game a few times without playing cannot rotate the last good copy out.
 * A copy is made under <stamp>.partial and renamed when complete. Only the save folder is read;
 * only mods-backups is written or deleted.
 *
 * Crash context: the loaded mods are added to the crash dump ("[crash-context] mods: ..."), so
 * the launcher can tell the game closed with mods on. */
#ifndef MODSAFE_H
#define MODSAFE_H
#include <stddef.h>

#define MODSAFE_KEEP 5
#define MODSAFE_TITLE "5454009b"

enum { MODSAFE_DONE, MODSAFE_UNCHANGED, MODSAFE_NOSAVE, MODSAFE_FAILED };

/* Backs up <hdd>/partition1/UDATA/<title> to <hdd>/mods-backups/<stamp> (-2, -3 ... when taken),
 * then deletes the oldest backups beyond keep. out = the backup folder made (DONE) or the newest
 * one (UNCHANGED), or why it failed (FAILED). */
int modsafe_backup(const char *hdd, const char *stamp, int keep, char *out, size_t cap);
/* Number of finished backups in <hdd>/mods-backups (tests). */
int modsafe_count(const char *hdd);

/* After mods_load: the backup when a loaded mod changes the save (logged as
 * `[mods] save backed up to ...`), and the crash-context mods line. hdd = the --hdd folder. */
void mods_safety_start(const char *hdd);
#endif
