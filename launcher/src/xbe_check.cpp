#include "xbe_check.h"
#include "command.h"
#include "platform.h"
#include <cstdlib>
#include <cstring>

namespace launcher {

#define XBE_BASE_ADDR 0x104
#define XBE_CERT_ADDR 0x118
#define XBE_CERT_TITLE 0x0C
#define XBE_TITLE_CHARS 40
#define XBE_HEADER_READ (64 * 1024) /* the certificate sits in the first page of the headers */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* UTF-16LE -> UTF-8, up to the first NUL. */
static std::string utf16le_to_utf8(const uint8_t *p, size_t chars) {
    std::string s;
    for (size_t i = 0; i < chars; i++) {
        uint32_t c = p[i * 2] | p[i * 2 + 1] << 8;
        if (!c) break;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        else s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    }
    return s;
}

XbeCheck check_xbe_bytes(const uint8_t *data, size_t size) {
    XbeCheck r;
    r.status = XbeStatus::NotXbe;
    if (size < XBE_CERT_ADDR + 4 || memcmp(data, "XBEH", 4)) return r;
    uint32_t base = rd32(data + XBE_BASE_ADDR), cert = rd32(data + XBE_CERT_ADDR);
    uint64_t title = (uint64_t)cert - base + XBE_CERT_TITLE;
    if (cert < base || title + XBE_TITLE_CHARS * 2 > size) return r;
    r.title = utf16le_to_utf8(data + title, XBE_TITLE_CHARS);
    r.status = r.title == LAUNCHER_GAME_TITLE ? XbeStatus::Ok : XbeStatus::WrongTitle;
    return r;
}

XbeCheck check_xbe_file(const std::string &path) {
    XbeCheck r;
    if (!is_file(path)) return r;
    std::string head;
    if (!read_file(path, head, XBE_HEADER_READ)) {
        r.status = XbeStatus::Unreadable;
        return r;
    }
    return check_xbe_bytes((const uint8_t *)head.data(), head.size());
}

const char *xbe_status_text(XbeStatus s) {
    switch (s) {
    case XbeStatus::Ok: return "The Warriors";
    case XbeStatus::Missing: return "default.xbe not found";
    case XbeStatus::Unreadable: return "default.xbe cannot be read";
    case XbeStatus::NotXbe: return "default.xbe is not an Xbox executable";
    case XbeStatus::WrongTitle: return "default.xbe is another title";
    }
    return "?";
}

static bool is_absolute(const std::string &path) {
    return !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() >= 2 && path[1] == ':'));
}

std::string xbrun_path(const Profile &p) {
    return p.xbrun.empty() || is_absolute(p.xbrun) ? p.xbrun : path_join(exe_dir(), p.xbrun);
}

std::string find_game_exe(const std::vector<std::string> &dirs, bool windows) {
    static const char *const win_names[] = {"TheWarriors.exe", "xbrun.exe"};
    static const char *const posix_names[] = {"xbrun"};
    const char *const *names = windows ? win_names : posix_names;
    size_t count = windows ? 2 : 1;
    for (size_t i = 0; i < count; i++)
        for (const std::string &d : dirs)
            if (is_file(path_join(d, names[i]))) return path_join(d, names[i]);
    return "";
}

std::string data_dir_env() {
    const char *d = std::getenv("WARRIORS_DATA_DIR");
    return d ? d : "";
}

std::string game_dir(const Profile &p) {
    std::string data = data_dir_env();
    return data.empty() ? path_dir(xbrun_path(p)) : data;
}

bool adopt_bundled_xbrun(Profile &p, const std::string &launcher_dir, bool windows) {
    if (data_dir_env().empty() || (!p.xbrun.empty() && is_file(xbrun_path(p)))) return false;
    std::string bundled = find_game_exe({launcher_dir}, windows);
    if (bundled.empty()) return false;
    p.xbrun = bundled.substr(bundled.find_last_of("/\\") + 1);
    return true;
}

std::string first_run_game_exe(const std::string &ini_dir, const std::string &launcher_dir, bool windows) {
    std::string found = find_game_exe({ini_dir, path_dir(ini_dir)}, windows);
    if (!found.empty() || data_dir_env().empty()) return found;
    found = find_game_exe({launcher_dir}, windows);
    return found.empty() ? "" : found.substr(found.find_last_of("/\\") + 1);
}

/* xbrun runs in its own folder (the data folder in the AppImage), so the other relative paths are
 * relative to that folder. */
