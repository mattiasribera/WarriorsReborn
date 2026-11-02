#include "gui.h"
#include "command.h"
#include "imgui.h"
#include "i18n.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "uiscale.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace launcher {

#ifdef _WIN32
#define GAME_EXE_NAME "TheWarriors.exe"
static const bool WINDOWS_HOST = true;
#else
#define GAME_EXE_NAME "xbrun"
static const bool WINDOWS_HOST = false;
#endif

static const double CHECK_PERIOD = 2.0; /* seconds between re-checks of the game copy */

/* ---- Pages ---- */

static const char *const PAGE_KEYS[] = {"home", "display", "game", "controls", "options"};
static const S PAGE_NAMES[] = {S::page_home, S::page_display, S::page_game, S::page_controls, S::page_options};
/* The launcher v2 pages (an older last_page, and --capture names), mapped to the page that has
 * their settings now. */
static const struct {
    const char *key;
    Page page;
} OLD_PAGE_KEYS[] = {
    {"play", Page::Home}, {"graphics", Page::Display}, {"hud", Page::Display},
    {"gameplay", Page::Game}, {"setup", Page::Game}, {"advanced", Page::Options},
};

const char *page_key(Page page) { return PAGE_KEYS[(int)page]; }

bool page_from_key(const std::string &key, Page &out) {
    for (int i = 0; i < (int)Page::Count; i++)
        if (key == PAGE_KEYS[i]) return out = (Page)i, true;
    for (const auto &o : OLD_PAGE_KEYS)
        if (key == o.key) return out = o.page, true;
    return false;
}

/* ---- State ---- */

void app_load(App &app, const std::string &ini_path, const std::string &log_dir) {
    app.ini_path = ini_path;
    app.log_dir = log_dir;
    app.file = ProfileFile();
    app.warnings.clear();
    std::string err;
    bool existed = is_file(ini_path);
    if (existed && !load_ini(ini_path, app.file, &app.warnings, &err)) app.message = err;
    if (app.file.profiles.empty()) { /* first run: only here, saved profiles are never changed */
        /* The game next to the launcher, or one folder up for a launcher/ subfolder. */
        std::string dir = path_dir(ini_path), xbrun = find_game_exe({dir, path_dir(dir)}, WINDOWS_HOST);
        Profile p = recommended_profile(xbrun.empty() ? dir : path_dir(xbrun));
        p.xbrun = xbrun;
        app.file.profiles.push_back(p);
    }
    if (!existed) app.file.settings.setup_done = 0;
    set_ui_lang(lang_from_setting(app.file.settings.ui_language));
    int i = app_find(app, app.file.selected);
    app.selected = i < 0 ? 0 : i;
    app.check = check_copy(*app_profile(app));
    app.check_time = -1;
    if (!page_from_key(app.file.settings.last_page, app.page)) app.page = Page::Home;
    /* An older launcher's file has no setup state: the wizard only when the game is not ready. */
    int done = app.file.settings.setup_done;
    app.wizard = done == 0 || (done < 0 && !app.check.ready());
    app.wizard_step = 0;
    /* A copy that already works was chosen as a folder; otherwise offer the disc image first. */
    app.iso.use_folder = app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok;
}

bool app_save(App &app) {
    app.dirty = false;
    if (app.read_only) return true;
    app.file.selected = app_profile(app)->name;
    app.file.settings.last_page = page_key(app.page);
    std::string err;
    bool ok = save_ini(app.ini_path, app.file, &err);
    if (!ok) app.message = err;
    return ok;
}

Profile *app_profile(App &app) { return &app.file.profiles[app.selected]; }

int app_find(const App &app, const std::string &name) {
    for (size_t i = 0; i < app.file.profiles.size(); i++)
        if (app.file.profiles[i].name == name) return (int)i;
    return -1;
}

std::vector<std::string> launch_argv(const App &app, const Profile &p) {
    std::vector<std::string> argv = build_argv(p, app.file.controllers);
    argv[0] = xbrun_path(p);
    return argv;
}

/* A profile name as a file name part. */
static std::string file_safe(const std::string &name) {
    std::string s;
    for (char c : name) s += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return s.empty() ? "profile" : s;
}

/* ---- Save backups ---- */

static std::string from_install(App &app, const std::string &path);

std::string app_save_dir(App &app) {
    const Profile &p = *app_profile(app);
    return save_folder(from_install(app, p.hdd.empty() ? "hdd" : p.hdd)); /* the runtime's default --hdd */
}

std::string app_backup_root(App &app) {
    std::string data = user_data_dir();
    return data.empty() ? "" : backup_root(data, app_save_dir(app));
}

/* Before PLAY: backs up the save (unless unchanged); the outcome goes to the Options page and a
 * line for the log. A failure never keeps the game from starting. */
static std::string backup_before_play(App &app) {
    BackupState &b = app.backups;
    std::string root = app_backup_root(app), made, err;
    b.listed = -1;
    if (root.empty()) return "# save backup: no user data folder\n";
    BackupResult r = backup_save(app_save_dir(app), root, timestamp(), app.file.settings.backup_count, "play", false, &made, &err);
    b.notice_warn = r == BackupResult::Failed;
    switch (r) {
    case BackupResult::Done:
        b.notice = trf(S::backup_done, backup_date(made.substr(made.find_last_of("/\\") + 1)).c_str());
        return "# save backup: made " + made.substr(made.find_last_of("/\\") + 1) + "\n";
    case BackupResult::Unchanged: b.notice = tr(S::backup_unchanged); return "# save backup: unchanged\n";
    case BackupResult::NoSave: b.notice.clear(); return "# save backup: no save yet\n";
    default: b.notice = trf(S::backup_failed, err.c_str()); return "# save backup: failed: " + err + "\n";
    }
}

bool app_play(App &app, std::string *err) {
    if (app.child.running) {
        if (err) *err = tr(S::err_running);
        return false;
    }
    const Profile &p = *app_profile(app);
    std::vector<std::string> argv = launch_argv(app, p);
    if (!make_dir(app.log_dir)) {
        if (err) *err = trf(S::err_cannot_create, app.log_dir.c_str());
        return false;
    }
    app.log_path = path_join(app.log_dir, file_safe(p.name) + "-" + timestamp() + ".log");
    std::string head = "# warriors-launcher: " + display_command(argv, WINDOWS_HOST) + "\n";
    if (app.file.settings.backup_saves) head += backup_before_play(app);
    write_file(app.log_path, head);
    if (!child_spawn(app.child, argv, path_dir(argv[0]), app.log_path, err)) return false;
    app.launched = true;
    app.run_profile = p.name;
    app.file.settings.last_log = app.log_path;
    app.dirty = true;
    if (app.file.settings.close_on_start) app.quit = true;
    return true;
}

/* The install folder the presets look for packs in: next to the game executable. */
static std::string install_dir(App &app) {
    std::string xbrun = xbrun_path(*app_profile(app));
    return xbrun.empty() ? path_dir(app.ini_path) : path_dir(xbrun);
}

/* A profile path as the game sees it: relative paths are relative to the game's folder. */
static std::string from_install(App &app, const std::string &path) {
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() >= 2 && path[1] == ':'));
    return abs ? path : path_join(install_dir(app), path);
}

/* ---- Plain-language status ---- */

static std::string xbe_plain(const XbeCheck &x) {
    switch (x.status) {
    case XbeStatus::Ok: return tr(S::xbe_found);
    case XbeStatus::Missing: return tr(S::xbe_missing);
    case XbeStatus::Unreadable: return tr(S::xbe_unreadable);
    case XbeStatus::NotXbe: return tr(S::xbe_not_xbe);
    case XbeStatus::WrongTitle: return trf(S::xbe_other, x.title.c_str());
    }
    return "";
}

/* What keeps the profile from playing: a short headline for Home and the sidebar, the detail
 * under it, and the button that opens the page that fixes it. Empty headline = ready. */
struct Problem {
    std::string headline, detail, action;
    Page page = Page::Game;
};

static Problem copy_problem(const CopyCheck &c) {
    const char *NOT_FOUND = tr(S::files_not_found), *INSTALL = tr(S::install_game), *FIX = tr(S::fix_game_page);
    if (!c.xbrun_ok) return {NOT_FOUND, trf(S::cannot_find_exe, GAME_EXE_NAME), tr(S::choose_program), Page::Game};
    if (!c.disc_ok)
        return {NOT_FOUND, tr(S::install_detail), INSTALL, Page::Game};
    if (c.xbe.status != XbeStatus::Ok) return {NOT_FOUND, xbe_plain(c.xbe), INSTALL, Page::Game};
    if (!c.hdd_ok) return {tr(S::save_not_found), tr(S::save_not_exist), FIX, Page::Game};
    if (!c.texpack_ok) return {tr(S::texpack_not_found), tr(S::texpack_not_exist), FIX, Page::Game};
    if (!c.input_ok) return {tr(S::keymap_not_found), tr(S::keymap_not_exist), tr(S::fix_options), Page::Options};
    return {};
}

/* ---- Layout helpers ---- */

static const float LABEL_W = 236, CONTROL_W = 280;
/* The sidebar's width and the rows' label column: roomy at the default window, narrower when the
 * window is narrow in logical pixels (a small screen at a large interface size; app_draw, begin_rows). */
static float SIDEBAR_W = 240, ROW_LABEL_W = 330;

static ImU32 u32(const ImVec4 &c, float alpha = 1) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

static void hint(const char *text, float indent = LABEL_W) {
    if (indent > 0) ImGui::SetCursorPosX(indent);
    ImGui::PushFont(fonts.hint);
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (indent > 0) ImGui::Dummy(ImVec2(0, 4)); /* a little air between form rows */
}

static void colored_line(const ImVec4 &c, const std::string &text, ImFont *font = nullptr) {
    if (font) ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (font) ImGui::PopFont();
}

/* A field's label in the left column; the control follows on the same line. */
static void label(const char *text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(LABEL_W);
}


static void status_tag(bool ok, const std::string &good, const std::string &bad) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ok ? col::good : col::bad, "%s", ok ? good.c_str() : bad.c_str());
}

static void page_title(const char *title, const char *subtitle) {
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6));
}

static void begin_card(const char *id, const char *title, ImVec2 padding = ImVec2(24, 20)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::card);
    ImGui::PushStyleColor(ImGuiCol_Border, col::line);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (title) {
        ImGui::PushFont(fonts.nav);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
    }
}

static void end_card() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 10));
}

/* A clickable card with a title and a description; true when clicked. */
static bool choice_card(const char *id, const char *title, const char *desc, bool selected, ImVec2 size, const char *badge = nullptr) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 end(pos.x + size.x, pos.y + size.y);
    ImVec4 fill = selected ? ImVec4(col::accent.x, col::accent.y, col::accent.z, 0.16f)
                           : ImGui::GetStyle().Colors[hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg];
    dl->AddRectFilled(pos, end, u32(fill), 10);
    if (selected || focused) dl->AddRect(pos, end, u32(col::accent, focused && !selected ? 0.6f : 1), 10, 0, 2);
    /* radio mark */
    ImVec2 c(pos.x + 24, pos.y + 27);
    dl->AddCircle(c, 9, u32(selected ? col::accent : col::dim), 0, 2);
    if (selected) dl->AddCircleFilled(c, 4.5f, u32(col::accent));
    float x = pos.x + 44;
    dl->AddText(fonts.nav, fonts.nav->FontSize, ImVec2(x, pos.y + 15), u32(col::text), title);
    if (badge) {
        float w = fonts.nav->CalcTextSizeA(fonts.nav->FontSize, FLT_MAX, 0, title).x;
        ImVec2 b(x + w + 10, pos.y + 17);
        ImVec2 ts = fonts.hint->CalcTextSizeA(fonts.hint->FontSize, FLT_MAX, 0, badge);
        dl->AddRectFilled(b, ImVec2(b.x + ts.x + 14, b.y + ts.y + 4), u32(col::accent, 0.25f), 6);
        dl->AddText(fonts.hint, fonts.hint->FontSize, ImVec2(b.x + 7, b.y + 2), u32(col::accent_text), badge);
    }
    dl->AddText(fonts.hint, fonts.hint->FontSize, ImVec2(x, pos.y + 44), u32(col::dim), desc, nullptr, end.x - x - 16);
    return clicked;
}

/* The accent PLAY button; disabled while the game copy is not ready or the game runs. */
static void play_button(App &app, ImVec2 size, bool running) {
    bool off = running || !app.check.ready();
    ImGui::BeginDisabled(off);
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, off ? ImGui::GetStyle().Colors[ImGuiCol_FrameBg] : col::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, off ? col::dim : col::on_accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10);
    ImGui::PushFont(fonts.play);
    if (ImGui::Button(running ? tr(S::running_caps) : tr(S::play), size)) {
        std::string err;
        app.message = app_play(app, &err) ? std::string() : trf(S::could_not_start, err.c_str());
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
}

/* A path text field with a Browse button (when the system has a chooser). */
static bool path_field(const char *id, std::string &value, const char *placeholder, bool folder, const char *dialog_title,
                       float width = 420, FileKind kind = FileKind::Program) {
    bool picker = can_pick_paths();
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(picker ? width - 110 : width);
    bool ch = ImGui::InputTextWithHint("##path", placeholder, &value);
    if (picker) {
        ImGui::SameLine();
        if (ImGui::Button(tr(S::browse), ImVec2(98, 0))) {
            std::string start = folder ? value : path_dir(value), out;
            if (folder ? pick_folder(dialog_title, start, out) : pick_file(dialog_title, start, out, kind)) value = out, ch = true;
        }
    }
    ImGui::PopID();
    return ch;
}

/* ---- Fields shared by the pages and the wizard ---- */

static bool field_game_exe(App &app, Profile &p, bool with_hint = true) {
    label(tr(S::game_program));
    bool ch = path_field("exe", p.xbrun, trf(S::path_to, GAME_EXE_NAME).c_str(), false, trf(S::choose_x, GAME_EXE_NAME).c_str());
    status_tag(app.check.xbrun_ok, tr(S::found), p.xbrun.empty() ? tr(S::not_chosen) : tr(S::not_found));
    if (with_hint) hint(trf(S::exe_hint_wizard, GAME_EXE_NAME).c_str());
    return ch;
}

static bool field_disc(App &app, Profile &p, bool with_hint = true) {
    label(tr(S::your_game_copy));
    bool ch = path_field("disc", p.disc, tr(S::folder_with_xbe), true, tr(S::choose_copy_folder));
    bool ok = app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok;
    status_tag(ok, tr(S::found), p.disc.empty() ? tr(S::not_chosen) : tr(S::not_found));
    if (!p.disc.empty()) {
        ImGui::SetCursorPosX(LABEL_W);
        colored_line(ok ? col::good : col::bad,
                     app.check.disc_ok ? xbe_plain(app.check.xbe) : std::string(tr(S::folder_not_exist)), fonts.hint);
    }
    if (with_hint) hint(tr(S::disc_hint_wizard));
    return ch;
}

/* ---- The game from a disc image ---- */

