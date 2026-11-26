#include "settings_sync.h"
#include "platform.h"
#include "../../runtime/src/settings_model.h"
#include <chrono>

namespace launcher {

std::string pending_path_for(const std::string &ini_path) { return ini_path + ".pending"; }
std::string presets_path_for(const std::string &ini_path) { return ini_path + ".presets"; }

SyncMerge sync_merge_file(ProfileFile &file, const std::string &pending_path, std::string *note) {
    std::string text;
    if (!is_file(pending_path) || !read_file(pending_path, text, 1 << 20)) return SyncMerge::None;
    std::vector<std::string> warnings;
    int n = merge_pending(file, text, &warnings);
    if (n < 0) {
        const std::string bad = pending_path + ".bad";
        remove_file(bad);
        if (!rename_path(pending_path, bad)) remove_file(pending_path);
        if (note) *note = "# in-game settings: " + pending_path + " is not a pending settings file (moved to .bad)";
        return SyncMerge::Corrupt;
    }
    remove_file(pending_path);
    if (note) {
        *note = "# in-game settings: " + std::to_string(n) + " change(s) merged from " + pending_path;
        for (const std::string &w : warnings) *note += "\n# " + w;
    }
    return SyncMerge::Merged;
}

SyncMerge sync_merge_and_save(ProfileFile &file, const std::string &ini_path, std::string *note) {
    SyncMerge r = sync_merge_file(file, pending_path_for(ini_path), note);
    if (r == SyncMerge::Merged) {
        std::string err;
        if (!save_ini(ini_path, file, &err) && note) *note += "\n# " + err;
    }
    return r;
}

std::string presets_text(const std::string &install_dir) {
    std::string s = "# warriors-launcher presets for the in-game settings menu (written at each PLAY)\n";
    for (Preset preset : {Preset::Laptop, Preset::Integrated, Preset::Balanced, Preset::Gaming, Preset::Ultra}) {
        Profile p;
        p.draw_distance = p.fx_scale = 0;   /* still 0 afterwards: the preset leaves them as they are */
        apply_preset(p, preset, install_dir);
        s += std::string("\n[preset ") + preset_key(preset) + "]\n";
        for (const auto &kv : profile_pairs(p)) {
            const SmSetting *st = sm_setting(sm_find(kv.first.c_str()));
            if (!st || !(st->flags & SM_PRESET) || ((kv.first == "draw_distance" || kv.first == "fx_scale") && kv.second == "0")) continue;
            s += kv.first + " = " + kv.second + "\n";
        }
    }
    return s;
}

std::vector<std::pair<std::string, std::string>> sync_env(const std::string &ini_path, const std::string &profile,
                                                          const std::string &ui_lang, bool restart) {
    return {{SM_ENV_INI, ini_path},
            {SM_ENV_PROFILE, profile},
            {SM_ENV_PENDING, pending_path_for(ini_path)},
            {SM_ENV_UI_LANGUAGE, ui_lang},
            {SM_ENV_PRESETS, presets_path_for(ini_path)},
            {SM_ENV_RESTART, restart ? "1" : "0"}};
}

bool apply_env(const std::vector<std::pair<std::string, std::string>> &env) {
    bool ok = true;
    for (const auto &kv : env) ok &= env_set(kv.first, kv.second);
    return ok;
}

bool RestartPolicy::after_exit(int exit_code, double run_seconds) {
    if (exit_code != SM_EXIT_RESTART) {
        quick_in_row = 0;
        return false;
    }
    if (run_seconds >= RESTART_SETTLED_SECONDS) {
        quick_in_row = 0;
        return true;
    }
    return ++quick_in_row <= RESTART_MAX_IN_ROW;
}

void add_skip_intro(std::vector<std::string> &argv) {
    for (const std::string &a : argv)
        if (a == "--skip-intro") return;
    argv.push_back("--skip-intro");
}

double sync_clock() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace launcher
