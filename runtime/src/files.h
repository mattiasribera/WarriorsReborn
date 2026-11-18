/* Xbox object-path -> host path mapping. */
#ifndef FILES_H
#define FILES_H
#include <stddef.h>
void files_init(const char *disc_dir, const char *hdd_dir);
/* Adds or replaces a symbolic link (e.g. "\??\D:" -> "\Device\CdRom0"). */
void files_add_symlink(const char *name, const char *target);
void files_remove_symlink(const char *name);
const char *files_symlink_target(const char *name);
/* Resolves an Xbox path to a host path. The final component may not exist (for create).
 * Returns 0, or -1 if the device is unknown / an intermediate directory is missing. */
int files_resolve(const char *xbox_path, char *host, size_t hostlen, int *read_only);
/* A file was opened at host path host: a save profile's profile.dat (<...>/UDATA/<title>/<container>/
 * profile.dat, read when a profile loads, written when it saves) makes its container's name the
 * current save profile, logged once when it changes ([save] profile ...). */
void files_note_open(const char *host);
/* The current save profile's container name (a-z A-Z 0-9 _ -), "" before any profile was opened. */
const char *files_profile_id(void);
#endif
