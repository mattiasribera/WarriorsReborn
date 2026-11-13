/* The game log in the launcher (the last log, read back with a level filter) and the "Copy report
 * for a bug" text: versions, OS, GPU, the command line and the end of the log, with the player's
 * private details replaced by placeholders (scrub). */
#ifndef LAUNCHER_REPORT_H
#define LAUNCHER_REPORT_H
#include <string>
#include <vector>

namespace launcher {

/* The last max_lines lines of a log (only its last 2 MB are read; CR LF and LF endings).
 * False when the file cannot be read. */
bool read_log_tail(const std::string &path, size_t max_lines, std::vector<std::string> &out);

enum class LogLevel { Info, Warn, Error };
/* By the runtime's wording: FAULT, [fatal], "error", "failed", "cannot" -> Error; "warning",
 * "warn", "not found", "missing", "skipped" -> Warn (case-insensitive). */
LogLevel log_level(const std::string &line);

/* One replacement of the scrubber. Paths match case-insensitively with either separator ("/" or
 * "\"); a word only matches between non-alphanumeric characters. */
struct ScrubRule {
    std::string from, to;
    bool path = false;
};
/* The rules for this player: the given game paths (label = placeholder, e.g. "<game>"), the home
 * folder ("~"), the user name ("<user>") and the machine name ("<host>"); names shorter than 3
 * characters are left alone. Longest first, so a game folder inside home becomes its label. */
std::vector<ScrubRule> scrub_rules(const std::vector<std::pair<std::string, std::string>> &game_paths, const std::string &home,
                                   const std::string &user, const std::string &host);
std::string scrub(const std::string &text, const std::vector<ScrubRule> &rules);

/* The first lines in the first max_bytes of a log (where the runtime prints its version and GPU). */
bool read_log_head(const std::string &path, size_t max_bytes, std::vector<std::string> &out);

struct ReportInfo {
    std::string launcher_version, os, command;
    std::vector<std::string> head; /* the log's first lines (searched for the version and GPU) */
    std::vector<std::string> log;  /* the log's last lines, oldest first (searched too) */
    std::string renderer = "vulkan"; /* the profile's renderer (vulkan|none), when the log does not say */
    int vulkan_driver = -1;          /* the launcher's Vulkan probe: 1 found, 0 no 32-bit driver, -1 not probed */
};
/* The report (plain text, scrubbed): launcher and runtime version ("[start] xbrun <version>" in the
 * log), OS, GPU ("[renderer] Vulkan on <device>, ..." in the log), Renderer (Vulkan, "Vulkan
 * (readback)" or "CPU only (...)" from the log, else the profile's renderer, plus the launcher's
 * probe when it found no 32-bit Vulkan driver), the first "[vk-error]" line when there is one, the
 * command, the last 200 log lines. */
std::string bug_report(const ReportInfo &info, const std::vector<ScrubRule> &rules);

constexpr size_t REPORT_LOG_LINES = 200;

} // namespace launcher
#endif