struct IsoJob {
    IsoImage img;
    std::string dest, err;
    IsoProgress progress;
    std::atomic<bool> done{false};
    IsoResult result = IsoResult::Failed;
    Thread thread;
    bool demo = false; /* --capture: no thread */
};

static void iso_worker(void *arg) {
    IsoJob *j = (IsoJob *)arg;
    j->result = iso_extract(j->img, j->dest, &j->progress, &j->err);
    j->done = true;
}

static std::string default_game_dir(App &app) { return path_join(install_dir(app), "game"); }
static std::string default_hdd_dir(App &app) { return path_join(install_dir(app), "hdd"); }

/* The nearest folder of path that exists (free space is asked of it). */
static std::string existing_parent(std::string path) {
    while (!path.empty() && !is_dir(path)) {
        std::string up = path_dir(path);
        if (up == path) break;
        path = up;
    }
    return path;
}

/* Reads the chosen image's folder tables and default.xbe (a moment, on the UI thread). */
static void iso_refresh(IsoSetup &s) {
    if (s.iso == s.opened) return;
    s.opened = s.iso;
    s.img_ok = false;
    s.img_err.clear();
    s.xbe = XbeCheck();
    s.space_for.clear();
    if (s.iso.empty()) return;
    if (!is_file(s.iso)) {
        s.img_err = tr(S::file_not_exist);
        return;
    }
    s.img_ok = iso_open(s.iso, s.img, &s.img_err);
    if (s.img_ok) s.xbe = iso_check_xbe(s.img);
}

static void iso_space(IsoSetup &s) {
    if (!s.img_ok || s.dest.empty() || s.space_for == s.dest) return;
    s.space_for = s.dest;
    s.need = iso_bytes_needed(s.img, s.dest);
    s.avail_known = free_space(existing_parent(s.dest), s.avail);
}

static bool iso_ready_to_extract(const IsoSetup &s) {
    return s.img_ok && s.xbe.status == XbeStatus::Ok && !s.dest.empty() && !s.job && (!s.avail_known || s.avail >= s.need);
}

static void iso_start(App &app) {
    IsoSetup &s = app.iso;
    IsoJob *j = new IsoJob;
    j->img = s.img;
    j->dest = s.dest;
    s.result.clear();
    if (!thread_start(j->thread, iso_worker, j)) {
        delete j;
        s.result = tr(S::extract_not_started);
        s.result_ok = false;
        return;
    }
    s.job = j;
}

/* A finished worker: joined, its outcome reported, and the profile pointed at the copy. */
static bool iso_poll(App &app, Profile &p) {
    IsoSetup &s = app.iso;
    if (!s.job || s.job->demo || !s.job->done) return false;
    thread_join(s.job->thread);
    IsoResult r = s.job->result;
    std::string err = s.job->err;
    delete s.job;
    s.job = nullptr;
    s.space_for.clear();
    s.result_ok = r == IsoResult::Ok;
    if (r == IsoResult::Cancelled) {
        s.result = tr(S::extract_stopped);
        return false;
    }
    if (r == IsoResult::Failed) {
        s.result = trf(S::extract_failed, err.c_str());
        return false;
    }
    s.result = trf(S::extract_done, s.dest.c_str());
    p.disc = s.dest;
    app.check_time = -1;
    return true;
}

void app_shutdown(App &app) {
    IsoJob *j = app.iso.job;
    if (!j) return;
    if (!j->demo) {
        j->progress.cancel = true;
        thread_join(j->thread);
    }
    delete j;
    app.iso.job = nullptr;
}

void app_capture_iso_demo(App &app, bool extracting) {
    IsoSetup &s = app.iso;
    s.use_folder = false;
    s.iso = s.opened = WINDOWS_HOST ? "D:\\Backups\\The Warriors.iso" : "/home/player/Backups/The Warriors.iso";
    s.dest = s.space_for = default_game_dir(app);
    s.img = IsoImage();
    s.img.total_bytes = 5620148362ull;
    s.img.files.resize(431);
    s.img_ok = true;
    s.xbe.status = XbeStatus::Ok;
    s.xbe.title = LAUNCHER_GAME_TITLE;
    s.need = s.img.total_bytes;
    s.avail = 46ull << 30;
    s.avail_known = true;
    if (extracting) {
        s.job = new IsoJob;
        s.job->demo = true;
        s.job->img = s.img;
        s.job->progress.files = 162;
        s.job->progress.bytes = s.img.total_bytes * 38 / 100;
    }
}

static bool accent_button(const char *text, ImVec2 size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, enabled ? col::accent : ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? col::on_accent : col::dim);
    bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return clicked;
}

/* Image, destination, free space, Extract / progress / Cancel. Returns true when the profile changed. */
static bool iso_panel(App &app, Profile &p) {
    IsoSetup &s = app.iso;
    bool running = s.job != nullptr;
    if (s.dest.empty()) s.dest = default_game_dir(app);
    iso_refresh(s);
    iso_space(s);

    ImGui::BeginDisabled(running);
    label(tr(S::disc_image));
    path_field("iso", s.iso, tr(S::your_disc_image), false, tr(S::choose_disc_image), 420, FileKind::DiscImage);
    if (!s.iso.empty()) {
        ImGui::SetCursorPosX(LABEL_W);
        std::string line;
        bool good = false;
        if (!s.img_ok) line = s.img_err;
        else if (s.xbe.status == XbeStatus::Ok)
            line = trf(S::iso_summary, (int)s.img.files.size(), format_bytes(s.img.total_bytes).c_str()),
            good = true;
        else if (s.xbe.status == XbeStatus::WrongTitle)
            line = trf(S::iso_other, s.xbe.title.c_str());
        else if (s.xbe.status == XbeStatus::Missing)
            line = tr(S::iso_no_xbe);
        else
            line = tr(S::iso_xbe_unreadable);
        colored_line(good ? col::good : col::bad, line, fonts.hint);
    }
    label(tr(S::extract_to));
    if (path_field("dest", s.dest, tr(S::folder_for_files), true, tr(S::choose_dest))) s.space_for.clear();
    ImGui::SameLine();
    if (ImGui::Button(tr(S::default_dir))) s.dest = default_game_dir(app), s.space_for.clear();
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::SetCursorPosX(LABEL_W);
    if (running) {
        const IsoProgress &pr = s.job->progress;
        uint64_t total = s.job->img.total_bytes, bytes = pr.bytes;
        float frac = total ? (float)((double)bytes / (double)total) : 1.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col::accent);
        ImGui::ProgressBar(frac, ImVec2(420 - 110, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(tr(S::cancel), ImVec2(98, 0))) s.job->progress.cancel = true;
        ImGui::SetCursorPosX(LABEL_W);
        std::string text = s.job->progress.cancel ? std::string(tr(S::stopping))
                           : trf(S::iso_progress, (int)pr.files.load(), (int)s.job->img.files.size(), format_bytes(bytes).c_str(),
                                 format_bytes(total).c_str());
        hint(text.c_str(), 0);
        return false;
    }
    bool can = iso_ready_to_extract(s) && s.need > 0;
    if (accent_button(tr(S::extract_game), ImVec2(200, 0), can)) iso_start(app);
    if (s.img_ok && s.xbe.status == XbeStatus::Ok && !s.dest.empty()) {
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        std::string free = s.avail_known ? trf(S::free_on_drive, format_bytes(s.avail).c_str()) : std::string(tr(S::free_unknown));
        if (s.need == 0)
            colored_line(col::good, tr(S::already_all), fonts.hint);
        else if (s.avail_known && s.avail < s.need)
            colored_line(col::bad, trf(S::not_enough_space, format_bytes(s.need).c_str(), free.c_str()), fonts.hint);
        else
            colored_line(col::dim, trf(S::needs_space, format_bytes(s.need).c_str(), free.c_str()), fonts.hint);
    }
    if (!s.result.empty()) {
        ImGui::SetCursorPosX(LABEL_W);
        colored_line(s.result_ok ? col::good : col::bad, s.result, fonts.hint);
    } else if (s.img_ok && s.need == 0 && p.disc != s.dest && s.xbe.status == XbeStatus::Ok) {
        ImGui::SetCursorPosX(LABEL_W);
        if (ImGui::Button(tr(S::use_this_folder))) {
            p.disc = s.dest;
            app.check_time = -1;
            return true;
        }
    }
    return false;
}

/* ---- Settings rows: label and description on the left, the control on the right ---- */

static float row_label_h; /* height of the current row's label column, to centre its control */

/* A table of rows inside a card; call end_rows() only when this returns true. */
static bool begin_rows(const char *id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 10));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, col::line);
    float avail = ImGui::GetContentRegionAvail().x * 0.45f;
    ROW_LABEL_W = avail > 330 ? 330 : avail < 200 ? 200 : avail;
    bool open = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH);
    if (open) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ROW_LABEL_W);
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
    } else {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    return open;
}

static void end_rows() {
    ImGui::EndTable();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/* The next row: its name, a short description and an optional warning (e.g. "Heavy on laptops");
 * the cursor is then in the control column. */
static void row(const char *name, const char *desc, const char *warn = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    float y = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ROW_LABEL_W - 28); /* long names wrap in a narrow window */
    ImGui::PushFont(fonts.label);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::PushFont(fonts.hint);
    if (desc && *desc) ImGui::TextColored(col::dim, "%s", desc);
    if (warn) ImGui::TextColored(col::warn, "%s", warn);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    row_label_h = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y - y;
    ImGui::TableSetColumnIndex(1);
}

/* Places a control of w x h at the right end of the control column, centred on the label. */
static void place(float w, float h) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (row_label_h > h) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_label_h - h) / 2);
}

static void focus_ring(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding) {
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) dl->AddRect(a, b, u32(col::accent, 0.7f), rounding, 0, 2);
}

/* Segmented "pill" buttons, right-aligned, wrapped onto more lines when they do not fit. sel is the
 * selected index (-1: none); true when another one was clicked. */
static bool pills(const char *id, int &sel, const char *const *labels, int n) {
    const float pad = 14, inset = 3, gap = 6, h = ImGui::GetFrameHeight();
    float avail = ImGui::GetContentRegionAvail().x;
    std::vector<float> w(n);
    std::vector<int> starts(1, 0);
    std::vector<float> line_w;
    float lw = inset * 2;
    for (int i = 0; i < n; i++) {
        w[i] = ImGui::CalcTextSize(labels[i]).x + pad * 2;
        if (i > starts.back() && lw + w[i] > avail) line_w.push_back(lw), starts.push_back(i), lw = inset * 2;
        lw += w[i];
    }
    line_w.push_back(lw);
    int lines = (int)starts.size();
    float line_h = h + inset * 2;
    place(lines == 1 ? line_w[0] : avail, lines * line_h + (lines - 1) * gap);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float right = origin.x + (lines == 1 ? line_w[0] : avail);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool changed = false;
    ImGui::PushID(id);
    for (int l = 0; l < lines; l++) {
        int end = l + 1 < lines ? starts[l + 1] : n;
        ImVec2 a(right - line_w[l], origin.y + l * (line_h + gap)), b(right, a.y + line_h);
        dl->AddRectFilled(a, b, u32(col::well), 10);
        dl->AddRect(a, b, u32(col::line), 10);
        float x = a.x + inset;
        for (int i = starts[l]; i < end; i++) {
            ImVec2 p(x, a.y + inset), q(x + w[i], a.y + inset + h);
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##pill", ImVec2(w[i], h)) && sel != i) sel = i, changed = true;
            bool hovered = ImGui::IsItemHovered();
            focus_ring(dl, p, q, 8);
            ImGui::PopID();
            if (sel == i) dl->AddRectFilled(p, q, u32(col::accent), 8);
            else if (hovered) dl->AddRectFilled(p, q, u32(col::hover), 8);
            ImVec2 ts = ImGui::CalcTextSize(labels[i]);
            dl->AddText(ImVec2(p.x + (w[i] - ts.x) / 2, p.y + (h - ts.y) / 2), u32(sel == i ? col::on_accent : col::text), labels[i]);
            x += w[i];
        }
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + lines * line_h + (lines - 1) * gap));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

static bool pills_int(const char *id, int &value, const int *values, const char *const *labels, int n) {
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (value == values[i]) sel = i;
    if (!pills(id, sel, labels, n)) return false;
    value = values[sel];
    return true;
}

static bool pills_str(const char *id, std::string &value, const char *const *values, const char *const *labels, int n) {
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (value == values[i]) sel = i;
    if (!pills(id, sel, labels, n)) return false;
    value = values[sel];
    return true;
}

/* An on/off switch, right-aligned; true when flipped. */
static bool toggle(const char *id, bool &v) {
    const float w = 48, h = 26;
    place(w, h);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) v = !v;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, u32(v ? (hovered ? col::accent_hover : col::accent) : hovered ? col::hover_strong : col::off), h / 2);
    float r = h / 2 - 4;
    dl->AddCircleFilled(ImVec2(v ? b.x - h / 2 : a.x + h / 2, a.y + h / 2), r, u32(v ? col::on_accent : col::text));
    focus_ring(dl, a, b, h / 2);
    return clicked;
}

