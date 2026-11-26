/* The launcher's side of the in-game settings sync. The game gets
 * at start (environment, runtime/src/settings_model.h SM_ENV_*): the profiles file, the profile's
 * name, the pending-changes file (<ini>.pending), the launcher's language, the presets file
 * (<ini>.presets, written at each PLAY) and whether the launcher waits to relaunch it. The game
 * never writes the profiles file: it writes the keys the player changed to the pending file, and
 * the launcher merges them with profile.cpp's rules (merge_pending) when the game exits, at start-up
 * and in --play. Exit code SM_EXIT_RESTART asks for the same profile again, started with
 * --skip-intro. */
#ifndef LAUNCHER_SETTINGS_SYNC_H
#define LAUNCHER_SETTINGS_SYNC_H
#include <string>
#include <utility>
#include <vector>
#include "profile.h"

namespace launcher {

std::string pending_path_for(const std::string &ini_path);   /* <ini>.pending */
std::string presets_path_for(const std::string &ini_path);   /* <ini>.presets */

/* Merges the pending file into file: None (no file, nothing changed), Merged (applied; the pending
 * file is removed) or Corrupt (not a pending file: file is unchanged and the pending file is moved
 * to <pending>.bad, so it is not read again). note gets a line for the log / sidebar. */
enum class SyncMerge { None, Merged, Corrupt };
SyncMerge sync_merge_file(ProfileFile &file, const std::string &pending_path, std::string *note);
/* sync_merge_file of ini_path's pending file, and the profiles file saved when it merged. */
SyncMerge sync_merge_and_save(ProfileFile &file, const std::string &ini_path, std::string *note);

/* The presets file the game's menu applies (Preajuste): one [preset KEY] section per preset with
 * every SM_PRESET key apply_preset sets for it (draw_distance / fx_scale only where it sets them;
 * the texture and mask packs are never part of it). */
std::string presets_text(const std::string &install_dir);

/* The SM_ENV_* variables for the game: ini path, profile, pending and presets files, the UI language
 * (en|es) and restart = the launcher waits for the game and relaunches on SM_EXIT_RESTART. */
std::vector<std::pair<std::string, std::string>> sync_env(const std::string &ini_path, const std::string &profile,
                                                          const std::string &ui_lang, bool restart);
bool apply_env(const std::vector<std::pair<std::string, std::string>> &env);

/* When a game that exited with exit_code after run_seconds is started again: on SM_EXIT_RESTART,
 * unless it is the (RESTART_MAX_IN_ROW + 1)th restart in a row of runs shorter than
 * RESTART_SETTLED_SECONDS (a game that fails at once is not relaunched forever). Any other exit code
 * ends the chain. */
constexpr double RESTART_SETTLED_SECONDS = 30;
constexpr int RESTART_MAX_IN_ROW = 3;
struct RestartPolicy {
    int quick_in_row = 0;
    bool after_exit(int exit_code, double run_seconds);
};

/* --skip-intro appended unless argv has it (the relaunch goes straight to the title). */
void add_skip_intro(std::vector<std::string> &argv);

/* Seconds on a monotonic clock (run lengths). */
double sync_clock();

} // namespace launcher
#endif
