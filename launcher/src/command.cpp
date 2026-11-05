#include "command.h"
#include "platform.h"

namespace launcher {

static bool is_windows_path(const std::string &p) {
    return p.find('\\') != std::string::npos || (p.size() >= 2 && p[1] == ':');
}

static std::string strip_separators(std::string p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\') && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    return p;
}

std::string disc_xbe_path(const std::string &disc) {
    std::string d = strip_separators(disc);
    char sep = is_windows_path(d) ? '\\' : '/';
    if (!d.empty() && d.back() != sep) d += sep;
    return d + "default.xbe";
}

/* A string option, left out when empty or equal to the runtime's default. */
static void opt_str(std::vector<std::string> &a, const char *flag, const std::string &v, const char *dflt = "") {
    if (!v.empty() && v != dflt) {
        a.push_back(flag);
        a.push_back(v);
    }
}

/* A numeric option, left out at or below its "off" value. */
static void opt_int(std::vector<std::string> &a, const char *flag, int v, int off) {
    if (v > off) {
        a.push_back(flag);
        a.push_back(std::to_string(v));
    }
}

/* A float option, left out when negative (not set). */
static void opt_float(std::vector<std::string> &a, const char *flag, float v) {
    if (v >= 0) {
        a.push_back(flag);
        a.push_back(format_float(v));
    }
}

/* The draw distance presets (Profile::draw_distance 2-4): far-plane cap and small-prop distance
 * multipliers (runtime/src/hle_drawdist.h; 1 = as shipped, so preset 1 adds nothing). */
static void draw_distance_args(std::vector<std::string> &a, int preset) {
    static const float far_scale[] = {1.5f, 2, 3}, prop_scale[] = {2, 4, 8};
    if (preset < 2 || preset > 4) return;
    opt_float(a, "--draw-distance", far_scale[preset - 2]);
    opt_float(a, "--prop-detail", prop_scale[preset - 2]);
}

static void opt_flag(std::vector<std::string> &a, const char *flag, bool on) {
    if (on) a.push_back(flag);
}

std::vector<std::string> build_argv(const Profile &p) {
    std::string disc = strip_separators(p.disc);
    std::vector<std::string> a = {p.xbrun, disc_xbe_path(disc), disc};
    opt_str(a, "--hdd", p.hdd);
    opt_str(a, "--texture-pack", p.icon_pack); /* before the texture pack: the first match wins */
    opt_str(a, "--texture-pack", p.texture_pack);
    opt_str(a, "--sound-pack", p.sound_pack);
    if (!p.resolution.empty()) opt_str(a, "--resolution", p.resolution);
    else opt_int(a, "--render-scale", p.render_scale, 1);
    opt_flag(a, "--widescreen", p.widescreen);
    opt_flag(a, "--no-movie-hd", !p.movie_hd);
    opt_flag(a, "--fullscreen", p.fullscreen);
    opt_int(a, "--msaa", p.msaa, 1);
    opt_int(a, "--aniso", p.aniso, 1);
    opt_str(a, "--present", p.present, "auto");
    opt_str(a, "--language", p.language, "en");
    opt_int(a, "--spawn-scale", p.spawn_scale, 1);
    opt_flag(a, "--no-rate-fixes", !p.rate_fixes);
    opt_flag(a, "--no-frame-lock", !p.frame_lock);
    opt_str(a, "--input-config", p.input_config);
    if (p.button_icons == "kbm" || p.button_icons == "auto") opt_str(a, "--button-icons", p.button_icons);
    opt_flag(a, "--no-rumble", !p.rumble);
    opt_str(a, "--warp", p.warp);
    opt_str(a, "--audio", p.audio, "sdl");
    opt_str(a, "--video", p.video, "sdl");
    opt_str(a, "--renderer", p.renderer, "vulkan");
    opt_flag(a, "--no-render-targets", !p.render_targets);
    opt_flag(a, "--stats", p.stats);
    opt_flag(a, "--trace", p.trace);
    opt_flag(a, "--sim60", p.sim60);
    bool hd = p.hud_style == "hd" || p.hud_style == "hd-glass";
    opt_flag(a, "--hud-hd", p.hud_style == "hd");
    opt_flag(a, "--hud-hd-glass", p.hud_style == "hd-glass");
    opt_flag(a, "--hud-glass", p.hud_style == "glass");
    if (hd) { /* after the style flag: they override its values */
        opt_float(a, "--hud-hd-track-opacity", p.hud_track_opacity);
        opt_float(a, "--hud-hd-fill-opacity", p.hud_fill_opacity);
        opt_float(a, "--hud-hd-frost", p.hud_frost);
        opt_float(a, "--hud-hd-height", p.hud_height);
    }
    opt_int(a, "--supersample", p.supersample, 1);
    opt_int(a, "--shadow-scale", p.shadow_scale, 1);
    opt_flag(a, "--shadow-soft", p.shadow_soft);
    opt_flag(a, "--shadow-range", p.shadow_range);
    opt_int(a, "--frames-in-flight", p.frames_in_flight, 0);
    if (p.sharpen > 0) opt_int(a, "--sharpen", p.sharpen > 100 ? 100 : p.sharpen, 0);
    opt_flag(a, "--bloom", p.bloom);
    if (p.bloom) opt_float(a, "--bloom-strength", p.bloom_strength);
    opt_flag(a, "--rain-hd", p.rain_hd);
    if (p.rain_hd) opt_flag(a, "--rain-hd-puddles", p.rain_puddles);
    opt_flag(a, "--ssao", p.ssao);
    if (p.ssao) opt_float(a, "--ssao-strength", p.ssao_strength);
    opt_flag(a, "--contact-shadows", p.contact_shadows);
    opt_flag(a, "--lamp-lights", p.lamp_lights);
    if (p.lamp_lights) opt_float(a, "--lamp-lights-strength", p.lamp_lights_strength);
    opt_flag(a, "--soft-particles", p.soft_particles);
    opt_flag(a, "--filmic", p.filmic);
    opt_flag(a, "--sky-hd", p.sky_hd);
    opt_flag(a, "--no-sharp-fonts", !p.sharp_fonts);
    opt_flag(a, "--sharp-faces", p.sharp_faces);
    draw_distance_args(a, p.draw_distance);
    opt_int(a, "--fx-scale", p.fx_scale, 1);
    if (p.spray_time != SPRAY_TIME_DEFAULT) opt_int(a, "--spray-time", p.spray_time, 0);
    opt_flag(a, "--no-text-fixes", !p.text_fixes);
    for (const std::string &x : split_args(p.extra_args)) a.push_back(x);
    return a;
}

const char *const CHARACTERS_HD_DIR = "characters-hd";

int count_wmesh(const std::string &dir) {
    std::vector<DirEntry> list;
    if (!list_dir(dir, list)) return 0;
    int n = 0;
    for (const DirEntry &e : list) {
        const std::string &s = e.name;
        if (!e.dir && s.size() > 6 && s.compare(s.size() - 6, 6, ".wmesh") == 0) n++;
    }
    return n;
}

void add_auto_mesh_pack(std::vector<std::string> &argv, const std::string &dir, int models) {
    if (models <= 0) return;
    for (const std::string &x : argv)
        if (x == "--mesh-pack") return;
    argv.push_back("--mesh-pack");
    argv.push_back(dir);
}

std::vector<std::string> pad_slot_args(const std::vector<ControllerPref> &controllers) {
    std::vector<std::string> a;
    for (int n = 1; n <= 4; n++)
        for (const ControllerPref &c : controllers)
            if (c.player == n && !c.key.empty()) {
                a.push_back("--pad-slot");
                a.push_back(std::to_string(n) + "=" + c.key);
                break;
            }
    return a;
}

std::vector<std::string> build_argv(const Profile &p, const std::vector<ControllerPref> &controllers) {
    Profile q = p;
    q.extra_args.clear();
    std::vector<std::string> a = build_argv(q);
    for (const std::string &x : pad_slot_args(controllers)) a.push_back(x);
    for (const std::string &x : split_args(p.extra_args)) a.push_back(x);
    return a;
}

std::vector<std::string> split_args(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_arg = false;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            if (c == quote) quote = 0;
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_arg = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (in_arg) out.push_back(cur);
            cur.clear();
            in_arg = false;
        } else {
            cur += c;
            in_arg = true;
        }
    }
    if (in_arg) out.push_back(cur);
    return out;
}

std::string quote_posix(const std::string &arg) {
    if (!arg.empty() && arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                                              "-_./=:,+@%") == std::string::npos)
        return arg;
    std::string q = "'";
    for (char c : arg) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

/* CommandLineToArgvW: backslashes are literal unless they precede a quote, where 2n (+1)
 * backslashes stand for n backslashes (and a literal quote). */
std::string quote_windows(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string q = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            backslashes++;
            continue;
        }
        q.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        q += c;
    }
    q.append(backslashes * 2, '\\');
    return q + "\"";
}

std::string windows_command_line(const std::vector<std::string> &argv) {
    std::string s;
    for (const std::string &a : argv) s += (s.empty() ? "" : " ") + quote_windows(a);
    return s;
}

std::string display_command(const std::vector<std::string> &argv, bool windows) {
    if (!windows) {
        std::string s;
        for (const std::string &a : argv) s += (s.empty() ? "" : " ") + quote_posix(a);
        return s;
    }
    std::string s = windows_command_line(argv);
    if (!argv.empty() && !s.empty() && s[0] == '"') s = "& " + s;
    return s;
}

} // namespace launcher