static bool slider_int(const char *id, int &v, int lo, int hi, const char *fmt) {
    place(CONTROL_W, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(CONTROL_W);
    return ImGui::SliderInt(id, &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}

static bool text_input(const char *id, std::string &v, const char *placeholder) {
    place(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputTextWithHint(id, placeholder, &v);
}

/* A path field (with Browse) filling the control column, and its status under it. */
static bool path_row(const char *id, std::string &value, const char *placeholder, bool folder, const char *title,
                     int status = -1, const char *good = tr(S::found), const char *bad = tr(S::not_found)) {
    float h = ImGui::GetFrameHeight() + (status >= 0 ? ImGui::GetTextLineHeightWithSpacing() : 0);
    place(ImGui::GetContentRegionAvail().x, h);
    bool ch = path_field(id, value, placeholder, folder, title, ImGui::GetContentRegionAvail().x);
    if (status >= 0) colored_line(status ? col::good : col::bad, status ? good : bad, fonts.hint);
    return ch;
}

static bool right_button(const char *text) {
    float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2;
    place(w, ImGui::GetFrameHeight());
    return ImGui::Button(text);
}

static bool hud_slider(const char *name, const char *id, float &value, float dflt, float lo, float hi, const char *fmt,
                       const char *help) {
    row(name, help);
    float v = value < 0 ? dflt : value;
    place(CONTROL_W, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(CONTROL_W);
    bool ch = ImGui::SliderFloat(id, &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (ch) value = v;
    return ch;
}

/* ---- Presets (the Display page and the wizard) ---- */

static const S PRESET_TITLES[] = {S::preset_laptop, S::preset_gaming, S::preset_custom};
static const S PRESET_DESCS[] = {S::preset_laptop_desc, S::preset_gaming_desc, S::preset_custom_desc};
static const char *const PRESET_KEYS[] = {"laptop", "gaming", "custom"}; /* choice card ids */

static bool preset_cards(App &app, Profile &p, float height) {
    Preset cur = detect_preset(p);
    int n = cur == Preset::Custom ? 3 : 2;
    float gap = ImGui::GetStyle().ItemSpacing.x, w = (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
    bool ch = false;
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine();
        if (choice_card(PRESET_KEYS[i], tr(PRESET_TITLES[i]), tr(PRESET_DESCS[i]), (int)cur == i, ImVec2(w, height)) && i < 2) {
            apply_preset(p, (Preset)i, install_dir(app));
            ch = true;
        }
    }
    return ch;
}

static void profile_popups(App &app) {
    static std::string rename_buf;
    if (ImGui::BeginPopupModal(tr(S::rename_profile), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            rename_buf = app_profile(app)->name;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(320);
        bool enter = ImGui::InputText("##name", &rename_buf, ImGuiInputTextFlags_EnterReturnsTrue);
        std::string name = rename_buf;
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        int other = app_find(app, name);
        bool valid = !name.empty() && name.find(']') == std::string::npos && (other < 0 || other == app.selected);
        if (!valid) ImGui::TextColored(col::bad, "%s", tr(S::name_invalid));
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(tr(S::rename), ImVec2(120, 0)) || (enter && valid)) {
            app_profile(app)->name = name;
            app.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr(S::cancel), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(tr(S::delete_profile), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(trf(S::delete_profile_q, app_profile(app)->name.c_str()).c_str());
        if (ImGui::Button(tr(S::delete_btn), ImVec2(120, 0))) {
            app.file.profiles.erase(app.file.profiles.begin() + app.selected);
            if (app.selected >= (int)app.file.profiles.size()) app.selected--;
            app.check_time = -1;
            app.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(S::cancel), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* ---- Home ---- */

static const char *const LANG_VALUES[] = {"en", "es", "fr", "de", "it"};
static const char *const LANG_LABELS[] = {"English", "Español", "Français", "Deutsch", "Italiano"};

/* "1920 × 1080", or the original size and its render scale. */
static std::string res_text(const Profile &p) {
    if (p.resolution.empty())
        return p.render_scale > 1 ? "640 × 480 × " + std::to_string(p.render_scale) : std::string("640 × 480");
    std::string s = p.resolution;
    size_t x = s.find('x');
    return x == std::string::npos ? s : s.substr(0, x) + " × " + s.substr(x + 1);
}

/* Opens the page that fixes a problem (the disc image fold-out when the game files are missing). */
static void go_fix(App &app, const Problem &pr) {
    if (pr.page == Page::Options) app.file.settings.show_advanced = true;
    if (pr.page == Page::Game && app.check.xbrun_ok && !(app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok))
        app.iso.setup_open = true;
    app.page = pr.page;
    app.dirty = true;
}

/* A status line: a coloured dot and the text in the label font. */
static void status_line(const ImVec4 &c, const std::string &text) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float line = fonts.label->FontSize;
    dl->AddCircleFilled(ImVec2(pos.x + 6, pos.y + line * 0.5f + 1), 6, u32(c));
    dl->AddCircle(ImVec2(pos.x + 6, pos.y + line * 0.5f + 1), 10, u32(c, 0.25f), 0, 2);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
    colored_line(c, text, fonts.label);
}

/* A clickable summary card that opens its page. */
static void summary_card(App &app, const char *id, const char *caption, const std::string &value, const std::string &detail,
                         Page page, float w) {
    ImVec2 a = ImGui::GetCursorScreenPos(), size(w, 118), b(a.x + w, a.y + size.y);
    if (ImGui::InvisibleButton(id, size)) app.page = page, app.dirty = true;
    bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, u32(col::card), 12);
    dl->AddRect(a, b, hovered ? u32(col::accent, 0.6f) : u32(col::line), 12);
    focus_ring(dl, a, b, 12);
    dl->AddText(fonts.caps, fonts.caps->FontSize, ImVec2(a.x + 20, a.y + 18), u32(col::accent_text), caption);
    dl->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(a.x + 20, a.y + 42), u32(col::text), value.c_str());
    dl->AddText(fonts.hint, fonts.hint->FontSize, ImVec2(a.x + 20, a.y + 80), u32(col::dim), detail.c_str(), nullptr);
}

static void log_view_card(App &app, const std::string &path);

static void page_home(App &app, bool running) {
    Profile &p = *app_profile(app);
    Problem pr = copy_problem(app.check);
    bool ok = pr.headline.empty();

    /* hero */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(34, 30));
    begin_card("hero", nullptr);
    ImGui::PopStyleVar();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wp.x, wp.y + 26), ImVec2(wp.x + 4, wp.y + ws.y - 26), u32(col::accent), 2);
    colored_line(col::accent_text, tr(S::eyebrow), fonts.caps);
    ImGui::PushFont(fonts.hero);
    ImGui::TextUnformatted("THE WARRIORS");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 12));
    if (running) status_line(col::good, tr(S::game_running));
    else if (ok) status_line(col::good, tr(S::ready));
    else {
        status_line(col::bad, pr.headline);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
        colored_line(col::dim, pr.detail, fonts.hint);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
        if (accent_button(pr.action.c_str(), ImVec2(0, 0), true)) go_fix(app, pr);
    }
    end_card();

    /* summaries */
    float gap = 14, w = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
    const char *lang = p.language.c_str();
    for (int i = 0; i < 5; i++)
        if (p.language == LANG_VALUES[i]) lang = LANG_LABELS[i];
    summary_card(app, "sum-display", tr(S::sum_display), p.fullscreen ? tr(S::fullscreen) : tr(S::windowed),
                 res_text(p) + (p.widescreen ? "  ·  16:9" : "  ·  4:3"), Page::Display, w);
    ImGui::SameLine(0, gap);
    summary_card(app, "sum-perf", tr(S::sum_perf), p.sim60 ? "60 FPS" : "30 FPS",
                 tr(p.present == "immediate" ? S::vsync_off : S::vsync_on), Page::Display, w);
    ImGui::SameLine(0, gap);
    summary_card(app, "sum-lang", tr(S::sum_lang), lang, tr(S::voices_english), Page::Game, w);
    ImGui::Dummy(ImVec2(0, 8));

    begin_card("last", tr(S::last_session));
    std::string last_log = app.log_path.empty() ? app.file.settings.last_log : app.log_path;
    if (begin_rows("last-rows")) {
        if (running) row(tr(S::running), trf(S::running_profile, app.run_profile.c_str()).c_str());
        else if (app.launched && app.child.exit_code == 0) row(tr(S::closed_ok), tr(S::closed_ok_desc));
        else if (app.launched)
            row(tr(S::stopped_error), nullptr,
                trf(S::exit_code, app.child.exit_code).c_str());
        else row(tr(S::no_game_yet), last_log.empty() ? tr(S::no_game_started) : tr(S::no_game_since));
        ImGui::BeginDisabled(last_log.empty() || !is_file(last_log));
        LogViewState &lv = app.log_view;
        const char *view = lv.open ? tr(S::hide_log) : tr(S::view_log), *open = tr(S::open_last_log);
        const ImGuiStyle &st = ImGui::GetStyle();
        place(ImGui::CalcTextSize(view).x + ImGui::CalcTextSize(open).x + st.FramePadding.x * 4 + st.ItemSpacing.x, ImGui::GetFrameHeight());
        if (ImGui::Button(view)) lv.open = !lv.open, lv.read = -1, lv.to_end = true;
        ImGui::SameLine();
        if (ImGui::Button(open) && !open_with_desktop(last_log)) app.message = trf(S::cannot_open, last_log.c_str());
        ImGui::EndDisabled();
        row(tr(S::close_on_play), tr(S::close_on_play_desc));
        if (toggle("##close", app.file.settings.close_on_start)) app.dirty = true;
        end_rows();
    }
    end_card();
    if (app.log_view.open && !last_log.empty() && is_file(last_log)) log_view_card(app, last_log);
}

/* ---- Log viewer and bug report ---- */

static const int LOG_VIEW_LINES = 2000;

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "unknown"
#endif

/* The report's scrubber: the profile's game paths (as typed and as the game sees them) become
 * labels, then the home folder, user and machine names. */
static std::vector<ScrubRule> report_rules(App &app) {
    const Profile &p = *app_profile(app);
    std::vector<std::pair<std::string, std::string>> paths;
    auto both = [&](const std::string &v, const char *label) {
        if (v.empty()) return;
        paths.push_back({v, label});
        paths.push_back({from_install(app, v), label});
    };
    both(p.disc, "<game>");
    both(p.hdd.empty() ? std::string("hdd") : p.hdd, "<save-folder>");
    both(p.texture_pack, "<texture-pack>");
    both(p.icon_pack, "<icon-pack>");
    both(p.input_config, "<key-map>");
    paths.push_back({install_dir(app), "<game-program-folder>"});
    paths.push_back({app.log_dir, "<launcher-logs>"});
    paths.push_back({path_dir(app.ini_path), "<launcher-folder>"});
    return scrub_rules(paths, home_dir(), user_name(), host_name());
}

std::string app_bug_report(App &app, const std::string &log_path) {
    ReportInfo info;
    info.launcher_version = LAUNCHER_VERSION;
    info.os = os_description();
    info.command = display_command(launch_argv(app, *app_profile(app)), WINDOWS_HOST);
    read_log_head(log_path, 64 * 1024, info.head);
    read_log_tail(log_path, REPORT_LOG_LINES, info.log);
    return bug_report(info, report_rules(app));
}

/* Re-reads the log when it is new, changed size (the game is writing it) or on Refresh. */
static void log_view_sync(LogViewState &lv, const std::string &path, double now, bool force) {
    uint64_t size = 0;
    file_size(path, size);
    if (!force && path == lv.path && size == lv.size && lv.read >= 0) return;
    if (!force && path == lv.path && lv.read >= 0 && now - lv.read < 1.0) return;
    bool grew = path == lv.path && size != lv.size;
    lv.path = path, lv.size = size, lv.read = now;
    read_log_tail(path, LOG_VIEW_LINES, lv.lines);
    lv.levels.clear();
    for (const std::string &l : lv.lines) lv.levels.push_back(log_level(l));
    lv.to_end |= grew;
}

static void log_view_card(App &app, const std::string &path) {
    LogViewState &lv = app.log_view;
    bool refresh = false;
    begin_card("log", tr(S::game_log));
    log_view_sync(lv, path, ImGui::GetTime(), false);
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    colored_line(col::dim, trf(S::log_of, (int)lv.lines.size(), name.c_str()), fonts.hint);
    ImGui::Dummy(ImVec2(0, 4));
    const char *const filters[] = {tr(S::log_all), tr(S::log_warn), tr(S::log_err)};
    if (ImGui::Button(tr(S::refresh))) refresh = true;
    ImGui::SameLine();
    if (accent_button(tr(S::copy_report), ImVec2(0, 0), true)) {
        ImGui::SetClipboardText(app_bug_report(app, path).c_str());
        lv.notice = tr(S::report_copied);
    }
    ImGui::SameLine(); /* the level filter right-aligned on the buttons' line (or under them) */
    row_label_h = 0;
    if (pills("log-level", lv.filter, filters, 3)) lv.to_end = true;
    if (!lv.notice.empty()) colored_line(col::good, lv.notice, fonts.hint);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::well);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("log-lines", ImVec2(0, 340), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar();
    std::vector<int> shown;
    for (size_t i = 0; i < lv.lines.size(); i++)
        if ((int)lv.levels[i] >= lv.filter) shown.push_back((int)i);
    ImGui::PushFont(fonts.hint);
    if (shown.empty()) ImGui::TextColored(col::dim, "%s", tr(S::log_none_level));
    ImGuiListClipper clip;
    clip.Begin((int)shown.size());
    while (clip.Step())
        for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
            int i = shown[k];
            const ImVec4 &c = lv.levels[i] == LogLevel::Error ? col::bad : lv.levels[i] == LogLevel::Warn ? col::warn : col::text;
            ImGui::TextColored(c, "%s", lv.lines[i].c_str());
        }
    ImGui::PopFont();
    if (lv.to_end) ImGui::SetScrollHereY(1.0f), lv.to_end = false;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    end_card();
    if (refresh) log_view_sync(lv, path, ImGui::GetTime(), true);
}

/* ---- Display ---- */

static bool page_display(App &app, Profile &p) {
    page_title(tr(S::page_display), tr(S::display_sub));
    bool ch = false;

    begin_card("preset", nullptr);
    if (begin_rows("preset-rows")) {
        const char *const preset_labels[] = {tr(S::preset_laptop), tr(S::preset_gaming), tr(S::preset_custom)};
        Preset cur = detect_preset(p);
        row(tr(S::preset), cur == Preset::Custom ? tr(S::preset_custom_desc)
                      : tr(PRESET_DESCS[(int)cur]));
        int sel = (int)cur;
        if (pills("preset", sel, preset_labels, cur == Preset::Custom ? 3 : 2) && sel < 2) {
            apply_preset(p, (Preset)sel, install_dir(app));
            ch = true;
        }
        end_rows();
    }
    end_card();

    begin_card("screen", tr(S::screen));
    if (begin_rows("screen-rows")) {
        const char *const mode_labels[] = {tr(S::windowed), tr(S::fullscreen)};
        row(tr(S::window_mode), tr(S::window_mode_desc));
        int mode = p.fullscreen;
        if (pills("mode", mode, mode_labels, 2)) p.fullscreen = mode == 1, ch = true;

        static const char *const fixed_values[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160", ""};
        const char *const fixed_labels[] = {"720p", "900p", "1080p", "1440p", "4K", tr(S::original)};
        std::vector<std::string> values, labels;
        bool screen_fixed = false;
        for (const char *v : fixed_values) screen_fixed |= app.desktop_res == v;
        if (!app.desktop_res.empty() && !screen_fixed) values.push_back(app.desktop_res), labels.push_back(tr(S::res_screen_pill));
        for (int i = 0; i < 6; i++) values.push_back(fixed_values[i]), labels.push_back(fixed_labels[i]);
        bool known = false;
        for (const std::string &v : values) known |= p.resolution == v;
        if (!known) values.push_back(p.resolution), labels.push_back(res_text(p));
        std::string desc = trf(!app.desktop_res.empty() && p.resolution == app.desktop_res ? S::res_desc_screen : S::res_desc,
                               res_text(p).c_str());
        row(tr(S::resolution), desc.c_str(), p.resolution == "3840x2160" ? tr(S::heavy) : nullptr);
        std::vector<const char *> lp;
        for (const std::string &l : labels) lp.push_back(l.c_str());
        int sel = -1;
        for (size_t i = 0; i < values.size(); i++)
            if (p.resolution == values[i]) sel = (int)i;
        if (pills("res", sel, lp.data(), (int)lp.size())) p.resolution = values[sel], ch = true;

        if (p.resolution.empty()) {
            static const int scale_values[] = {1, 2, 3, 4, 6, 8};
            static const char *const scale_labels[] = {"1x", "2x", "3x", "4x", "6x", "8x"};
            row(tr(S::render_scale), tr(S::render_scale_desc));
            ch |= pills_int("scale", p.render_scale, scale_values, scale_labels, 6);
        }
        row(tr(S::widescreen), tr(S::widescreen_desc));
        ch |= toggle("##widescreen", p.widescreen);
        end_rows();
    }
    end_card();

    begin_card("perf", tr(S::performance));
    if (begin_rows("perf-rows")) {
        static const char *const fps_labels[] = {"30", "60"};
        row(tr(S::frame_rate), tr(p.sim60 ? S::frame_rate_60_desc : S::frame_rate_30_desc));
        int fps = p.sim60;
        if (pills("fps", fps, fps_labels, 2)) p.sim60 = fps == 1, ch = true;
        row("V-Sync", tr(S::vsync_desc));
        bool vsync = p.present != "immediate";
        if (toggle("##vsync", vsync)) p.present = vsync ? "auto" : "immediate", ch = true;
        static const int fif_values[] = {0, 1, 2, 3};
        const char *const fif_labels[] = {tr(S::auto_mode), "1", "2", "3"};
        row(tr(S::fif), tr(S::fif_desc));
        ch |= pills_int("fif", p.frames_in_flight, fif_values, fif_labels, 4);
        end_rows();
    }
    end_card();

    begin_card("quality", tr(S::quality));
    if (begin_rows("quality-rows")) {
        static const int aa_values[] = {0, 2, 4, 8}, ss_values[] = {1, 2, 4}, aniso_values[] = {0, 2, 4, 8, 16};
        const char *const aa_labels[] = {tr(S::off), "2x", "4x", "8x"}, *const ss_labels[] = {tr(S::off), "2x", "4x"};
        const char *const aniso_labels[] = {tr(S::off), "2x", "4x", "8x", "16x"};
        const char *heavy = tr(S::heavy);
        row(tr(S::msaa), tr(S::msaa_desc), p.msaa >= 4 ? heavy : nullptr);
        ch |= pills_int("msaa", p.msaa, aa_values, aa_labels, 4);
        row(tr(S::supersampling), tr(S::supersampling_desc), p.supersample >= 2 ? heavy : nullptr);
        ch |= pills_int("ss", p.supersample, ss_values, ss_labels, 3);
        row(tr(S::texfilter), tr(S::texfilter_desc));
        ch |= pills_int("aniso", p.aniso, aniso_values, aniso_labels, 5);
        static const int shadow_values[] = {1, 2, 4};
        const char *const shadow_labels[] = {tr(S::shadow_normal), tr(S::shadow_high), tr(S::shadow_ultra)};
        row(tr(S::shadow_quality), tr(S::shadow_quality_desc), p.shadow_scale >= 4 ? heavy : nullptr);
        ch |= pills_int("shadow", p.shadow_scale, shadow_values, shadow_labels, 3);
        row(tr(S::soft_shadows), tr(S::soft_shadows_desc));
        ch |= toggle("##soft", p.shadow_soft);
        row(tr(S::shadow_range), tr(S::shadow_range_desc));
        ch |= toggle("##shadow-range", p.shadow_range);
        static const int sharp_values[] = {0, 25, 50, 75, 100};
        const char *const sharp_labels[] = {tr(S::off), "25", "50", "75", "100"};
        row(tr(S::sharpness), tr(S::sharpness_desc));
        ch |= pills_int("sharpen", p.sharpen, sharp_values, sharp_labels, 5);
        row(tr(S::bloom), tr(S::bloom_desc));
        ch |= toggle("##bloom", p.bloom);
        if (p.bloom)
            ch |= hud_slider(tr(S::bloom_strength), "##bloom-strength", p.bloom_strength, 0.6f, 0.1f, 3, "%.1f", tr(S::bloom_strength_desc));
        row(tr(S::rain_hd), tr(S::rain_hd_desc));
        ch |= toggle("##rain-hd", p.rain_hd);
        row(tr(S::ssao), tr(S::ssao_desc));
        ch |= toggle("##ssao", p.ssao);
        if (p.ssao)
            ch |= hud_slider(tr(S::ssao_strength), "##ssao-strength", p.ssao_strength, 0.8f, 0.1f, 2, "%.1f", tr(S::ssao_strength_desc));
        end_rows();
    }
    end_card();

    begin_card("hud", "HUD");
    if (begin_rows("hud-rows")) {
        static const char *const values[] = {"original", "hd", "hd-glass", "glass"};
        const char *const titles[] = {tr(S::original), "HD", tr(S::hud_hdglass), tr(S::hud_glass)};
        const char *const descs[] = {
            tr(S::hud_original_desc),
            tr(S::hud_hd_desc),
            tr(S::hud_hdglass_desc),
            tr(S::hud_glass_desc),
        };
        int sel = -1;
        for (int i = 0; i < 4; i++)
            if (p.hud_style == values[i]) sel = i;
        row(tr(S::health_bars), sel >= 0 ? descs[sel] : tr(S::health_bars_desc));
        if (pills("hud", sel, titles, 4)) p.hud_style = values[sel], ch = true;
        bool hd = p.hud_style == "hd" || p.hud_style == "hd-glass", glass = p.hud_style == "hd-glass";
        if (hd) {
            row(tr(S::customize_bars), tr(S::customize_bars_desc));
            toggle("##customize", app.hud_customize);
        }
        if (hd && app.hud_customize) {
            ch |= hud_slider(tr(S::empty_opacity), "##track", p.hud_track_opacity, glass ? 0.25f : 1, 0, 1, "%.2f",
                             tr(S::empty_opacity_desc));
            ch |= hud_slider(tr(S::fill_opacity), "##fill", p.hud_fill_opacity, glass ? 0.6f : 1, 0, 1, "%.2f",
                             tr(S::fill_opacity_desc));
            ch |= hud_slider(tr(S::frost), "##frost", p.hud_frost, glass ? 4 : 0, 0, 32, "%.1f", tr(S::frost_desc));
            ch |= hud_slider(tr(S::height), "##height", p.hud_height, 1, 0.5f, 4, "%.2fx", tr(S::height_desc));
            row(tr(S::reset), tr(S::reset_desc));
            if (right_button(tr(S::reset_bars))) {
                p.hud_track_opacity = p.hud_fill_opacity = p.hud_frost = p.hud_height = -1;
                ch = true;
            }
        }
        end_rows();
    }
    end_card();
    return ch;
}

/* ---- Game ---- */

static bool page_game(App &app, Profile &p) {
    page_title(tr(S::page_game), tr(S::game_sub));
    bool ch = false;

    begin_card("files", tr(S::game_files));
    Problem pr = copy_problem(app.check);
    bool copy_ok = app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok;
    if (copy_ok) status_line(col::good, tr(S::installed));
    else status_line(col::bad, pr.headline.empty() ? tr(S::files_not_found) : pr.headline);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
    colored_line(col::dim, copy_ok ? from_install(app, p.disc) : tr(S::no_game_files), fonts.hint);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24);
    const char *iso_label = app.iso.setup_open ? tr(S::hide_iso) : tr(S::install_iso);
    if (copy_ok ? ImGui::Button(iso_label) : accent_button(iso_label, ImVec2(0, 0), true)) app.iso.setup_open = !app.iso.setup_open;
    ImGui::SameLine();
    ImGui::BeginDisabled(!copy_ok);
    if (ImGui::Button(tr(S::open_folder)) && !open_with_desktop(from_install(app, p.disc)))
        app.message = trf(S::cannot_open, from_install(app, p.disc).c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr(S::run_wizard))) {
        app.wizard = true;
        app.wizard_step = 0;
    }
    if (app.iso.setup_open || app.iso.job) {
        ImGui::Dummy(ImVec2(0, 8));
        ch |= iso_panel(app, p);
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (begin_rows("files-rows")) {
        row(tr(S::game_program), trf(S::exe_desc, GAME_EXE_NAME).c_str());
        ch |= path_row("exe", p.xbrun, trf(S::path_to, GAME_EXE_NAME).c_str(), false, trf(S::choose_x, GAME_EXE_NAME).c_str(),
                       app.check.xbrun_ok, tr(S::found),
                       p.xbrun.empty() ? tr(S::not_chosen) : tr(S::not_found));
        row(tr(S::game_folder), tr(S::game_folder_desc));
        std::string disc_state = p.disc.empty() ? tr(S::not_chosen) : app.check.disc_ok ? xbe_plain(app.check.xbe) : tr(S::folder_not_exist);
        ch |= path_row("disc", p.disc, tr(S::folder_with_xbe), true, tr(S::choose_copy_folder), copy_ok,
                       disc_state.c_str(), disc_state.c_str());
        row(tr(S::save_folder), tr(S::save_folder_desc));
        ch |= path_row("hdd", p.hdd, tr(S::save_default_hint), true, tr(S::choose_save), app.check.hdd_ok,
                       p.hdd.empty() ? tr(S::default_dir) : tr(S::found), tr(S::not_found));
        end_rows();
    }
    end_card();

    begin_card("text", tr(S::lang_text));
    if (begin_rows("text-rows")) {
        row(tr(S::game_language), tr(S::game_language_desc));
        ch |= pills_str("lang", p.language, LANG_VALUES, LANG_LABELS, 5);
        row(tr(S::fix_typos), tr(S::fix_typos_desc));
        ch |= toggle("##typos", p.text_fixes);
        row(tr(S::spray), tr(S::spray_desc));
        ch |= slider_int("##spray", p.spray_time, SPRAY_TIME_MIN, SPRAY_TIME_MAX, "%d%%");
        end_rows();
    }
    end_card();

    begin_card("addons", tr(S::addons));
    if (begin_rows("addon-rows")) {
        row(tr(S::texture_pack), tr(S::pack_optional));
        ch |= path_row("texpack", p.texture_pack, tr(S::no_texture_pack), true, tr(S::choose_texpack),
                       p.texture_pack.empty() ? -1 : app.check.texpack_ok);
        row(tr(S::extra_args), tr(S::extra_args_desc));
        ch |= text_input("##extra", p.extra_args, tr(S::extra_args_hint));
        end_rows();
    }
    end_card();
    return ch;
}

/* ---- Controls ---- */

/* The actions the page lists, in groups (a group starts where `group` is set). */
static const struct { int control; S name; S group; bool starts; } ACTIONS[] = {
    {CTL_LS_UP, S::act_ls_up, S::group_move, true}, {CTL_LS_DOWN, S::act_ls_down, S::group_move, false},
    {CTL_LS_LEFT, S::act_ls_left, S::group_move, false}, {CTL_LS_RIGHT, S::act_ls_right, S::group_move, false},
    {CTL_X, S::act_x, S::group_actions, true}, {CTL_A, S::act_a, S::group_actions, false},
    {CTL_B, S::act_b, S::group_actions, false}, {CTL_Y, S::act_y, S::group_actions, false},
    {CTL_LT, S::act_lt, S::group_actions, false}, {CTL_RT, S::act_rt, S::group_actions, false},
    {CTL_BLACK, S::act_black, S::group_actions, false}, {CTL_WHITE, S::act_white, S::group_actions, false},
    {CTL_LTHUMB, S::act_lthumb, S::group_actions, false}, {CTL_RTHUMB, S::act_rthumb, S::group_actions, false},
    {CTL_SPIN, S::act_spin, S::group_actions, false},
    {CTL_RS_UP, S::act_rs_up, S::group_camera, true}, {CTL_RS_DOWN, S::act_rs_down, S::group_camera, false},
    {CTL_RS_LEFT, S::act_rs_left, S::group_camera, false}, {CTL_RS_RIGHT, S::act_rs_right, S::group_camera, false},
    {CTL_START, S::act_start, S::group_menus, true}, {CTL_BACK, S::act_back, S::group_menus, false},
    {CTL_UP, S::act_up, S::group_menus, false}, {CTL_DOWN, S::act_down, S::group_menus, false},
    {CTL_LEFT, S::act_left, S::group_menus, false}, {CTL_RIGHT, S::act_right, S::group_menus, false},
};

static const char *action_name(int control) {
    for (const auto &a : ACTIONS)
        if (a.control == control) return tr(a.name);
    return "?";
}

/* ImGui key -> runtime key code. */
static const struct { ImGuiKey imgui; int key; } KEYS[] = {
    {ImGuiKey_Space, ' '}, {ImGuiKey_Enter, '\r'}, {ImGuiKey_KeypadEnter, '\r'}, {ImGuiKey_Backspace, '\b'},
    {ImGuiKey_Tab, '\t'}, {ImGuiKey_UpArrow, KEY_UP}, {ImGuiKey_DownArrow, KEY_DOWN}, {ImGuiKey_LeftArrow, KEY_LEFT},
    {ImGuiKey_RightArrow, KEY_RIGHT}, {ImGuiKey_LeftShift, KEY_LSHIFT}, {ImGuiKey_RightShift, KEY_RSHIFT},
    {ImGuiKey_LeftCtrl, KEY_LCTRL}, {ImGuiKey_RightCtrl, KEY_RCTRL}, {ImGuiKey_LeftAlt, KEY_LALT}, {ImGuiKey_RightAlt, KEY_RALT},
    {ImGuiKey_Minus, '-'}, {ImGuiKey_Equal, '='}, {ImGuiKey_LeftBracket, '['}, {ImGuiKey_RightBracket, ']'},
    {ImGuiKey_Backslash, '\\'}, {ImGuiKey_Semicolon, ';'}, {ImGuiKey_Apostrophe, '\''}, {ImGuiKey_GraveAccent, '`'},
    {ImGuiKey_Comma, ','}, {ImGuiKey_Period, '.'}, {ImGuiKey_Slash, '/'}, {ImGuiKey_Escape, KEY_ESCAPE},
};

/* The key or mouse button pressed this frame (runtime code), or -1. */
static int pressed_key() {
    for (int i = 0; i < 26; i++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + i), false)) return 'a' + i;
    for (int i = 0; i < 10; i++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + i), false)) return '0' + i;
    for (int i = 0; i < 12; i++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + i), false)) return KEY_F1 + i;
    for (const auto &k : KEYS)
        if (ImGui::IsKeyPressed(k.imgui, false)) return k.key;
    static const int mouse[5] = {MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_X1, MOUSE_X2};
    for (int b = 0; b < 5; b++)
        if (ImGui::IsMouseClicked(b)) return mouse[b];
    return -1;
}

