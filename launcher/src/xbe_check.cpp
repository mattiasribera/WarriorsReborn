#include "xbe_check.h"
#include "command.h"
#include "platform.h"
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

/* xbrun runs in its own folder, so the other relative paths are relative to that folder. */
static std::string from_xbrun(const Profile &p, const std::string &path) {
    std::string dir = path_dir(xbrun_path(p));
    return is_absolute(path) || dir.empty() ? path : path_join(dir, path);
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
    c.icon_pack_ok = p.icon_pack.empty() || is_dir(from_xbrun(p, p.icon_pack));
    c.input_ok = p.input_config.empty() || is_file(from_xbrun(p, p.input_config));
    if (c.disc_ok) c.xbe = check_xbe_file(from_xbrun(p, disc_xbe_path(p.disc)));
    return c;
}

} // namespace launcher
