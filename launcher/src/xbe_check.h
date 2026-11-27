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
    bool sndpack_ok = false;  /* folder exists, or none set */
    bool input_ok = false;    /* file exists, or none set */
    bool glyphs_ok = false;   /* the chosen PlayStation style's glyph pack exists, or none is set (not required: the runtime paints its own icons) */
    bool icon_pack_ok = false;    /* the own icon atlas folder (icon_pack) exists, or none set */
    bool glyph_folder_ok = false; /* the glyph packs folder holds ps-black, ps-color or ps-white (find_glyph_packs), or none set */
    XbeCheck xbe;
    bool ready() const { return xbrun_ok && disc_ok && hdd_ok && texpack_ok && sndpack_ok && input_ok && xbe.status == XbeStatus::Ok; }
};

/* Relative paths: the xbrun binary is relative to the launcher's folder, and xbrun runs in its
 * own folder, so the disc, hdd, texture pack and input config paths are relative to that. */
std::string xbrun_path(const Profile &p);
/* The key map file the runtime reads: input_config (relative to the game program's folder), else
 * warriors-input.cfg in that folder (the runtime's default; the game runs in that folder). Empty
 * when no game program is set. */
std::string keymap_path(const Profile &p);
/* The mods folder the game uses: mods_dir, else mods/ next to the game executable ("" when
 * neither is known). Relative paths are relative to the executable's folder, like the others. */
std::string mods_path(const Profile &p);
/* The glyph packs folder as picked, resolved to the one holding ps-black, ps-color or ps-white: the
 * folder itself (trailing separators dropped), its parent when a ps-* folder was picked, or its
 * glyph-packs subfolder (the install folder, or glyph-packs inside glyph-packs). Relative paths are
 * checked from the game program's folder and keep their relative form. "" when none holds a pack. */
std::string find_glyph_packs(const Profile &p);
/* Auto button icons with a PlayStation controller connected at launch: <glyph_packs>/ps-black
 * (resolved by find_glyph_packs) when that folder exists, else "" (the runtime's Xbox or keyboard
 * icons). For add_glyph_pack (command.h). */
std::string auto_glyph_pack(const Profile &p, bool playstation_pad);
/* Auto button icons with a PlayStation controller connected at launch: the glyph pack
 * (auto_glyph_pack, add_glyph_pack) when there is one, else --pad-icons ps-black (the runtime paints
 * the PlayStation icons itself). Nothing for other icon choices or without a PlayStation pad. */
void add_auto_ps_icons(std::vector<std::string> &argv, const Profile &p, bool playstation_pad);
/* The profile a launch builds its command line from, for the button icons: glyph_packs resolved by
 * find_glyph_packs, and cleared when no folder holds the chosen PlayStation style's pack (missing,
 * or another style only), so the runtime's own icons are used instead of a missing --texture-pack.
 * A folder that holds the style keeps winning over the runtime's icons. */
Profile resolve_button_icons(const Profile &p);
/* gamecontrollerdb.txt next to the game program (the runtime reads it there), "" without one. */
std::string pad_db_path(const Profile &p);

/* The game executable in the first of dirs that has one: TheWarriors.exe, then the older
 * xbrun.exe of earlier releases (windows), or xbrun (POSIX). Every dir is searched for the
 * preferred name before the fallback. Empty when none is found. */
std::string find_game_exe(const std::vector<std::string> &dirs, bool windows);

/* ---- The AppImage (tools/appimage) ----
 * Its AppRun sets WARRIORS_DATA_DIR to the writable folder that holds the profiles file, saves,
 * mods and logs; the launcher, xbrun and xbrun-render64 sit together in the read-only image, whose
 * mount point changes on every start. Outside the AppImage the variable is unset. */
std::string data_dir_env();   /* WARRIORS_DATA_DIR, "" when unset or empty */
/* The folder the game runs in, the one relative profile paths (disc, hdd, mods, packs, input
 * config) are relative to: WARRIORS_DATA_DIR when set, else the game executable's folder. */
std::string game_dir(const Profile &p);
/* The first run's game executable: find_game_exe in the profiles file's folder, then one up (as
 * before); else, with WARRIORS_DATA_DIR set, the bare name of the one next to the launcher
 * (launcher_dir), which xbrun_path resolves against the launcher's folder on every start. */
std::string first_run_game_exe(const std::string &ini_dir, const std::string &launcher_dir, bool windows);
/* With WARRIORS_DATA_DIR set (the AppImage): when the profile's game executable is not found and
 * one is bundled next to the launcher (launcher_dir), point the profile at that one by its bare
 * name. The image's mount folder changes on every start, so the user cannot choose it. Returns
 * whether the profile changed. */
bool adopt_bundled_xbrun(Profile &p, const std::string &launcher_dir, bool windows);
CopyCheck check_copy(const Profile &p);

} // namespace launcher
#endif