/* A key's name in the UI language (letters, digits and symbols as they are). */
static std::string key_label(int key) {
    static const struct { int key; S name; } named[] = {
        {' ', S::key_space}, {'\r', S::key_enter}, {'\b', S::key_backspace}, {KEY_UP, S::key_up}, {KEY_DOWN, S::key_down},
        {KEY_LEFT, S::key_left}, {KEY_RIGHT, S::key_right}, {KEY_LSHIFT, S::key_shift}, {KEY_RSHIFT, S::key_rshift},
        {KEY_RCTRL, S::key_rctrl}, {KEY_RALT, S::key_ralt},
    };
    for (const auto &n : named)
        if (n.key == key) return tr(n.name);
    return key_display_name(key);
}

/* The in-game key cap symbols (runtime kbm_glyphs.c, same strokes): polylines of "xy" points on an
 * 8 x 8 grid, for the keys whose names are long; nullptr for the others. */
static const char *key_symbol_strokes(int key) {
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: return "4084646828240440";
    case '\r': return "717515 331537";
    case '\t': return "0464 426446 8187";
    case '\b': return "042181872704 4365 6345";
    case ' ': return "13167673";
    default: return nullptr;
    }
}
static void draw_key_symbol(ImDrawList *dl, const char *strokes, ImVec2 o, float w, float h, ImU32 ink) {
    for (const char *g = strokes; *g;) {
        if (*g == ' ') { g++; continue; }
        ImVec2 pts[12];
        int n = 0;
        for (; g[0] && g[0] != ' ' && n < 12; g += 2) pts[n++] = ImVec2(o.x + (g[0] - '0') * w / 8, o.y + (g[1] - '0') * h / 8);
        dl->AddPolyline(pts, n, ink, ImDrawFlags_None, 1.5f);
    }
}

