/* Checks that a profile points at a usable copy of the game. Only the header of the user's own
 * default.xbe is read: the "XBEH" magic at 0 and the certificate title (base address at 0x104,
 * certificate address at 0x118, title at certificate + 0x0C, 40 UTF-16LE characters). */
#ifndef LAUNCHER_XBE_CHECK_H
#define LAUNCHER_XBE_CHECK_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "profile.h"

namespace launcher {

#define LAUNCHER_GAME_TITLE "The Warriors"

enum class XbeStatus { Ok, Missing, Unreadable, NotXbe, WrongTitle };

struct XbeCheck {
    XbeStatus status = XbeStatus::Missing;
    std::string title; /* the certificate title (UTF-8) when the header could be read */
};

XbeCheck check_xbe_bytes(const uint8_t *data, size_t size);
XbeCheck check_xbe_file(const std::string &path);
const char *xbe_status_text(XbeStatus s);

struct CopyCheck {
    bool xbrun_ok = false, disc_ok = false;
    bool hdd_ok = false;      /* folder exists (an empty hdd field is ok: xbrun uses ./hdd) */
    bool texpack_ok = false;  /* folder exists, or none set */
    bool input_ok = false;    /* file exists, or none set */
    bool icon_pack_ok = false; /* folder exists, or none set (not required to play) */
    XbeCheck xbe;
    bool ready() const { return xbrun_ok && disc_ok && hdd_ok && texpack_ok && input_ok && xbe.status == XbeStatus::Ok; }
};

/* Relative paths: the xbrun binary is relative to the launcher's folder, and xbrun runs in its
 * own folder, so the disc, hdd, texture pack and input config paths are relative to that. */
std::string xbrun_path(const Profile &p);
/* The key map file the runtime reads: input_config (relative to the game program's folder), else
 * warriors-input.cfg in that folder (the runtime's default; the game runs in that folder). Empty
 * when no game program is set. */
std::string keymap_path(const Profile &p);
/* gamecontrollerdb.txt next to the game program (the runtime reads it there), "" without one. */
std::string pad_db_path(const Profile &p);

/* The game executable in the first of dirs that has one: TheWarriors.exe, then the older
 * xbrun.exe of earlier releases (windows), or xbrun (POSIX). Every dir is searched for the
 * preferred name before the fallback. Empty when none is found. */
std::string find_game_exe(const std::vector<std::string> &dirs, bool windows);
CopyCheck check_copy(const Profile &p);

} // namespace launcher
#endif