static std::string from_xbrun(const Profile &p, const std::string &path) {
    std::string dir = game_dir(p);
    return is_absolute(path) || dir.empty() ? path : path_join(dir, path);
}

static bool holds_glyph_packs(const Profile &p, const std::string &dir) {
    for (const char *style : {"ps-black", "ps-color", "ps-white"})
        if (is_dir(path_join(from_xbrun(p, dir), style))) return true;
    return false;
}

std::string find_glyph_packs(const Profile &p) {
    std::string d = p.glyph_packs;
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\') && !(d.size() == 3 && d[1] == ':')) d.pop_back();
    if (d.empty()) return "";
    if (holds_glyph_packs(p, d)) return d;
    size_t cut = d.find_last_of("/\\");
    std::string parent = cut == std::string::npos ? "" : path_dir(d);
    if (is_ps_icons(d.substr(cut == std::string::npos ? 0 : cut + 1)) && !parent.empty() && holds_glyph_packs(p, parent)) return parent;
    std::string sub = path_join(d, "glyph-packs");
    return holds_glyph_packs(p, sub) ? sub : "";
}

std::string auto_glyph_pack(const Profile &p, bool playstation_pad) {
    if (p.button_icons != "auto" || !playstation_pad) return "";
    Profile q = p;
    q.glyph_packs = find_glyph_packs(p);
    q.button_icons = "ps-black";
    std::string dir = q.glyph_packs.empty() ? "" : glyph_pack_dir(q);
    return !dir.empty() && is_dir(from_xbrun(p, dir)) ? dir : "";
}

void add_auto_ps_icons(std::vector<std::string> &argv, const Profile &p, bool playstation_pad) {
    if (p.button_icons != "auto" || !playstation_pad) return;
    std::string pack = auto_glyph_pack(p, true);
    if (!pack.empty()) {
        add_glyph_pack(argv, pack);
        return;
    }
    argv.push_back("--pad-icons");
    argv.push_back("ps-black");
}

Profile resolve_button_icons(const Profile &p) {
    Profile q = p;
    std::string found = find_glyph_packs(p);
    if (!found.empty()) q.glyph_packs = found;
    if (is_ps_icons(q.button_icons) && (found.empty() || !is_dir(from_xbrun(p, glyph_pack_dir(q))))) q.glyph_packs.clear();
    return q;
}

std::string mods_path(const Profile &p) {
    if (p.xbrun.empty() && !is_absolute(p.mods_dir)) return "";
    return from_xbrun(p, p.mods_dir.empty() ? "mods" : p.mods_dir);
}

std::string keymap_path(const Profile &p) {
    if (p.xbrun.empty() && !is_absolute(p.input_config)) return "";
    return from_xbrun(p, p.input_config.empty() ? "warriors-input.cfg" : p.input_config);
}

std::string pad_db_path(const Profile &p) {
    return p.xbrun.empty() ? "" : path_join(path_dir(xbrun_path(p)), "gamecontrollerdb.txt");
}

CopyCheck check_copy(const Profile &p) {
    CopyCheck c;
    c.xbrun_ok = !p.xbrun.empty() && is_file(xbrun_path(p));
    c.disc_ok = !p.disc.empty() && is_dir(from_xbrun(p, p.disc));
    c.hdd_ok = p.hdd.empty() || is_dir(from_xbrun(p, p.hdd));
    c.texpack_ok = p.texture_pack.empty() || is_dir(from_xbrun(p, p.texture_pack));
    c.sndpack_ok = p.sound_pack.empty() || is_dir(from_xbrun(p, p.sound_pack));
    Profile resolved = p;   /* a ps-* folder or a folder holding glyph-packs picked: the one holding the packs */
    std::string found = find_glyph_packs(p);
    if (!found.empty()) resolved.glyph_packs = found;
    std::string glyphs = glyph_pack_dir(resolved);
    c.glyphs_ok = glyphs.empty() || is_dir(from_xbrun(p, glyphs));   /* no folder: the runtime's own icons */
    c.glyph_folder_ok = p.glyph_packs.empty() || !found.empty();
    c.icon_pack_ok = p.icon_pack.empty() || is_dir(from_xbrun(p, p.icon_pack));
    c.input_ok = p.input_config.empty() || is_file(from_xbrun(p, p.input_config));
    if (c.disc_ok) c.xbe = check_xbe_file(from_xbrun(p, disc_xbe_path(p.disc)));
    return c;
}

} // namespace launcher