/* A small mouse with the bound button in Warriors red, w x h at p. */
static void draw_mouse(ImDrawList *dl, ImVec2 p, float w, float h, int key, ImU32 ink) {
    float r = w * 0.48f, split = p.y + h * 0.44f, cx = p.x + w / 2;
    ImU32 red = u32(col::accent);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(col::sidebar_solid), r);
    if (key == MOUSE_LEFT) dl->AddRectFilled(p, ImVec2(cx, split), red, r, ImDrawFlags_RoundCornersTopLeft);
    if (key == MOUSE_RIGHT) dl->AddRectFilled(ImVec2(cx, p.y), ImVec2(p.x + w, split), red, r, ImDrawFlags_RoundCornersTopRight);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ink, r, 0, 1.4f);
    dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, split), ink, 1.2f);
    dl->AddLine(ImVec2(p.x, split), ImVec2(p.x + w, split), ink, 1.2f);
    ImVec2 wa(cx - w * 0.1f, p.y + h * 0.12f), wb(cx + w * 0.1f, p.y + h * 0.3f);
    dl->AddRectFilled(wa, wb, key == MOUSE_MIDDLE ? red : u32(col::sidebar_solid), w * 0.1f);
    dl->AddRect(wa, wb, ink, w * 0.1f, 0, 1.0f);
    if (key == MOUSE_X1 || key == MOUSE_X2) {
        float y = key == MOUSE_X2 ? split + h * 0.08f : split + h * 0.26f;
        dl->AddRectFilled(ImVec2(p.x - 2, y), ImVec2(p.x + 3, y + h * 0.13f), red, 2);
    }
}

/* A binding as a key cap or a mouse drawing, centred in a slot of size; returns nothing. */
static void draw_binding(ImDrawList *dl, ImVec2 a, ImVec2 size, int key, bool dim) {
    ImU32 ink = u32(dim ? col::dim : col::text);
    float h = 26, cy = a.y + size.y / 2;
    if (key_is_mouse(key)) {
        static const S names[] = {S::mouse_left, S::mouse_middle, S::mouse_right, S::mouse_back, S::mouse_forward};
        const char *label = tr(names[key - MOUSE_LEFT]);
        ImVec2 ts = ImGui::CalcTextSize(label);
        float total = 16 + 8 + ts.x, x = a.x + (size.x - total) / 2;
        draw_mouse(dl, ImVec2(x, cy - 12), 16, 24, key, ink);
        dl->AddText(ImVec2(x + 24, cy - ts.y / 2), ink, label);
        return;
    }
    std::string label = key_label(key);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const char *sym = key_symbol_strokes(key);
    float sw = sym ? (key == ' ' ? 18.0f : 12.0f) : 0, gap = sym ? 5.0f : 0;
    float w = ts.x + sw + gap + 18 < h ? h : ts.x + sw + gap + 18;
    ImVec2 p(a.x + (size.x - w) / 2, cy - h / 2), q(p.x + w, p.y + h);
    dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(q.x, q.y + 2), u32(col::well), 6);
    dl->AddRectFilled(p, q, u32(col::sidebar_solid), 6);
    dl->AddRect(p, q, ink, 6, 0, 1.4f);
    float tx = p.x + (w - ts.x - sw - gap) / 2;
    if (sym) draw_key_symbol(dl, sym, ImVec2(tx, cy - 6), sw, 12, ink);
    dl->AddText(ImVec2(tx + sw + gap, p.y + (h - ts.y) / 2), ink, label.c_str());
}

static void controls_notice(ControlsState &c, const std::string &text, bool warn) {
    c.notice = text;
    c.notice_warn = warn;
}

static void controls_save(App &app) {
    ControlsState &c = app.controls;
    if (app.read_only || c.path.empty()) return;
    std::string err;
    if (save_keymap(c.path, c.map, &err)) c.file_exists = true;
    else controls_notice(c, trf(S::save_failed, err.c_str()), true);
}

/* (Re)loads the key map when the profile's key map file changed. */
static void controls_sync(App &app, const Profile &p) {
    ControlsState &c = app.controls;
    std::string path = keymap_path(p);
    if (c.loaded && path == c.path) return;
    c = ControlsState();
    c.path = path;
    c.loaded = true;
    std::vector<std::string> warnings;
    c.file_exists = !path.empty() && load_keymap(path, c.map, &warnings);
    c.ignored = (int)warnings.size();
}

/* Applies a captured key to the action being captured. */
static void controls_bind(App &app, int key) {
    ControlsState &c = app.controls;
    int control = c.capture_control;
    c.capture_control = -1;
    if (key == KEY_ESCAPE) return;
    if (key_reserved(key)) return controls_notice(c, tr(S::key_reserved), true);
    std::vector<int> keys = c.map.keys_for(control);
    if (c.capture_slot < (int)keys.size() && keys[c.capture_slot] != key) c.map.unbind(keys[c.capture_slot]);
    int prev = c.map.bind(key, control);
    if (prev >= 0 && prev != control)
        controls_notice(c, trf(S::key_moved, key_label(key).c_str(), action_name(prev), action_name(control)), true);
    else c.notice.clear();
    controls_save(app);
}

/* ---- the controller preview and set-up (padmap.h) ---- */

static const float PAD_W = 360, PAD_H = 200;

/* The pad drawing, an Xbox-style controller (no marks): the live state of pad, and a pulsing ring
 * on part (PadTarget::part, -1 none; 100 + an axis: that stick with an arrow in the asked
 * direction). Buttons: 0 A, 1 B, 2 X, 3 Y, 4 Back (View), 5 Guide, 6 Start (Menu), 7/8 stick
 * clicks, 9/10 LB/RB, 11-14 D-pad up, down, left, right; axes 0-3 sticks, 4/5 LT/RT. It fits in
 * PAD_W x (PAD_H - 40). */
static void draw_pad(ImDrawList *dl, ImVec2 o, const PadView &pad, int part) {
    float W = PAD_W, cx = o.x + W / 2;
    /* body: an opaque theme colour, so the overlapping shapes of the outline pass do not show */
    ImU32 ink = u32(col::dim), on = u32(col::accent), body = u32(col::sidebar_solid);
    ImU32 cap = IM_COL32(40, 43, 50, 255), cap_top = IM_COL32(84, 88, 98, 255);
    float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 6);
    ImU32 hl = u32(col::accent_text, pulse);
    auto at = [&](float x, float y) { return ImVec2(o.x + x, o.y + y); };
    auto ring = [&](ImVec2 c0, float r) { dl->AddCircle(c0, r + 5, hl, 0, 3); };
    auto ring_rect = [&](ImVec2 a, ImVec2 b) { dl->AddRect(ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4), hl, 6, 0, 3); };
    ImFont *font = ImGui::GetFont();
    float small = ImGui::GetFontSize() * 0.78f;
    /* A small label at pad coordinates: right-aligned at x (align 1) or left-aligned (0), centred on y. */
    auto label = [&](const char *t, float x, float y, float align, bool lit) {
        ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0, t);
        dl->AddText(font, small, ImVec2(o.x + x - ts.x * align, o.y + y - ts.y / 2), lit ? u32(col::accent_text) : ink, t);
    };

    /* Triggers (behind the body, above the bumpers), with LT/RT. */
    for (int s = 0; s < 2; s++) {
        float t = pad.axis[4 + s], m = s ? -1.0f : 1.0f, edge = s ? W : 0;
        float tx0 = edge + m * 72, tx1 = edge + m * 120;
        ImVec2 ta = at(std::min(tx0, tx1), 0), tb = at(std::max(tx0, tx1), 22);
        dl->AddRectFilled(ta, tb, body, 8, ImDrawFlags_RoundCornersTop);
        if (t > 0.02f) dl->AddRectFilled(ta, ImVec2(tb.x, ta.y + (tb.y - ta.y) * std::min(t, 1.0f)), on, 8, ImDrawFlags_RoundCornersTop);
        dl->AddRect(ta, tb, ink, 8, ImDrawFlags_RoundCornersTop, 1.5f);
        if (part == 104 + s) ring_rect(ta, tb);
        label(s ? "RT" : "LT", edge + m * 66, 9, s ? 0.0f : 1.0f, t > 0.02f);
    }

    /* Body: the upper shell and two grips, outlined as one shape (ink grown by the line width
     * first, then the fill). */
    for (int pass = 0; pass < 2; pass++) {
        float g = pass ? 0 : 1.5f;
        ImU32 c = pass ? body : ink;
        dl->AddRectFilled(at(30 - g, 26 - g), at(W - 30 + g, 112 + g), c, 38 + g);
        dl->AddEllipseFilled(at(70, 110), ImVec2(32 + g, 45 + g), c, 0.5f);
        dl->AddEllipseFilled(at(W - 70, 110), ImVec2(32 + g, 45 + g), c, -0.5f);
        dl->AddEllipseFilled(at(W / 2, 106), ImVec2(100 + g, 20 + g), c);
    }

    /* Bumpers: wide bars on the top edge, with LB/RB. */
    for (int s = 0; s < 2; s++) {
        float m = s ? -1.0f : 1.0f, edge = s ? W : 0, bx0 = edge + m * 50, bx1 = edge + m * 138;
        ImVec2 sa = at(std::min(bx0, bx1), 18), sb = at(std::max(bx0, bx1), 31);
        dl->AddRectFilled(sa, sb, pad.button[9 + s] ? on : body, 6.5f);
        dl->AddRect(sa, sb, ink, 6.5f, 0, 1.5f);
        if (part == 9 + s) ring_rect(sa, sb);
        label(s ? "RB" : "LB", edge + m * 44, 25, s ? 0.0f : 1.0f, pad.button[9 + s]);
    }

    auto stick = [&](ImVec2 c0, float ax, float ay, bool click, int button, int axis) {
        dl->AddCircleFilled(c0, 20, cap);
        dl->AddCircle(c0, 20, ink, 0, 1.5f);
        ImVec2 top(c0.x + ax * 9, c0.y + ay * 9);
        dl->AddCircleFilled(top, 13, click ? on : cap_top);
        dl->AddCircle(top, 13, IM_COL32(0, 0, 0, 120), 0, 1.5f);
        if (part == button) ring(c0, 20);
        if (part == 100 + axis || part == 101 + axis) {   /* the asked direction: right, or up */
            ring(c0, 20);
            ImVec2 d = part == 100 + axis ? ImVec2(1, 0) : ImVec2(0, -1), n(-d.y, d.x);
            ImVec2 tip(c0.x + d.x * 40, c0.y + d.y * 40), base(c0.x + d.x * 30, c0.y + d.y * 30);
            dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * 7, base.y + n.y * 7), ImVec2(base.x - n.x * 7, base.y - n.y * 7), hl);
        }
    };
    stick(at(94, 62), pad.axis[0], pad.axis[1], pad.button[7], 7, 0);        /* left stick: upper left */
    stick(at(W - 134, 104), pad.axis[2], pad.axis[3], pad.button[8], 8, 2);  /* right stick: lower right */

    /* D-pad (lower left): a cross whose arms light up. */
    ImVec2 d = at(134, 104);
    const ImVec2 dirs[4] = {ImVec2(0, -1), ImVec2(0, 1), ImVec2(-1, 0), ImVec2(1, 0)};
    auto arm = [&](int i, float grow) {
        ImVec2 a(d.x + dirs[i].x * 15, d.y + dirs[i].y * 15), w(dirs[i].y ? 5.5f : 0, dirs[i].x ? 5.5f : 0);
        ImVec2 lo(std::min(d.x, a.x) - w.x - grow, std::min(d.y, a.y) - w.y - grow);
        ImVec2 hi(std::max(d.x, a.x) + w.x + grow, std::max(d.y, a.y) + w.y + grow);
        return std::make_pair(lo, hi);
    };
    for (int i = 0; i < 4; i++) dl->AddRectFilled(arm(i, 1.5f).first, arm(i, 1.5f).second, ink, 3);
    for (int i = 0; i < 4; i++) dl->AddRectFilled(arm(i, 0).first, arm(i, 0).second, cap, 2);
    for (int i = 0; i < 4; i++) {
        ImVec2 a(d.x + dirs[i].x * 9.5f, d.y + dirs[i].y * 9.5f);
        if (pad.button[11 + i]) dl->AddRectFilled(ImVec2(a.x - 5, a.y - 5), ImVec2(a.x + 5, a.y + 5), on, 2);
        if (part == 11 + i) ring_rect(ImVec2(a.x - 6, a.y - 6), ImVec2(a.x + 6, a.y + 6));
    }

    /* Face buttons (upper right): coloured letters on dark caps, filled with the colour when lit. */
    static const struct { int b; float dx, dy; const char *l; ImVec4 c; } face[] = {
        {0, 0, 17, "A", ImVec4(0.38f, 0.75f, 0.30f, 1)}, {1, 17, 0, "B", ImVec4(0.88f, 0.27f, 0.25f, 1)},
        {2, -17, 0, "X", ImVec4(0.27f, 0.50f, 0.95f, 1)}, {3, 0, -17, "Y", ImVec4(0.95f, 0.80f, 0.22f, 1)},
    };
    ImVec2 f = at(W - 94, 62);
    for (const auto &fb : face) {
        ImVec2 c0(f.x + fb.dx, f.y + fb.dy);
        ImU32 fc = ImGui::GetColorU32(fb.c);
        dl->AddCircleFilled(c0, 9.5f, pad.button[fb.b] ? fc : cap);
        dl->AddCircle(c0, 9.5f, ink, 0, 1.2f);
        ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0, fb.l);
        dl->AddText(font, small, ImVec2(c0.x - ts.x / 2, c0.y - ts.y / 2), pad.button[fb.b] ? cap : fc, fb.l);
        if (part == fb.b) ring(c0, 9.5f);
    }

    /* View (Back) and Menu (Start), then the guide button: a plain circle. */
    for (int i = 0; i < 2; i++) {
        int b = i ? 6 : 4;
        ImVec2 c0(cx + (i ? 24 : -24), o.y + 66);
        dl->AddCircleFilled(c0, 7, pad.button[b] ? on : cap);
        dl->AddCircle(c0, 7, ink, 0, 1.2f);
        ImU32 mark = pad.button[b] ? cap : ink;
        if (i) {   /* Menu: three lines */
            for (int k = -1; k <= 1; k++) dl->AddLine(ImVec2(c0.x - 3, c0.y + k * 2.2f), ImVec2(c0.x + 3, c0.y + k * 2.2f), mark, 1);
        } else {   /* View: two overlapping windows */
            dl->AddRect(ImVec2(c0.x - 3.5f, c0.y - 2.5f), ImVec2(c0.x + 1, c0.y + 1.5f), mark);
            dl->AddRect(ImVec2(c0.x - 1, c0.y - 0.5f), ImVec2(c0.x + 3.5f, c0.y + 3.5f), mark);
        }
        if (part == b) ring(c0, 7);
    }
    ImVec2 g = at(W / 2, 42);
    dl->AddCircleFilled(g, 11, pad.button[5] ? on : cap);
    dl->AddCircle(g, 11, ink, 0, 1.5f);
    if (part == 5) ring(g, 11);
}

