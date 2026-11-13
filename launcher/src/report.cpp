#include "report.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace launcher {

static const size_t TAIL_BYTES = 2u << 20;

bool read_log_tail(const std::string &path, size_t max_lines, std::vector<std::string> &out) {
    out.clear();
    InputFile f;
    if (!f.open(path)) return false;
    uint64_t size = f.size(), start = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
    std::string text((size_t)(size - start), '\0');
    if (!text.empty() && !f.read_at(start, &text[0], text.size())) return false;
    size_t pos = 0;
    if (start) pos = text.find('\n') == std::string::npos ? text.size() : text.find('\n') + 1; /* a cut first line */
    std::vector<std::string> lines;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos), end = nl == std::string::npos ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = end + 1;
    }
    size_t from = lines.size() > max_lines ? lines.size() - max_lines : 0;
    out.assign(lines.begin() + from, lines.end());
    return true;
}

bool read_log_head(const std::string &path, size_t max_bytes, std::vector<std::string> &out) {
    out.clear();
    std::string text;
    if (!read_file(path, text, max_bytes)) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) break; /* a cut last line */
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        pos = nl + 1;
    }
    return true;
}

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

LogLevel log_level(const std::string &line) {
    if (line.find("FAULT") != std::string::npos) return LogLevel::Error;
    std::string l = lower(line);
    for (const char *w : {"[fatal]", "error", "failed", "cannot"})
        if (l.find(w) != std::string::npos) return LogLevel::Error;
    for (const char *w : {"warning", "warn", "not found", "missing", "skipped"})
        if (l.find(w) != std::string::npos) return LogLevel::Warn;
    return LogLevel::Info;
}

std::vector<ScrubRule> scrub_rules(const std::vector<std::pair<std::string, std::string>> &game_paths, const std::string &home,
                                   const std::string &user, const std::string &host) {
    std::vector<ScrubRule> rules;
    auto add_path = [&](std::string p, const std::string &to) {
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
        if (p.size() >= 3) rules.push_back({p, to, true});
    };
    for (const auto &g : game_paths) add_path(g.first, g.second);
    add_path(home, "~");
    if (user.size() >= 3) rules.push_back({user, "<user>", false});
    if (host.size() >= 3) rules.push_back({host, "<host>", false});
    std::stable_sort(rules.begin(), rules.end(), [](const ScrubRule &a, const ScrubRule &b) { return a.from.size() > b.from.size(); });
    return rules;
}

/* Case-insensitive, and "/" equals "\" for path rules. */
static bool match_at(const std::string &text, size_t at, const ScrubRule &r) {
    if (at + r.from.size() > text.size()) return false;
    for (size_t i = 0; i < r.from.size(); i++) {
        char a = text[at + i], b = r.from[i];
        if (r.path && (a == '\\' || a == '/') && (b == '\\' || b == '/')) continue;
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return false;
    }
    if (r.path) return true;
    auto word = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    size_t end = at + r.from.size();
    return (at == 0 || !word(text[at - 1])) && (end == text.size() || !word(text[end]));
}

std::string scrub(const std::string &text, const std::vector<ScrubRule> &rules) {
    std::string out = text;
    for (const ScrubRule &r : rules) {
        std::string next;
        next.reserve(out.size());
        for (size_t i = 0; i < out.size();)
            if (match_at(out, i, r)) next += r.to, i += r.from.size();
            else next += out[i++];
        out.swap(next);
    }
    return out;
}

/* The rest of the first log line that starts with prefix ("" when none), up to stop (when given). */
static std::string from_log(const std::vector<std::string> &head, const std::vector<std::string> &log, const char *prefix,
                            const char *stop) {
    for (const std::vector<std::string> *lines : {&head, &log}) {
        for (const std::string &l : *lines) {
            if (l.compare(0, strlen(prefix), prefix) != 0) continue;
            std::string v = l.substr(strlen(prefix));
            if (stop) { /* an older runtime prints no driver: "Vulkan on llvmpipe, 640x480" */
                size_t s = v.find(stop);
                if (s == std::string::npos) s = v.find(", ");
                if (s != std::string::npos) v.resize(s);
            }
            return v;
        }
    }
    return "";
}

/* The first whole log line that starts with prefix, in head then log ("" when none). */
static std::string log_line(const std::vector<std::string> &head, const std::vector<std::string> &log, const char *prefix) {
    for (const std::vector<std::string> *lines : {&head, &log})
        for (const std::string &l : *lines)
            if (l.compare(0, strlen(prefix), prefix) == 0) return l;
    return "";
}

/* What drew the frames: the runtime's own lines first, then the profile's renderer, then unknown;
 * the launcher's probe is added when it found no 32-bit Vulkan driver. */
static std::string report_renderer(const ReportInfo &info) {
    const std::vector<std::string> &h = info.head, &l = info.log;
    std::string s, why = from_log(h, l, "[renderer] ERROR: 3D frames will be black: ", nullptr);
    if (why.empty() && !log_line(h, l, "[renderer] Vulkan unavailable (").empty()) {
        why = from_log(h, l, "[renderer] Vulkan unavailable (", nullptr);
        size_t e = why.rfind("); using");
        if (e != std::string::npos) why.resize(e);
    }
    if (!why.empty()) s = "CPU only (Vulkan failed to start: " + why + ")";
    else if (!log_line(h, l, "[renderer] none").empty()) s = "CPU only (renderer set to none)";
    else if (!log_line(h, l, "[renderer] Vulkan on ").empty())
        s = log_line(h, l, "[video] present: readback").empty() ? "Vulkan" : "Vulkan (readback)";
    else if (info.renderer == "none") s = "CPU only (renderer set to none)";
    else s = "unknown (not in the log)";
    if (info.vulkan_driver == 0) s += "; the launcher found no 32-bit Vulkan driver";
    return s;
}

std::string bug_report(const ReportInfo &info, const std::vector<ScrubRule> &rules) {
    std::string runtime = from_log(info.head, info.log, "[start] xbrun ", nullptr);
    std::string gpu = from_log(info.head, info.log, "[renderer] Vulkan on ", "), ");
    if (!gpu.empty() && gpu.find('(') != std::string::npos && gpu.back() != ')') gpu += ")";
    if (gpu.empty()) gpu = from_log(info.head, info.log, "[renderer] ", nullptr);
    std::string s = "The Warriors PC port: bug report\n";
    s += "Launcher: " + (info.launcher_version.empty() ? std::string("unknown") : info.launcher_version) + "\n";
    s += "Runtime: " + (runtime.empty() ? std::string("unknown (not in the log)") : runtime) + "\n";
    s += "OS: " + info.os + "\n";
    s += "GPU: " + (gpu.empty() ? std::string("unknown (not in the log)") : gpu) + "\n";
    s += "Renderer: " + report_renderer(info) + "\n";
    if (std::string vk = log_line(info.head, info.log, "[vk-error] "); !vk.empty()) s += "Vulkan error: " + vk + "\n";
    s += "Command: " + info.command + "\n";
    size_t from = info.log.size() > REPORT_LOG_LINES ? info.log.size() - REPORT_LOG_LINES : 0;
    s += "\nLast " + std::to_string(info.log.size() - from) + " log lines:\n";
    for (size_t i = from; i < info.log.size(); i++) s += info.log[i] + "\n";
    return scrub(s, rules);
}

} // namespace launcher
