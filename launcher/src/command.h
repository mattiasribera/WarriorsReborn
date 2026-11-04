/* Profile -> xbrun command line. The argv is what Play starts (argv[0] is the xbrun binary):
 *   xbrun <disc>/default.xbe <disc> [options]
 * Options at their default are left out, so a default profile runs the bare command. */
#ifndef LAUNCHER_COMMAND_H
#define LAUNCHER_COMMAND_H
#include <string>
#include <vector>
#include "profile.h"

namespace launcher {

std::vector<std::string> build_argv(const Profile &p);
/* With --pad-slot N=KEY for each controller that has a player (pad_slot_args), before the extra
 * args (runtime/src/hle_input.h input_pad_slot_parse). */
std::vector<std::string> build_argv(const Profile &p, const std::vector<ControllerPref> &controllers);
std::vector<std::string> pad_slot_args(const std::vector<ControllerPref> &controllers);

/* The optional smoothed character models: CHARACTERS_HD_DIR next to the launcher. count_wmesh is the
 * number of *.wmesh files in dir (0 when it is missing or empty); add_auto_mesh_pack appends
 * --mesh-pack <dir> when models > 0 and argv has no --mesh-pack yet (a typed one wins). */
extern const char *const CHARACTERS_HD_DIR;
int count_wmesh(const std::string &dir);
void add_auto_mesh_pack(std::vector<std::string> &argv, const std::string &dir, int models);

/* <disc>/default.xbe with the separator style of disc (backslash for Windows paths). */
std::string disc_xbe_path(const std::string &disc);

/* Splits typed extra arguments on whitespace; "..." and '...' group, backslashes are literal
 * (so Windows paths can be typed as they are). */
std::vector<std::string> split_args(const std::string &s);

/* One argument for a POSIX shell / for CommandLineToArgvW (the CreateProcessW command line). */
std::string quote_posix(const std::string &arg);
std::string quote_windows(const std::string &arg);
std::string windows_command_line(const std::vector<std::string> &argv);

/* The command to copy: POSIX sh, or PowerShell on Windows (& "C:\path\TheWarriors.exe" ... when the
 * binary path needs quotes). */
std::string display_command(const std::vector<std::string> &argv, bool windows);

} // namespace launcher
#endif