/* The prompt and the short name of each PAD_TARGETS step. */
static const S PADMAP_PROMPT[PAD_TARGET_COUNT] = {
    S::pm_a, S::pm_b, S::pm_x, S::pm_y, S::pm_lb, S::pm_rb, S::pm_lt, S::pm_rt, S::pm_back, S::pm_start, S::pm_guide,
    S::pm_l3, S::pm_r3, S::pm_up, S::pm_down, S::pm_left, S::pm_right, S::pm_lx, S::pm_ly, S::pm_rx, S::pm_ry,
};
static std::string padmap_short(int step) {
    static const char *const fixed[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", nullptr, "L3", "R3"};
    if (step < 13 && fixed[step]) return fixed[step];
    static const S named[] = {S::pm_guide_short, S::act_up, S::act_down, S::act_left, S::act_right,
                              S::pm_lx_short, S::pm_ly_short, S::pm_rx_short, S::pm_ry_short};
    return tr(named[step == 10 ? 0 : step - 12]);
}
static std::string padmap_describe(const PadBinding &b) {
    std::string kind = b.kind == PadBinding::Button ? trf(S::padmap_raw_button, b.index)
                       : b.kind == PadBinding::Hat  ? trf(S::padmap_raw_hat, b.index)
                                                    : trf(S::padmap_raw_axis, b.index);
    return kind + " (" + b.str() + ")";
}

static void padmap_begin(App &app) {
    PadMapState &m = app.padmap;
    m = PadMapState();
    m.active = true;
    m.guid = app.pad.guid;
    m.name = app.pad.name;
    m.capture.start(app.pad.raw);
}

static void padmap_end(App &app, const std::string &notice, bool warn) {
    PadMapState &m = app.padmap;
    m.active = false;
    m.notice = notice;
    m.notice_warn = warn;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
}

/* Writes the mapping line into gamecontrollerdb.txt (replacing this controller's line) and asks
 * main.cpp to use it at once. */
static void padmap_save(App &app) {
    PadMapState &m = app.padmap;
    Profile *p = app_profile(app);
    std::string path = p ? pad_db_path(*p) : "", db, err;
    std::string line = pad_mapping_line(m.guid, m.name, m.binds, pad_platform());
    if (app.read_only) return padmap_end(app, trf(S::padmap_saved, m.name.c_str()), false);
    read_file(path, db, 4 << 20);   /* a missing file starts empty */
    if (path.empty() || !write_file(path, pad_db_merge(db, m.guid, pad_platform(), line)))
        return padmap_end(app, trf(S::save_failed, path.c_str()), true);
    m.apply = line;
    padmap_end(app, trf(S::padmap_saved, m.name.c_str()), false);
}

/* Reset to default: this controller's line removed from the file; main.cpp re-adds SDL's own. */
static void padmap_reset(App &app, const std::string &path) {
    PadMapState &m = app.padmap;
    std::string db;
    if (app.read_only || !read_file(path, db, 4 << 20)) return;
    if (!write_file(path, pad_db_merge(db, app.pad.guid, pad_platform(), ""))) {
        m.notice = trf(S::save_failed, path.c_str());
        m.notice_warn = true;
        return;
    }
    m.guid = app.pad.guid;
    m.reset_guid = app.pad.guid;
}

/* The set-up's steps in the Controller card: one input at a time from the raw joystick, Skip,
 * Back, Cancel (and Esc); gamepad navigation is off meanwhile so the pad only answers the steps. */
static void padmap_card(App &app) {
    PadMapState &m = app.padmap;
    const PadView &pad = app.pad;
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    if (!app.read_only && (!pad.connected || pad.guid != m.guid)) return padmap_end(app, tr(S::padmap_lost), true);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return padmap_end(app, "", false);
    if (m.step < PAD_TARGET_COUNT) {
        PadBinding b;
        if (m.capture.feed(pad.raw, PAD_TARGETS[m.step].kind, b)) {
            int dup = -1;
            for (int i = 0; i < PAD_TARGET_COUNT && dup < 0; i++)
                if (i != m.step && pad_same_input(m.binds[i], b)) dup = i;
            if (dup >= 0) {
                m.notice = trf(S::padmap_dup, padmap_short(dup).c_str());
                m.notice_warn = true;
            } else {
                m.binds[m.step] = b;
                m.last = trf(S::padmap_last, padmap_short(m.step).c_str(), padmap_describe(b).c_str());
                m.notice.clear();
                m.step++;
            }
        }
    }
    colored_line(col::good, trf(S::padmap_title, m.name.c_str()));
    bool done = m.step >= PAD_TARGET_COUNT;
    colored_line(col::dim, done ? std::string(tr(S::padmap_done_desc)) : trf(S::padmap_step, m.step + 1, PAD_TARGET_COUNT), fonts.hint);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(done ? tr(S::padmap_done) : tr(PADMAP_PROMPT[m.step]));
    ImGui::PopFont();
    if (!m.notice.empty()) colored_line(m.notice_warn ? col::warn : col::good, m.notice, fonts.hint);
    else if (!done && m.capture.waiting_release()) colored_line(col::dim, tr(S::padmap_release), fonts.hint);
    else if (!m.last.empty()) colored_line(col::dim, m.last, fonts.hint);
    else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing()));
    ImGui::Dummy(ImVec2(0, 6));
    draw_pad(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), PadView(), done ? -1 : PAD_TARGETS[m.step].part);
    ImGui::Dummy(ImVec2(PAD_W, PAD_H - 40));
    bool any = false;
    for (const PadBinding &b : m.binds) any |= b.kind != PadBinding::None;
    if (done) {
        if (accent_button(tr(S::padmap_save), ImVec2(0, 0), any)) return padmap_save(app);
        ImGui::SameLine(0, 12);
    } else {
        if (ImGui::Button(tr(S::padmap_skip))) m.binds[m.step] = PadBinding(), m.step++, m.notice.clear(), m.capture.restart();
        ImGui::SameLine(0, 12);
    }
    ImGui::BeginDisabled(m.step == 0);
    if (ImGui::Button(tr(S::back))) {
        m.step--;
        m.binds[m.step] = PadBinding();
        m.notice.clear();
        m.last.clear();
        m.capture.restart();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 12);
    if (ImGui::Button(tr(S::cancel))) return padmap_end(app, "", false);
    ImGui::SameLine(0, 16);
    colored_line(col::dim, tr(S::padmap_esc), fonts.hint);
}

/* A controller's name: the one given in the Controllers card, else SDL's. */
static std::string pad_display_name(const App &app, const PadView &pad) {
    const ControllerPref *c = find_controller(app.file, pad.key);
    return c && !c->name.empty() ? c->name : pad.name;
}

/* The Controllers card: every attached controller, and one that has a player but is not attached,
 * with its name (editable), short ID, player (Auto or 1-4, ControllerPref), Set up buttons and
 * Test (both show it in the Controller card below). Identical controllers share a GUID and so
 * one button set-up, which their rows say. */
static bool controllers_card(App &app) {
    bool ch = false;
    begin_card("pads", tr(S::pads_card));
    colored_line(col::dim, tr(S::pads_desc), fonts.hint);
    ImGui::Dummy(ImVec2(0, 4));
    std::vector<std::string> keys;
    for (const PadView &v : app.pads) keys.push_back(v.key);
    for (const ControllerPref &c : app.file.controllers)
        if (c.player && std::find(keys.begin(), keys.end(), c.key) == keys.end()) keys.push_back(c.key);
    if (keys.empty()) colored_line(col::dim, tr(S::pad_none), fonts.hint);
    std::string players[5] = {tr(S::pad_player_auto)};
    for (int n = 1; n <= 4; n++) players[n] = trf(S::pad_player_n, n);
    const float player_w = 150;
    for (size_t i = 0; i < keys.size(); i++) {
        const std::string &key = keys[i];
        const PadView *pad = i < app.pads.size() ? &app.pads[i] : nullptr;
        const ControllerPref *pref = find_controller(app.file, key);
        ImGui::PushID(key.c_str());
        if (i) ImGui::Separator();
        std::string name = pref ? pref->name : "";
        std::string sdl_name = pad ? pad->name : key;
        float buttons_w = ImGui::CalcTextSize(tr(S::pad_setup)).x + ImGui::CalcTextSize(tr(S::pad_test)).x +
                          ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 3;
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - player_w - buttons_w - 8));
        if (ImGui::InputTextWithHint("##name", sdl_name.c_str(), &name)) {
            if (name.size() > 60) name.resize(60);
            controller_pref(app.file, key).name = name;
            ch = true;
        }
        ImGui::SameLine();
        int player = pref ? pref->player : 0;
        ImGui::SetNextItemWidth(player_w);
        if (ImGui::BeginCombo("##player", players[player].c_str())) {
            for (int n = 0; n <= 4; n++)
                if (ImGui::Selectable(players[n].c_str(), n == player)) {
                    set_controller_player(app.file, key, n);
                    ch = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!pad || app.padmap.active || pad_db_path(*app_profile(app)).empty());
        if (ImGui::Button(tr(S::pad_setup))) {
            app.pad_selected = key;
            app.pad = *pad;
            app.pad_scroll = true;
            padmap_begin(app);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!pad || app.padmap.active || app.pad.key == key);
        if (ImGui::Button(tr(S::pad_test))) {
            app.pad_selected = key;
            app.pad = *pad;
            app.pad_scroll = true;
        }
        ImGui::EndDisabled();
        std::string info = pad ? pad_guid_short(pad->guid) : std::string(tr(S::pad_absent));
        if (pad && pad->custom) info += "  \xc2\xb7  " + std::string(tr(S::padmap_custom));
        if (pad && !pad->is_controller) info += "  \xc2\xb7  " + std::string(tr(S::pad_not_controller));
        if (pad && app.pad.key == key && app.pads.size() > 1) info += "  \xc2\xb7  " + std::string(tr(S::pad_shown_below));
        colored_line(col::dim, info, fonts.hint);
        for (const PadView &other : app.pads)   /* the first other controller of the same model */
            if (pad && other.key != key && other.guid == pad->guid) {
                colored_line(col::dim, trf(S::pad_same_model, pad_display_name(app, other).c_str()), fonts.hint);
                break;
            }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    colored_line(col::dim, tr(S::pads_note), fonts.hint);
    end_card();
    return ch;
}

static bool page_controls(App &app, Profile &p) {
    page_title(tr(S::page_controls), tr(S::controls_sub));
    bool ch = false;
    ControlsState &c = app.controls;
    controls_sync(app, p);
    ImGuiIO &io = ImGui::GetIO();
    if (c.capture_control >= 0) {   /* no keyboard navigation while a key is being captured */
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        int key = ++c.capture_frames > 1 ? pressed_key() : -1;
        if (key >= 0) controls_bind(app, key);
    } else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    begin_card("kbm", tr(S::kbm_card));
    colored_line(col::dim, tr(S::kbm_help), fonts.hint);
    std::string where = c.path.empty() ? tr(S::key_file_none)
                        : c.file_exists ? trf(S::key_file, c.path.c_str())
                                        : trf(S::key_file_new, c.path.c_str());
    colored_line(c.path.empty() ? col::warn : col::dim, where, fonts.hint);
    if (c.ignored) colored_line(col::warn, trf(S::key_file_warn, c.ignored), fonts.hint);
    if (c.capture_control >= 0) colored_line(col::accent_text, trf(S::press_key, action_name(c.capture_control)));
    else if (!c.notice.empty()) colored_line(c.notice_warn ? col::warn : col::good, c.notice);
    if (ImGui::Button(tr(S::reset_defaults))) {
        c.map.reset();
        c.capture_control = -1;
        controls_notice(c, tr(S::reset_done), false);
        controls_save(app);
    }
    ImGui::Dummy(ImVec2(0, 6));
    const float slot_w = 150, slot_h = 34;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 3));
    if (ImGui::BeginTable("actions", 3, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("key1", ImGuiTableColumnFlags_WidthFixed, slot_w + 8);
        ImGui::TableSetupColumn("key2", ImGuiTableColumnFlags_WidthFixed, slot_w + 8);
        for (const auto &act : ACTIONS) {
            if (act.starts) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushFont(fonts.caps);
                ImGui::TextColored(col::dim, "%s", tr(act.group));
                ImGui::PopFont();
            }
            std::vector<int> keys = c.map.keys_for(act.control);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            float y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(y + (slot_h - ImGui::GetTextLineHeight()) / 2);
            ImGui::TextUnformatted(tr(act.name));
            if (keys.empty()) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(col::warn, "%s", tr(S::not_bound));
            }
            for (int slot = 0; slot < 2; slot++) {
                ImGui::TableSetColumnIndex(1 + slot);
                ImGui::PushID(act.control * 4 + slot);
                ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + slot_w, a.y + slot_h);
                bool clicked = ImGui::InvisibleButton("##slot", ImVec2(slot_w, slot_h));
                bool hovered = ImGui::IsItemHovered();
                bool capturing = c.capture_control == act.control && c.capture_slot == slot;
                if (clicked && c.capture_control < 0) {
                    c.capture_control = act.control;
                    c.capture_slot = slot < (int)keys.size() ? slot : (int)keys.size();
                    c.capture_frames = 0;
                }
                if (hovered && c.capture_control < 0 && slot < (int)keys.size() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    controls_notice(c, trf(S::key_cleared, key_label(keys[slot]).c_str()), false);
                    c.map.unbind(keys[slot]);
                    controls_save(app);
                }
                focus_ring(dl, a, b, 8);
                dl->AddRectFilled(a, b, u32(capturing ? col::accent : hovered ? col::hover : col::well, capturing ? 0.35f : 1), 8);
                if (capturing) {
                    ImVec2 ts = ImGui::CalcTextSize(tr(S::press_short));
                    dl->AddText(ImVec2(a.x + (slot_w - ts.x) / 2, a.y + (slot_h - ts.y) / 2), u32(col::text), tr(S::press_short));
                } else if (slot < (int)keys.size()) {
                    draw_binding(dl, a, ImVec2(slot_w, slot_h), keys[slot], false);
                    if (slot == 1 && keys.size() > 2) dl->AddText(ImVec2(b.x - 22, a.y + 2), u32(col::dim), ("+" + std::to_string(keys.size() - 2)).c_str());
                } else {
                    ImVec2 ts = ImGui::CalcTextSize("+");
                    dl->AddText(ImVec2(a.x + (slot_w - ts.x) / 2, a.y + (slot_h - ts.y) / 2), u32(col::dim), "+");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    end_card();

    ch |= controllers_card(app);
    if (app.pad_scroll) ImGui::SetScrollHereY(0.1f);   /* Set up buttons or Test in the Controllers card */
    app.pad_scroll = false;
    begin_card("pad", tr(S::pad_card));
    const PadView &pad = app.pad;
    PadMapState &m = app.padmap;
    if (m.active) padmap_card(app);
    else if (!pad.connected) colored_line(col::dim, tr(S::pad_none), fonts.hint);
    else {
        colored_line(col::good, pad_display_name(app, pad));
        if (pad.custom) {
            ImGui::SameLine(0, 12);
            colored_line(col::accent_text, tr(S::padmap_custom), fonts.hint);
        }
        colored_line(pad.is_controller ? col::dim : col::warn, tr(pad.is_controller ? S::pad_live : S::padmap_unknown), fonts.hint);
        ImGui::Dummy(ImVec2(0, 6));
        draw_pad(dl, ImGui::GetCursorScreenPos(), pad, -1);
        ImGui::Dummy(ImVec2(PAD_W, PAD_H - 40));
    }
    if (!m.active && begin_rows("pad-rows")) {
        row(tr(S::vibration), tr(S::vibration_desc));
        float tw = ImGui::CalcTextSize(tr(S::test_vibration)).x + ImGui::GetStyle().FramePadding.x * 2;
        place(48 + 12 + tw, 26);
        ImGui::BeginDisabled(!pad.connected || !pad.can_rumble || !p.rumble);
        if (ImGui::Button(tr(S::test_vibration))) app.rumble_test = true;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12);
        ch |= toggle("##rumble", p.rumble);
        end_rows();
    }
    if (!m.active) {
        if (!m.notice.empty()) colored_line(m.notice_warn ? col::warn : col::good, m.notice);
        colored_line(col::dim, tr(S::pad_remap_note), fonts.hint);
        std::string db = pad_db_path(p);
        if (db.empty()) colored_line(col::warn, tr(S::padmap_no_program), fonts.hint);
        ImGui::BeginDisabled(!pad.connected || db.empty());
        if (ImGui::Button(tr(S::padmap_start))) padmap_begin(app);
        ImGui::EndDisabled();
        if (pad.connected && pad.custom) {
            ImGui::SameLine(0, 12);
            if (ImGui::Button(tr(S::padmap_reset))) padmap_reset(app, db);
        }
    }
    end_card();

    begin_card("icons", tr(S::icons_card));
    if (begin_rows("icon-rows")) {
        static const char *const icon_values[] = {"auto", "xbox", "kbm"};
        const char *const icon_labels[] = {tr(S::icons_auto), "Xbox", tr(S::icons_kbm)};
        row(tr(S::button_icons), tr(S::button_icons_desc));
        ch |= pills_str("icons", p.button_icons, icon_values, icon_labels, 3);
        row(tr(S::glyph_pack), tr(S::pack_optional), app.check.icon_pack_ok ? nullptr : tr(S::pack_not_found));
        ch |= path_row("glyphs", p.icon_pack, tr(S::glyph_pack_hint), true, tr(S::choose_glyphs),
                       p.icon_pack.empty() ? -1 : app.check.icon_pack_ok);
        end_rows();
    }
    colored_line(col::dim, tr(S::icons_kbm_note), fonts.hint);
    end_card();
    return ch;
}

