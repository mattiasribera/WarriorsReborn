#include "mods.h"
#include "platform.h"

namespace launcher {

static void add_problem(const char *msg, void *ud) { static_cast<ModList *>(ud)->problems.push_back(msg); }

static void add_check(int kind, const char *name, void *ud) {
    ModCheck &c = *static_cast<ModCheck *>(ud);
    (kind == MODCHECK_LEFTOVER ? c.leftovers : c.missing).push_back(name);
}

ModList mods_scan(const std::string &dir) {
    ModList l;
    if (dir.empty() || !is_dir(dir)) return l;
    l.folder_ok = true;
    std::vector<ModInfo> buf(MOD_MAX);
    int n = modini_scan(dir.c_str(), buf.data(), MOD_MAX, add_problem, &l);
    if (n > 0) l.mods.assign(buf.begin(), buf.begin() + n);
    for (const ModInfo &m : l.mods) {
        ModCheck c;
        c.runtime_ok = modcheck_runtime_ok(&m) != 0;
        modcheck_files(dir.c_str(), &m, add_check, &c);
        l.checks.push_back(c);
    }
    return l;
}

std::string mods_backup_root(const std::string &hdd_dir) { return path_join(hdd_dir, "mods-backups"); }

std::vector<std::string> crash_mods(const std::vector<std::string> &tail) {
    static const char REASON[] = "[crash-context] reason:", MODS[] = "[crash-context] mods: ";
    std::vector<std::string> out;
    bool crashed = false;
    for (const std::string &line : tail) {
        if (line.find(REASON) != std::string::npos) crashed = true, out.clear();   /* the last dump counts */
        size_t at = line.find(MODS);
        if (!crashed || at == std::string::npos) continue;
        std::string list = line.substr(at + sizeof MODS - 1);
        size_t pos = 0;
        while (pos < list.size()) {   /* "id version, id version" */
            size_t end = list.find(", ", pos);
            std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            std::string id = item.substr(0, item.find(' '));
            while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
            if (modini_valid_id(id.c_str())) out.push_back(id);
            pos = end == std::string::npos ? list.size() : end + 2;
        }
    }
    return crashed ? out : std::vector<std::string>();
}

bool crash_notice_due(bool running, const std::string &last_log, const std::string &crash_seen,
                      const std::vector<std::string> &mods) {
    return !running && !last_log.empty() && last_log != crash_seen && !mods.empty();
}

std::vector<std::string> mods_enabled(const Profile &p) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= p.mods.size(); i++) {
        char c = i < p.mods.size() ? p.mods[i] : ',';
        if (c == ',') {
            if (!cur.empty() && modini_valid_id(cur.c_str())) {
                bool dup = false;
                for (const std::string &e : out) dup |= e == cur;
                if (!dup) out.push_back(cur);
            }
            cur.clear();
        } else if (c != ' ') cur += c;
    }
    return out;
}

bool mod_enabled(const Profile &p, const std::string &id) {
    for (const std::string &e : mods_enabled(p))
        if (e == id) return true;
    return false;
}

void mod_set_enabled(Profile &p, const std::string &id, bool on, const std::vector<ModInfo> &found) {
    std::vector<std::string> cur = mods_enabled(p), next;
    for (const ModInfo &m : found)
        if (m.id == id ? on : mod_enabled(p, m.id)) next.push_back(m.id);
    for (const std::string &e : cur) {   /* enabled ids not in the folder (now) */
        bool listed = false;
        for (const ModInfo &m : found) listed |= e == m.id;
        if (!listed && (e != id || on)) next.push_back(e);
    }
    if (on) {
        bool have = false;
        for (const std::string &e : next) have |= e == id;
        if (!have) next.push_back(id);
    }
    std::string s;
    for (const std::string &e : next) s += (s.empty() ? "" : ",") + e;
    p.mods = s;
}

} // namespace launcher