/* ---- Options ---- */

/* The Save backups card: the setting, how many to keep, the backups with Restore, the folder. */
static void backups_card(App &app, bool running) {
    BackupState &b = app.backups;
    LauncherSettings &st = app.file.settings;
    std::string save = app_save_dir(app), root = app_backup_root(app);
    double now = ImGui::GetTime();
    if (b.listed < 0 || now - b.listed > CHECK_PERIOD || save != b.save || root != b.root) {
        b.save = save, b.root = root, b.listed = now;
        b.list = root.empty() ? std::vector<BackupInfo>() : list_backups(root);
    }
    begin_card("backups", tr(S::backups));
    if (begin_rows("backup-rows")) {
        row(tr(S::backup_on), tr(S::backup_on_desc));
        if (toggle("##backup", st.backup_saves)) app.dirty = true;
        row(tr(S::backup_count), tr(S::backup_count_desc));
        if (slider_int("##backup-count", st.backup_count, BACKUP_COUNT_MIN, BACKUP_COUNT_MAX, "%d")) app.dirty = true;
        if (root.empty()) row(tr(S::backup_folder), nullptr, tr(S::backup_unavailable));
        else {
            row(tr(S::backup_folder), tr(S::backup_folder_desc));
            ImGui::BeginDisabled(!is_dir(root));
            if (right_button(tr(S::open_folder)) && !open_with_desktop(root)) app.message = trf(S::cannot_open, root.c_str());
            ImGui::EndDisabled();
        }
        row(tr(S::backup_list), b.list.empty() ? tr(S::backup_none) : tr(S::backup_list_desc));
        for (size_t i = 0; i < b.list.size(); i++) {
            const BackupInfo &e = b.list[i];
            std::string date = backup_date(e.name), detail = trf(S::backup_detail, format_bytes(e.bytes).c_str(), e.files);
            if (e.kind == "restore") detail = std::string(tr(S::backup_before_restore)) + "  ·  " + detail;
            row(date.c_str(), detail.c_str());
            ImGui::PushID((int)i);
            ImGui::BeginDisabled(running);
            if (right_button(tr(S::restore))) {
                b.confirm = (int)i;
                ImGui::OpenPopup(tr(S::restore_title));
            }
            ImGui::EndDisabled();
            if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(S::restore_running));
            if (b.confirm == (int)i && ImGui::BeginPopupModal(tr(S::restore_title), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(trf(S::restore_q, date.c_str()).c_str());
                colored_line(col::dim, tr(S::restore_note), fonts.hint);
                ImGui::Dummy(ImVec2(0, 4));
                if (accent_button(tr(S::restore), ImVec2(140, 0), !running)) {
                    std::string undo, err;
                    BackupInfo pick = e; /* the list is re-read below */
                    if (restore_backup(save, root, pick, timestamp(), &undo, &err)) b.notice = trf(S::restore_done, date.c_str()), b.notice_warn = false;
                    else b.notice = trf(S::restore_failed, err.c_str()), b.notice_warn = true;
                    b.listed = -1;
                    b.confirm = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr(S::cancel), ImVec2(140, 0))) b.confirm = -1, ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        end_rows();
    }
    if (!b.notice.empty()) colored_line(b.notice_warn ? col::warn : col::dim, b.notice, fonts.hint);
    end_card();
}

static bool page_options(App &app, Profile &p, bool running) {
    page_title(tr(S::page_options), tr(S::options_sub));
    bool ch = false;

    begin_card("profiles", tr(S::profiles));
    if (begin_rows("profile-rows")) {
        row(tr(S::profile), tr(S::profile_desc));
        place(260, ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(260);
        ImGui::BeginDisabled(running);
        if (ImGui::BeginCombo("##profile", p.name.c_str())) {
            for (size_t i = 0; i < app.file.profiles.size(); i++) {
                ImGui::PushID((int)i);
                if (ImGui::Selectable(app.file.profiles[i].name.c_str(), app.selected == (int)i)) {
                    app.selected = (int)i;
                    app.check_time = -1;
                    app.dirty = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        row(tr(S::manage), tr(S::manage_desc));
        const char *names[] = {tr(S::new_btn), tr(S::duplicate), tr(S::rename), tr(S::delete_btn)};
        float w = -ImGui::GetStyle().ItemSpacing.x;
        for (const char *n : names) w += ImGui::CalcTextSize(n).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        const bool wrap = w > ImGui::GetContentRegionAvail().x; /* a narrow window: two per line */
        if (!wrap) place(w, ImGui::GetFrameHeight());
        if (ImGui::Button(tr(S::new_btn))) {
            /* Keep the game copy, reset the options to the recommended ones (packs next to the game). */
            Profile n = recommended_profile(install_dir(app));
            n.xbrun = p.xbrun;
            n.disc = p.disc;
            n.hdd = p.hdd;
            n.name = unique_profile_name(app.file, "new");
            app.file.profiles.push_back(n);
            app.selected = (int)app.file.profiles.size() - 1;
            app.check_time = -1;
            app.dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(S::duplicate))) {
            Profile d = p;
            d.name = unique_profile_name(app.file, p.name);
            app.file.profiles.push_back(d);
            app.selected = (int)app.file.profiles.size() - 1;
            app.dirty = true;
        }
        if (!wrap) ImGui::SameLine();
        if (ImGui::Button(tr(S::rename))) ImGui::OpenPopup(tr(S::rename_profile));
        ImGui::SameLine();
        ImGui::BeginDisabled(app.file.profiles.size() < 2);
        if (ImGui::Button(tr(S::delete_btn))) ImGui::OpenPopup(tr(S::delete_profile));
        ImGui::EndDisabled();
        profile_popups(app);
        end_rows();
    }
    end_card();
    Profile &cur = *app_profile(app); /* New, Duplicate or Delete may have changed the selection */

    begin_card("launcher", tr(S::launcher));
    if (begin_rows("launcher-rows")) {
        row(tr(S::ui_language), tr(S::ui_language_desc));
        int lang = (int)ui_lang();
        if (pills("ui-lang", lang, LANG_LABELS, 2)) { /* LANG_LABELS starts with English, Español */
            set_ui_lang((Lang)lang);
            app.file.settings.ui_language = lang_key((Lang)lang);
            app.dirty = true;
        }
        {
            char cur[16];
            snprintf(cur, sizeof cur, "%gx", app.ui_scale_auto);
            std::string auto_label = trf(S::ui_scale_auto, cur);
            static const float scales[] = {0, 1, 1.25f, 1.5f};
            const char *const labels[] = {auto_label.c_str(), "1x", "1.25x", "1.5x"};
            float want = ui_scale_from_setting(app.file.settings.ui_scale);
            int sel = 0;
            for (int i = 1; i < 4; i++)
                if (want == scales[i]) sel = i;
            row(tr(S::ui_scale), tr(S::ui_scale_desc));
            if (pills("ui-scale", sel, labels, 4)) {
                app.file.settings.ui_scale = ui_scale_setting(scales[sel]);
                app.dirty = true;
            }
        }
        row(tr(S::close_on_play), tr(S::close_on_play_desc));
        if (toggle("##close", app.file.settings.close_on_start)) app.dirty = true;
        row(tr(S::show_advanced), tr(S::show_advanced_desc));
        if (toggle("##advanced", app.file.settings.show_advanced)) app.dirty = true;
        end_rows();
    }
    end_card();

    backups_card(app, running);

    if (app.file.settings.show_advanced) {
        begin_card("render", tr(S::rendering));
        if (begin_rows("render-rows")) {
            const char *const renderer_values[] = {"vulkan", "none"}, *const renderer_labels[] = {"Vulkan", tr(S::none)};
            static const char *const present_values[] = {"auto", "mailbox", "fifo", "immediate", "readback"};
            const char *const present_labels[] = {tr(S::auto_mode), "Mailbox", "FIFO", tr(S::immediate), "Readback"};
            row(tr(S::renderer), tr(S::renderer_desc));
            ch |= pills_str("renderer", cur.renderer, renderer_values, renderer_labels, 2);
            row(tr(S::present_mode), tr(S::present_mode_desc));
            ch |= pills_str("present", cur.present, present_values, present_labels, 5);
            row(tr(S::res_typed), tr(S::res_typed_desc));
            ch |= text_input("##res", cur.resolution, "e.g. 1366x768");
            row(tr(S::render_targets), tr(S::render_targets_desc));
            ch |= toggle("##rt", cur.render_targets);
            end_rows();
        }
        end_card();

        begin_card("backends", tr(S::backends));
        if (begin_rows("backend-rows")) {
            const char *const audio_values[] = {"sdl", "null"}, *const audio_labels[] = {"SDL", tr(S::none)};
            const char *const video_values[] = {"sdl", "x11", "null"}, *const video_labels[] = {"SDL", "X11", tr(S::none)};
            row(tr(S::audio), tr(S::audio_desc));
            ch |= pills_str("audio", cur.audio, audio_values, audio_labels, 2);
            row(tr(S::video), tr(S::video_desc));
            ch |= pills_str("video", cur.video, video_values, video_labels, 3);
            row(tr(S::frame_stats), tr(S::frame_stats_desc));
            ch |= toggle("##stats", cur.stats);
            row(tr(S::trace_log), tr(S::trace_log_desc));
            ch |= toggle("##trace", cur.trace);
            end_rows();
        }
        end_card();

        begin_card("dev", tr(S::developer));
        if (begin_rows("dev-rows")) {
            row(tr(S::frame_lock), tr(S::frame_lock_desc));
            ch |= toggle("##lock", cur.frame_lock);
            row(tr(S::rate_fixes), tr(S::rate_fixes_desc));
            ch |= toggle("##ratefix", cur.rate_fixes);
            row(tr(S::spawn_scale), tr(S::spawn_scale_desc));
            ch |= slider_int("##spawn", cur.spawn_scale, 1, 8, "%d");
            row(tr(S::warp), tr(S::warp_desc));
            ch |= text_input("##warp", cur.warp, tr(S::warp_hint));
            row(tr(S::key_map), tr(S::key_map_desc));
            ch |= path_row("input", cur.input_config, tr(S::no_key_map), false, tr(S::choose_key_map),
                           cur.input_config.empty() ? -1 : app.check.input_ok);
            end_rows();
        }
        end_card();

    }
    for (const std::string &w : app.warnings) colored_line(col::warn, app.ini_path + ": " + w, fonts.hint);

    begin_card("about", tr(S::about));
    colored_line(col::text, tr(S::about_1));
    colored_line(col::dim, tr(S::about_2), fonts.hint);
    if (!app.background_note.empty()) colored_line(col::dim, app.background_note, fonts.hint);
    colored_line(col::dim, tr(S::about_3), fonts.hint);
    colored_line(col::dim, trf(S::about_version, LAUNCHER_VERSION), fonts.hint);
    end_card();
    return ch;
}

/* ---- Wizard ---- */

static const S WIZARD_STEPS[] = {S::step_program, S::step_copy, S::step_save, S::step_graphics};

static void wizard_finish(App &app) {
    app.wizard = false;
    app.file.settings.setup_done = 1;
    app.page = Page::Home;
    app.dirty = true;
}

static bool draw_wizard(App &app, Profile &p) {
    bool ch = false;
    ImGui::PushFont(fonts.title);
    ImGui::TextUnformatted(tr(S::welcome));
    ImGui::PopFont();
    colored_line(col::dim, tr(S::welcome_sub));
    ImGui::Dummy(ImVec2(0, 8));

    /* step indicator */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float x = pos.x;
    for (int i = 0; i < 4; i++) {
        bool cur = i == app.wizard_step, done = i < app.wizard_step;
        ImVec2 c(x + 14, pos.y + 14);
        dl->AddCircleFilled(c, 14, u32(cur || done ? col::accent : ImGui::GetStyle().Colors[ImGuiCol_FrameBg]));
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        ImVec2 ts = fonts.nav->CalcTextSizeA(fonts.nav->FontSize, FLT_MAX, 0, num);
        dl->AddText(fonts.nav, fonts.nav->FontSize, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), u32(cur || done ? col::on_accent : col::dim), num);
        dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(x + 36, pos.y + 4), u32(cur ? col::text : col::dim), tr(WIZARD_STEPS[i]));
        x += 36 + fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, tr(WIZARD_STEPS[i])).x + 34;
    }
    ImGui::Dummy(ImVec2(0, 36));

    bool valid = false;
    begin_card("step", nullptr);
    ImGui::PushFont(fonts.heading);
    ImGui::Text("%d. %s", app.wizard_step + 1, tr(WIZARD_STEPS[app.wizard_step]));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    switch (app.wizard_step) {
    case 0:
        colored_line(col::text, trf(S::wiz_program, GAME_EXE_NAME));
        ImGui::Dummy(ImVec2(0, 4));
        ch |= field_game_exe(app, p, false);
        valid = app.check.xbrun_ok;
        break;
    case 1: {
        colored_line(col::text, tr(S::no_game_files));
        ImGui::Dummy(ImVec2(0, 6));
        float cw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        ImGui::BeginDisabled(app.iso.job != nullptr);
        if (choice_card("iso", tr(S::wiz_iso_card), tr(S::wiz_iso_card_desc),
                        !app.iso.use_folder, ImVec2(cw, 72)))
            app.iso.use_folder = false;
        ImGui::SameLine();
        if (choice_card("folder", tr(S::wiz_folder_card), tr(S::wiz_folder_card_desc),
                        app.iso.use_folder, ImVec2(cw, 72)))
            app.iso.use_folder = true;
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 6));
        if (app.iso.use_folder) ch |= field_disc(app, p, false);
        else {
            ch |= iso_panel(app, p);
            if (app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok && !p.disc.empty() && app.iso.result.empty()) {
                ImGui::SetCursorPosX(LABEL_W);
                colored_line(col::good, trf(S::copy_in_use, p.disc.c_str()), fonts.hint);
            }
        }
        valid = app.check.disc_ok && app.check.xbe.status == XbeStatus::Ok && !app.iso.job;
        break;
    }
    case 2:
        colored_line(col::text, tr(S::wiz_save));
        ImGui::Dummy(ImVec2(0, 4));
        if (p.hdd.empty()) p.hdd = default_hdd_dir(app), ch = true, app.check_time = -1;
        label(tr(S::save_folder));
        ch |= path_field("hdd", p.hdd, tr(S::folder_for_saves), true, tr(S::choose_save));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(app.check.hdd_ok ? col::good : col::dim, "%s", app.check.hdd_ok ? tr(S::found) : tr(S::will_be_created));
        ImGui::SetCursorPosX(LABEL_W);
        if (ImGui::Button(tr(S::use_default))) p.hdd = default_hdd_dir(app), ch = true;
        valid = !p.hdd.empty();
        break;
    default:
        colored_line(col::text, tr(S::wiz_graphics));
        ImGui::Dummy(ImVec2(0, 4));
        ch |= preset_cards(app, p, 104);
        valid = detect_preset(p) != Preset::Custom;
        break;
    }
    end_card();

    /* footer */
    bool busy = app.iso.job != nullptr;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr(S::skip_setup))) wizard_finish(app);
    ImGui::EndDisabled();
    float bw = 140, right = ImGui::GetContentRegionMax().x;
    ImGui::SameLine(right - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(app.wizard_step == 0 || busy);
    if (ImGui::Button(tr(S::back), ImVec2(bw, 0))) app.wizard_step--;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    ImGui::PushStyleColor(ImGuiCol_Button, valid ? col::accent : ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, valid ? col::on_accent : col::dim);
    if (ImGui::Button(app.wizard_step == 3 ? tr(S::finish) : tr(S::next), ImVec2(bw, 0))) {
        if (app.wizard_step == 2 && !make_dirs(from_install(app, p.hdd))) {
            app.message = trf(S::save_cannot_create, p.hdd.c_str());
        } else if (app.wizard_step == 3) {
            wizard_finish(app);
        } else {
            app.message.clear();
            app.wizard_step++;
            app.check_time = -1;
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (!valid) {
        ImGui::SetCursorPosX(right - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
        hint(app.wizard_step == 3 ? tr(S::wiz_pick_preset)
             : busy                ? tr(S::wiz_wait)
             : app.wizard_step == 1 && !app.iso.use_folder ? tr(S::wiz_choose_iso)
                                   : tr(S::wiz_fix), 0);
    }
    if (!app.message.empty()) colored_line(col::bad, app.message, fonts.hint);
    return ch;
}

/* ---- Shell ---- */

/* The sidebar's line icons, drawn in a 20 x 20 box centred on c. */
static void nav_icon(ImDrawList *dl, Page page, ImVec2 c, ImU32 colour) {
    const float t = 1.8f;
    switch (page) {
    case Page::Home: {
        ImVec2 roof[] = {ImVec2(c.x - 8, c.y - 0.5f), ImVec2(c.x, c.y - 8), ImVec2(c.x + 8, c.y - 0.5f)};
        dl->AddPolyline(roof, 3, colour, 0, t);
        dl->AddRect(ImVec2(c.x - 5.5f, c.y - 2.5f), ImVec2(c.x + 5.5f, c.y + 7.5f), colour, 1.5f, 0, t);
        break;
    }
    case Page::Display:
        dl->AddRect(ImVec2(c.x - 9, c.y - 7), ImVec2(c.x + 9, c.y + 4.5f), colour, 2, 0, t);
        dl->AddLine(ImVec2(c.x - 4.5f, c.y + 8), ImVec2(c.x + 4.5f, c.y + 8), colour, t);
        break;
    case Page::Game:
        dl->AddRect(ImVec2(c.x - 10, c.y - 6), ImVec2(c.x + 10, c.y + 6), colour, 6, 0, t);
        dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x - 2, c.y), colour, t);
        dl->AddLine(ImVec2(c.x - 4.5f, c.y - 2.5f), ImVec2(c.x - 4.5f, c.y + 2.5f), colour, t);
        dl->AddCircleFilled(ImVec2(c.x + 5, c.y - 1.5f), 1.6f, colour);
        dl->AddCircleFilled(ImVec2(c.x + 3, c.y + 2), 1.6f, colour);
        break;
    case Page::Controls: /* a keyboard */
        dl->AddRect(ImVec2(c.x - 10, c.y - 6), ImVec2(c.x + 10, c.y + 6), colour, 2.5f, 0, t);
        for (int i = 0; i < 4; i++) dl->AddCircleFilled(ImVec2(c.x - 6 + i * 4, c.y - 2), 1.1f, colour);
        dl->AddLine(ImVec2(c.x - 5, c.y + 2.5f), ImVec2(c.x + 5, c.y + 2.5f), colour, t);
        break;
    default:
        for (int i = 0; i < 3; i++) {
            float y = c.y - 6 + i * 6, k = c.x + (i == 1 ? 3.5f : i == 0 ? -3 : -0.5f);
            dl->AddLine(ImVec2(c.x - 9, y), ImVec2(c.x + 9, y), colour, t);
            dl->AddCircleFilled(ImVec2(k, y), 3, u32(col::sidebar_solid));
            dl->AddCircle(ImVec2(k, y), 3, colour, 0, t);
        }
        break;
    }
}

static void sidebar(App &app, bool running) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float wh = ImGui::GetWindowHeight();
    /* the mark: an accent slash beside the name */
    dl->AddQuadFilled(ImVec2(wp.x + 26, wp.y + 56), ImVec2(wp.x + 34, wp.y + 30), ImVec2(wp.x + 41, wp.y + 30),
                      ImVec2(wp.x + 33, wp.y + 56), u32(col::accent));
    dl->AddText(fonts.title, fonts.title->FontSize, ImVec2(wp.x + 50, wp.y + 26), u32(col::text), "THE WARRIORS");
    dl->AddText(fonts.caps, fonts.caps->FontSize, ImVec2(wp.x + 51, wp.y + 52), u32(col::dim), tr(S::launcher_caps));
    /* bottom block first: a short window (a small screen at a large interface size) gets a smaller
     * PLAY, no PROFILE caption, and the page entries share what is left */
    const bool short_h = wh < 600, tiny_h = wh < 520;
    const float pad = 16, play_h = short_h ? 50 : 64, w = SIDEBAR_W - pad * 2;
    Problem pr = copy_problem(app.check);
    bool ok = pr.headline.empty();
    std::string status = app.wizard          ? trf(S::setup_step, app.wizard_step + 1)
                         : !app.message.empty() ? app.message
                         : running              ? tr(S::game_running)
                         : ok                   ? tr(S::ready)
                                                : pr.headline;
    ImVec4 sc = app.wizard ? col::dim : !app.message.empty() ? col::bad : running || ok ? col::good : col::bad;
    ImFont *f = fonts.hint;
    ImVec2 ss = f->CalcTextSizeA(f->FontSize, FLT_MAX, w - 18, status.c_str());
    float play_y = wh - pad - play_h, status_y = play_y - (short_h ? 10 : 14) - ss.y,
          profile_y = status_y - (short_h ? 10 : 16) - ImGui::GetFrameHeight();
    float block_top = tiny_h ? profile_y - 12 : profile_y - 34;
    if (!tiny_h) {
        dl->AddLine(ImVec2(wp.x + pad, wp.y + profile_y - 34), ImVec2(wp.x + SIDEBAR_W - pad, wp.y + profile_y - 34), u32(col::line));
        dl->AddText(fonts.caps, fonts.caps->FontSize, ImVec2(wp.x + pad, wp.y + profile_y - 22), u32(col::dim), tr(S::profile_caps));
    }

    const float nav_top = short_h ? 84 : 104;
    float step = (block_top - 8 - nav_top) / (int)Page::Count; /* entry + gap, at most the roomy 60 */
    step = step > 60 ? 60 : step < 30 ? 30 : step;
    const float entry_h = step > 50 ? 46 : step - 4;
    ImGui::SetCursorPos(ImVec2(0, nav_top));
    for (int i = 0; i < (int)Page::Count; i++) {
        ImGui::SetCursorPos(ImVec2(14, nav_top + i * step));
        ImVec2 pos = ImGui::GetCursorScreenPos(), size(SIDEBAR_W - 28, entry_h);
        bool sel = !app.wizard && app.page == (Page)i;
        ImGui::BeginDisabled(app.wizard);
        if (ImGui::InvisibleButton(PAGE_KEYS[i], size)) {
            app.page = (Page)i;
            app.dirty = true;
        }
        bool hovered = ImGui::IsItemHovered();
        ImVec2 end(pos.x + size.x, pos.y + size.y);
        focus_ring(dl, pos, end, 10);
        ImGui::EndDisabled();
        if (sel) dl->AddRectFilled(pos, end, u32(col::accent, 0.22f), 10);
        else if (hovered) dl->AddRectFilled(pos, end, u32(col::hover), 10);
        if (sel) dl->AddRectFilled(ImVec2(pos.x, pos.y + entry_h / 4), ImVec2(pos.x + 3, end.y - entry_h / 4), u32(col::accent), 2);
        ImVec4 tc = app.wizard ? col::dim : sel ? col::text : col::nav;
        nav_icon(dl, (Page)i, ImVec2(pos.x + 28, pos.y + size.y / 2), u32(sel ? col::accent_text : tc));
        ImVec2 ts = fonts.nav->CalcTextSizeA(fonts.nav->FontSize, FLT_MAX, 0, tr(PAGE_NAMES[i]));
        dl->AddText(fonts.nav, fonts.nav->FontSize, ImVec2(pos.x + 52, pos.y + (size.y - ts.y) / 2), u32(tc), tr(PAGE_NAMES[i]));
    }

    ImGui::SetCursorPos(ImVec2(pad, profile_y));
    ImGui::SetNextItemWidth(w);
    ImGui::BeginDisabled(running || app.wizard);
    if (ImGui::BeginCombo("##sidebar-profile", app_profile(app)->name.c_str())) {
        for (size_t i = 0; i < app.file.profiles.size(); i++) {
            ImGui::PushID((int)i);
            if (ImGui::Selectable(app.file.profiles[i].name.c_str(), app.selected == (int)i)) {
                app.selected = (int)i;
                app.check_time = -1;
                app.dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    dl->AddCircleFilled(ImVec2(wp.x + pad + 5, wp.y + status_y + f->FontSize * 0.5f + 1), 4.5f, u32(sc));
    dl->AddText(f, f->FontSize, ImVec2(wp.x + pad + 18, wp.y + status_y), u32(sc), status.c_str(), nullptr, w - 18);
    ImGui::SetCursorPos(ImVec2(pad, play_y));
    play_button(app, ImVec2(w, play_h), running);
}

std::string app_game_dir(App &app) {
    const Profile &p = *app_profile(app);
    if (!app.check.disc_ok || app.check.xbe.status != XbeStatus::Ok) return "";
    return from_install(app, p.disc);
}

void app_draw(App &app, double now) {
    if (app.check_time < 0 || now - app.check_time > CHECK_PERIOD) {
        app.check = check_copy(*app_profile(app));
        app.check_time = now;
    }
    bool running = child_poll(app.child);
    bool extracted = iso_poll(app, *app_profile(app));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const bool narrow = vp->WorkSize.x < 1000; /* logical pixels */
    SIDEBAR_W = narrow ? 204 : 240;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("warriors-launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    /* a soft shade toward the bottom, over the background art */
    ImDrawList *bg = ImGui::GetWindowDrawList();
    ImVec2 a = vp->WorkPos, b(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    bg->AddRectFilledMultiColor(ImVec2(a.x, a.y + vp->WorkSize.y * 0.45f), b, u32(col::shade, 0), u32(col::shade, 0), u32(col::shade),
                                u32(col::shade));
    ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::sidebar);
    ImGui::BeginChild("sidebar", ImVec2(SIDEBAR_W, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    sidebar(app, running);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    bg->AddLine(ImVec2(a.x + SIDEBAR_W, a.y), ImVec2(a.x + SIDEBAR_W, b.y), u32(col::line));
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, narrow ? ImVec2(20, 20) : ImVec2(32, 28));
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    Profile &p = *app_profile(app);
    bool ch = false;
    if (app.padmap.active && (app.wizard || app.page != Page::Controls)) padmap_end(app, "", false);   /* left the page */
    if (app.wizard) ch |= draw_wizard(app, p);
    else switch (app.page) {
        case Page::Home: page_home(app, running); break;
        case Page::Display: ch |= page_display(app, p); break;
        case Page::Game: ch |= page_game(app, p); break;
        case Page::Controls: ch |= page_controls(app, p); break;
        default: ch |= page_options(app, p, running); break;
        }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();

    Profile &now_p = *app_profile(app); /* Options may have changed the selected profile */
    if (ch || extracted) {
        now_p.preset = preset_key(detect_preset(now_p));
        app.dirty = true;
        app.check_time = -1;
    }
    if (app.dirty && !ImGui::IsAnyItemActive()) app_save(app);
}

} // namespace launcher
