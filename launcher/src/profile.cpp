#include "profile.h"
#include "command.h"
#include "platform.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace launcher {

/* Key tables: the file order of the known keys, by type. */
struct StrKey { const char *key; std::string Profile::*field; };
struct IntKey { const char *key; int Profile::*field; };
struct BoolKey { const char *key; bool Profile::*field; };
struct FloatKey { const char *key; float Profile::*field; };

static const StrKey str_keys[] = {
    {"xbrun", &Profile::xbrun},          {"disc", &Profile::disc},
    {"hdd", &Profile::hdd},              {"resolution", &Profile::resolution},
    {"present", &Profile::present},      {"texture_pack", &Profile::texture_pack},
    {"sound_pack", &Profile::sound_pack},
    {"button_icons", &Profile::button_icons}, {"icon_pack", &Profile::icon_pack},
    {"neon_masks", &Profile::neon_masks},
    {"language", &Profile::language},    {"input_config", &Profile::input_config},
    {"warp", &Profile::warp},
    {"audio", &Profile::audio},          {"video", &Profile::video},
    {"renderer", &Profile::renderer},    {"hud_style", &Profile::hud_style},
    {"gpu_vertex_programs", &Profile::gpu_vertex_programs},
    {"preset", &Profile::preset},        {"extra_args", &Profile::extra_args},
    {"fps_overlay_items", &Profile::fps_overlay_items}, {"fps_overlay_corner", &Profile::fps_overlay_corner},
};
static const IntKey int_keys[] = {
    {"render_scale", &Profile::render_scale}, {"msaa", &Profile::msaa},
    {"aniso", &Profile::aniso},               {"spawn_scale", &Profile::spawn_scale},
    {"supersample", &Profile::supersample},   {"shadow_scale", &Profile::shadow_scale},
    {"fx_scale", &Profile::fx_scale},
    {"frames_in_flight", &Profile::frames_in_flight}, {"spray_time", &Profile::spray_time},
    {"sharpen", &Profile::sharpen},           {"draw_distance", &Profile::draw_distance},
};
static const BoolKey bool_keys[] = {
    {"widescreen", &Profile::widescreen},   {"fullscreen", &Profile::fullscreen},
    {"rate_fixes", &Profile::rate_fixes},   {"frame_lock", &Profile::frame_lock},
    {"render_targets", &Profile::render_targets}, {"stats", &Profile::stats},
    {"trace", &Profile::trace},             {"shadow_soft", &Profile::shadow_soft},
    {"sim60", &Profile::sim60},             {"text_fixes", &Profile::text_fixes},
    {"rumble", &Profile::rumble},           {"bloom", &Profile::bloom},
    {"shadow_range", &Profile::shadow_range}, {"rain_hd", &Profile::rain_hd},
    {"ssao", &Profile::ssao},               {"movie_hd", &Profile::movie_hd},
    {"filmic", &Profile::filmic},
    {"sky_hd", &Profile::sky_hd},
    {"sharp_fonts", &Profile::sharp_fonts},
    {"sharp_faces", &Profile::sharp_faces},
    {"sharp_faces_over_pack", &Profile::sharp_faces_over_pack},
    {"skip_intro", &Profile::skip_intro},
    {"rain_puddles", &Profile::rain_puddles},
    {"contact_shadows", &Profile::contact_shadows},
    {"lamp_lights", &Profile::lamp_lights},
    {"neon_glow", &Profile::neon_glow},
    {"char_lighting", &Profile::char_lighting},
    {"world_lighting", &Profile::world_lighting},
    {"fire_light", &Profile::fire_light},
    {"soft_particles_opt_in", &Profile::soft_particles},
    {"fps_overlay", &Profile::fps_overlay},
};
static const FloatKey float_keys[] = {
    {"hud_track_opacity", &Profile::hud_track_opacity}, {"hud_fill_opacity", &Profile::hud_fill_opacity},
    {"hud_frost", &Profile::hud_frost},                 {"hud_height", &Profile::hud_height},
    {"bloom_strength", &Profile::bloom_strength},     {"ssao_strength", &Profile::ssao_strength},
    {"lamp_lights_strength", &Profile::lamp_lights_strength},
    {"neon_glow_strength", &Profile::neon_glow_strength},
};

static const char PROFILE_PREFIX[] = "profile ";

static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

static bool parse_bool(const std::string &v, bool &out) {
    if (v == "true" || v == "yes" || v == "1" || v == "on") return out = true, true;
    if (v == "false" || v == "no" || v == "0" || v == "off") return out = false, true;
    return false;
}

static bool parse_int(const std::string &v, int &out) {
    char *end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || n < -1000000 || n > 1000000) return false;
    out = (int)n;
    return true;
}

static bool parse_float(const std::string &v, float &out) {
    char *end = nullptr;
    double d = strtod(v.c_str(), &end);
    if (v.empty() || *end || d < -1000000 || d > 1000000) return false;
    out = (float)d;
    return true;
}

std::string format_float(float v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.3g", v);
    return buf;
}

/* Keys older launchers wrote that no longer set anything: read without a warning and not written
 * back. soft_particles: written before v0.1.4, when the Quality preset turned soft particles on and
 * they faded graffiti tags lying on walls into the wall. Soft particles are now opt-in under
 * soft_particles_opt_in, so a profile saved with the old key loads with them off and keeps its preset. */
static const char *const retired_keys[] = {"soft_particles"};

/* Sets one key of p; returns false for an unknown key, sets *bad for a bad value. */
static bool set_key(Profile &p, const std::string &key, const std::string &value, bool *bad) {
    if (key == "char_lighting") p.char_lighting_saved = true;
    if (key == "world_lighting") p.world_lighting_saved = true;
    if (key == "fire_light") p.fire_light_saved = true;
    if (key == "skip_intro") p.skip_intro_saved = true;
    for (const char *k : retired_keys)
        if (key == k) return true;
    for (const StrKey &k : str_keys)
        if (key == k.key) return p.*k.field = value, true;
    for (const IntKey &k : int_keys)
        if (key == k.key) return *bad = !parse_int(value, p.*k.field), true;
    for (const BoolKey &k : bool_keys)
        if (key == k.key) return *bad = !parse_bool(value, p.*k.field), true;
    for (const FloatKey &k : float_keys)
        if (key == k.key) return *bad = !parse_float(value, p.*k.field), true;
    return false;
}

static void set_launcher_key(ProfileFile &out, const std::string &key, const std::string &value, bool *bad) {
    LauncherSettings &s = out.settings;
    if (key == "selected") out.selected = value;
    else if (key == "setup_done") *bad = !parse_int(value, s.setup_done);
    else if (key == "close_on_start") *bad = !parse_bool(value, s.close_on_start);
    else if (key == "show_advanced") *bad = !parse_bool(value, s.show_advanced);
    else if (key == "last_page") s.last_page = value;
    else if (key == "last_log") s.last_log = value;
    else if (key == "ui_language") s.ui_language = value;
    else if (key == "ui_scale") s.ui_scale = value;
    else if (key == "gpu_id") s.gpu.id = value;
    else if (key == "gpu_name") s.gpu.name = value;
    else if (key == "gpu_driver") s.gpu.driver = value;
    else if (key == "gpu_tier") s.gpu.tier = value;
    else if (key == "gpu_recommended") s.gpu.recommended = value;
    else if (key == "gpu_vram_mb") *bad = !parse_int(value, s.gpu.vram_mb);
    else if (key == "gpu_budget_mb") *bad = !parse_int(value, s.gpu.budget_mb);
    else if (key == "gpu_bc") *bad = !parse_int(value, s.gpu.bc);
    else if (key == "gpu_table") *bad = !parse_int(value, s.gpu.table);
    else if (key == "backup_saves") *bad = !parse_bool(value, s.backup_saves);
    else if (key == "perf_overlay_asked") *bad = !parse_bool(value, s.perf_overlay_asked);
    else if (key == "backup_count") {
        int n = 0;
        *bad = !parse_int(value, n) || n < 1 || n > 20;
        if (!*bad) s.backup_count = n;
    }
    else s.unknown.push_back({key, value});
}

static const char CONTROLLER_PREFIX[] = "controller ";

std::string controller_key(const std::string &guid, int nth) {
    std::string k;
    for (char c : guid) k += (char)tolower((unsigned char)c);
    return nth > 1 ? k + "#" + std::to_string(nth) : k;
}
const ControllerPref *find_controller(const ProfileFile &file, const std::string &key) {
    for (const ControllerPref &c : file.controllers)
        if (c.key == key) return &c;
    return nullptr;
}
ControllerPref *find_controller(ProfileFile &file, const std::string &key) {
    return const_cast<ControllerPref *>(find_controller(static_cast<const ProfileFile &>(file), key));
}
ControllerPref &controller_pref(ProfileFile &file, const std::string &key) {
    if (ControllerPref *c = find_controller(file, key)) return *c;
    file.controllers.push_back(ControllerPref());
    file.controllers.back().key = key;
    return file.controllers.back();
}
void set_controller_player(ProfileFile &file, const std::string &key, int player) {
    if (player < 0 || player > 4) player = 0;
    if (player)
        for (ControllerPref &c : file.controllers)
            if (c.player == player && c.key != key) c.player = 0;
    controller_pref(file, key).player = player;
}

static void set_controller_key(ControllerPref &c, const std::string &key, const std::string &value, bool *bad, bool *unknown) {
    if (key == "name") c.name = value;
    else if (key == "player") {
        int n = 0;
        if (value == "auto") c.player = 0;
        else if (parse_int(value, n) && n >= 1 && n <= 4) c.player = n;
        else *bad = true;
    } else {
        c.unknown.push_back({key, value});
        *unknown = true;
    }
}

bool parse_ini(const std::string &text, ProfileFile &out, std::vector<std::string> *warnings) {
    out = ProfileFile();
    enum { NONE, LAUNCHER, PROFILE, CONTROLLER, OTHER } section = NONE;
    std::string controller;   /* the key of the [controller KEY] section being read */
    bool ok = true;
    size_t pos = 0;
    for (int line_no = 1; pos < text.size(); line_no++) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        auto warn = [&](const std::string &msg) {
            if (warnings) warnings->push_back("line " + std::to_string(line_no) + ": " + msg);
        };
        if (section == OTHER && !(line.size() > 1 && line[0] == '[')) {
            out.other_sections.push_back(raw);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            std::string name = trim(line.substr(1, line.size() - 2));
            if (name == "launcher") section = LAUNCHER;
            else if (name.compare(0, sizeof PROFILE_PREFIX - 1, PROFILE_PREFIX) == 0) {
                section = PROFILE;
                out.profiles.push_back(Profile());
                out.profiles.back().name = trim(name.substr(sizeof PROFILE_PREFIX - 1));
            } else if (name.compare(0, sizeof CONTROLLER_PREFIX - 1, CONTROLLER_PREFIX) == 0) {
                section = CONTROLLER;   /* a repeated section adds to the first */
                controller = controller_pref(out, trim(name.substr(sizeof CONTROLLER_PREFIX - 1))).key;
            } else {
                section = OTHER;
                out.other_sections.push_back(raw);
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || section == NONE) {
            warn("expected [section] or key = value");
            ok = false;
            continue;
        }
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == LAUNCHER) {
            bool bad = false;
            size_t unknown = out.settings.unknown.size();
            set_launcher_key(out, key, value, &bad);
            if (out.settings.unknown.size() != unknown) warn("unknown launcher key '" + key + "' (kept)");
            if (bad) warn("bad value '" + value + "' for '" + key + "' (default kept)");
            continue;
        }
        if (section == CONTROLLER) {
            bool bad = false, unknown = false;
            set_controller_key(*find_controller(out, controller), key, value, &bad, &unknown);
            if (unknown) warn("unknown key '" + key + "' for controller " + controller + " (kept)");
            if (bad) warn("bad value '" + value + "' for '" + key + "' (default kept)");
            continue;
        }
        Profile &p = out.profiles.back();
        bool bad = false;
        if (!set_key(p, key, value, &bad)) {
            p.unknown.push_back({key, value});
            warn("unknown key '" + key + "' in profile '" + p.name + "' (kept)");
        } else if (bad) {
            warn("bad value '" + value + "' for '" + key + "' (default kept)");
        }
    }
    for (Profile &p : out.profiles) {
        migrate_extra_args(p);
        /* char_lighting is newer than the file: a profile saved as Balanced or Quality takes the
         * preset's value (on), so it keeps its preset. */
        const Preset saved = preset_from_key(p.preset);
        if (!p.char_lighting_saved && (saved == Preset::Balanced || saved == Preset::Gaming)) p.char_lighting = true;
        /* world_lighting likewise: a profile saved as Quality takes it on. */
        if (!p.world_lighting_saved && saved == Preset::Gaming) p.world_lighting = true;
        /* fire_light likewise. */
        if (!p.fire_light_saved && saved == Preset::Gaming) p.fire_light = true;
        /* skip_intro is newer than the file: on, as every preset sets it (not part of the preset match). */
        if (!p.skip_intro_saved) p.skip_intro = true;
        p.preset = preset_key(detect_preset(p));
    }
    return ok;
}

std::string write_ini(const ProfileFile &file) {
    std::string s = "# warriors-launcher profiles (see launcher/README.md)\n[launcher]\nselected = " + file.selected + "\n";
    const LauncherSettings &st = file.settings;
    if (st.setup_done >= 0) s += "setup_done = " + std::to_string(st.setup_done) + "\n";
    s += std::string("close_on_start = ") + (st.close_on_start ? "true" : "false") + "\n";
    s += std::string("show_advanced = ") + (st.show_advanced ? "true" : "false") + "\n";
    s += "last_page = " + st.last_page + "\nlast_log = " + st.last_log + "\n";
    if (!st.ui_language.empty()) s += "ui_language = " + st.ui_language + "\n";
    if (!st.ui_scale.empty() && st.ui_scale != "auto") s += "ui_scale = " + st.ui_scale + "\n";
    if (!st.backup_saves) s += "backup_saves = false\n";
    if (st.backup_count != 5) s += "backup_count = " + std::to_string(st.backup_count) + "\n";
    if (st.perf_overlay_asked) s += "perf_overlay_asked = true\n";
    if (const GpuDetection &g = st.gpu; !g.id.empty()) {
        s += "gpu_id = " + g.id + "\ngpu_name = " + g.name + "\ngpu_driver = " + g.driver + "\ngpu_tier = " + g.tier +
             "\ngpu_recommended = " + g.recommended + "\ngpu_vram_mb = " + std::to_string(g.vram_mb) +
             "\ngpu_budget_mb = " + std::to_string(g.budget_mb) + "\ngpu_bc = " + std::to_string(g.bc) +
             "\ngpu_table = " + std::to_string(g.table) + "\n";
    }
    for (const auto &kv : st.unknown) s += kv.first + " = " + kv.second + "\n";
    for (const Profile &p : file.profiles) {
        s += "\n[profile " + p.name + "]\n";
        for (const StrKey &k : str_keys)
            s += std::string(k.key) + " = " + (k.field == &Profile::preset ? preset_key(detect_preset(p)) : p.*k.field) + "\n";
        for (const IntKey &k : int_keys) s += std::string(k.key) + " = " + std::to_string(p.*k.field) + "\n";
        for (const BoolKey &k : bool_keys) s += std::string(k.key) + " = " + (p.*k.field ? "true" : "false") + "\n";
        for (const FloatKey &k : float_keys) s += std::string(k.key) + " = " + format_float(p.*k.field) + "\n";
        for (const auto &kv : p.unknown) s += kv.first + " = " + kv.second + "\n";
    }
    for (const ControllerPref &c : file.controllers) {
        if (c.key.empty() || (c.name.empty() && !c.player && c.unknown.empty())) continue;
        s += "\n[controller " + c.key + "]\n";
        if (!c.name.empty()) s += "name = " + c.name + "\n";
        if (c.player) s += "player = " + std::to_string(c.player) + "\n";
        for (const auto &kv : c.unknown) s += kv.first + " = " + kv.second + "\n";
    }
    if (!file.other_sections.empty()) s += "\n";
    for (const std::string &line : file.other_sections) s += line + "\n";
    return s;
}

bool load_ini(const std::string &path, ProfileFile &out, std::vector<std::string> *warnings, std::string *err) {
    std::string text;
    if (!read_file(path, text, 1 << 20)) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    parse_ini(text, out, warnings); /* bad lines are reported as warnings, the rest is used */
    return true;
}

bool save_ini(const std::string &path, const ProfileFile &file, std::string *err) {
    if (write_file(path, write_ini(file))) return true;
    if (err) *err = "cannot write " + path;
    return false;
}

/* ---- Presets ---- */

static const char TEXPACK_DIR[] = "texpack-v3";
static const char NEON_MASKS_DIR[] = "neon-masks";

void apply_preset(Profile &p, Preset preset, const std::string &install_dir) {
    if (preset == Preset::Custom) return;
    bool gaming = preset == Preset::Gaming, balanced = preset == Preset::Balanced;
    p.resolution = "1920x1080";
    p.widescreen = p.fullscreen = true;
    p.msaa = gaming ? 4 : balanced ? 2 : 0;
    p.supersample = 1; /* off in every preset: it costs 4x the pixels and slows most PCs */
    p.aniso = 4;
    p.shadow_scale = gaming ? 2 : 1;
    p.shadow_soft = gaming || balanced;
    p.shadow_range = gaming;
    p.frames_in_flight = preset == Preset::Laptop ? 2 : 0;
    p.sim60 = true;
    p.hud_style = "hd-glass";
    p.hud_track_opacity = p.hud_fill_opacity = p.hud_frost = p.hud_height = -1;
    p.bloom = gaming || balanced;
    p.bloom_strength = -1;
    p.rain_hd = gaming || balanced;
    p.rain_puddles = gaming || balanced; /* about 0.05 ms at 1080p: cheap enough for Balanced */
    p.ssao = gaming;
    p.ssao_strength = -1;
    p.contact_shadows = gaming; /* 0.3-0.7 ms at 1080p: Quality only */
    p.lamp_lights = gaming;
    p.lamp_lights_strength = -1;
    p.neon_glow = gaming || balanced; /* one texture sample on the masked signs' draws only: Balanced too */
    p.neon_glow_strength = -1;
    p.char_lighting = gaming || balanced; /* well under 0.3 ms at 1080p (characters only): Balanced too */
    p.world_lighting = gaming;            /* the whole lit world: Quality only */
    p.fire_light = gaming;                /* up to 8 more lights in the lit draws: Quality only */
    p.soft_particles = false; /* opt-in only (Display): no preset turns it on */
    p.sharp_fonts = true;     /* cheap (one upscale on a worker thread, ~64 MB of video memory): every preset */
    /* Draw distance and effect pools: Balanced takes the mildest step up, Performance goes back
     * to the shipped values, Quality leaves them as they are (older Quality profiles keep their
     * own choice and stay Quality). */
    if (balanced) p.draw_distance = p.fx_scale = 2;
    else if (preset == Preset::Laptop) p.draw_distance = p.fx_scale = 1;
    p.sky_hd = gaming || balanced;
    p.sharp_faces = gaming || balanced; /* cheap: 1.3 MB per 256 skin (5.3 MB per 512), filtered off the frame thread */
    p.sharp_faces_over_pack = false;    /* the user's choice over their pack: no preset turns it on (not in the match) */
    p.fps_overlay = false;              /* Show FPS: a display aid, no preset turns it on (not in the match) */
    p.filmic = gaming || balanced; /* the cinematic look is cheap: on in Balanced and Quality */
    p.skip_intro = true; /* every preset; a Game option, so detect_preset leaves it out */
    std::string texpack = path_join(install_dir, TEXPACK_DIR);
    if (p.texture_pack.empty() && is_dir(texpack)) p.texture_pack = texpack;
    std::string masks = path_join(install_dir, NEON_MASKS_DIR);
    if (p.neon_masks.empty() && !install_dir.empty() && is_dir(masks)) p.neon_masks = masks;
}

bool sharp_faces_over_pack_offered(const Profile &p) { return p.sharp_faces && !p.texture_pack.empty(); }

Preset detect_preset(const Profile &p) {
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile want = p;
        want.texture_pack = "";
        apply_preset(want, preset, "");
        if (p.resolution == want.resolution && p.widescreen == want.widescreen && p.fullscreen == want.fullscreen &&
            p.msaa == want.msaa && p.supersample == want.supersample && p.aniso == want.aniso &&
            p.shadow_scale == want.shadow_scale && p.shadow_soft == want.shadow_soft && p.shadow_range == want.shadow_range &&
            p.frames_in_flight == want.frames_in_flight && p.sim60 == want.sim60 && p.hud_style == want.hud_style &&
            p.hud_track_opacity < 0 && p.hud_fill_opacity < 0 && p.hud_frost < 0 && p.hud_height < 0 && p.bloom == want.bloom &&
            p.bloom_strength < 0 && p.rain_hd == want.rain_hd && p.ssao == want.ssao && p.ssao_strength < 0 &&
            p.lamp_lights == want.lamp_lights && p.lamp_lights_strength < 0 && p.char_lighting == want.char_lighting &&
            p.world_lighting == want.world_lighting && p.fire_light == want.fire_light &&
            p.neon_glow == want.neon_glow && p.neon_glow_strength < 0 &&
            p.soft_particles == want.soft_particles &&
            p.sharp_fonts == want.sharp_fonts &&
            p.filmic == want.filmic && p.sky_hd == want.sky_hd && p.sharp_faces == want.sharp_faces &&
            p.rain_puddles == want.rain_puddles && p.contact_shadows == want.contact_shadows &&
            /* only Balanced covers them: Performance and Quality match any draw distance and pools */
            (preset != Preset::Balanced || (p.draw_distance == want.draw_distance && p.fx_scale == want.fx_scale)))
            return preset;
    }
    return Preset::Custom;
}

const char *preset_key(Preset preset) {
    switch (preset) {
    case Preset::Laptop: return "laptop";
    case Preset::Balanced: return "balanced";
    case Preset::Gaming: return "gaming";
    default: return "custom";
    }
}

Preset preset_from_key(const std::string &key) {
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming})
        if (key == preset_key(preset)) return preset;
    return Preset::Custom;
}

Profile recommended_profile(const std::string &install_dir, Preset preset) {
    if (preset == Preset::Custom) preset = Preset::Balanced;
    Profile p;
    apply_preset(p, preset, install_dir);
    p.language = "en";
    p.stats = true;
    p.preset = preset_key(preset);
    return p;
}

/* ---- extra_args migration ---- */

/* One argument back in split_args syntax: quoted when it holds whitespace or a quote, or is
 * empty. Returns false when it cannot be written (both quote characters). */
static bool requote(const std::string &a, std::string &out) {
    bool plain = !a.empty() && a.find_first_of(" \t\r\n\"'") == std::string::npos;
    if (plain) return out = a, true;
    if (a.find('"') == std::string::npos) return out = "\"" + a + "\"", true;
    if (a.find('\'') == std::string::npos) return out = "'" + a + "'", true;
    return false;
}

/* An integer argument within lo..hi. */
static bool int_arg(const std::string &s, int lo, int hi, int &out) {
    int v;
    if (!parse_int(s, v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

static const char *const FPS_OVERLAY_ITEM_NAMES[] = {"fps", "ms", "cpu", "gpu"};

int parse_fps_overlay_items(const std::string &s) {
    int mask = 0;
    size_t at = 0;
    for (;;) {
        size_t comma = s.find(',', at);
        std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        int bit = -1;
        for (int i = 0; i < 4; i++)
            if (item == FPS_OVERLAY_ITEM_NAMES[i]) bit = i;
        if (bit < 0) return -1;
        mask |= 1 << bit;
        if (comma == std::string::npos) return mask;
        at = comma + 1;
    }
}

int fps_overlay_mask(const Profile &p) {
    if (p.fps_overlay_items.empty()) return FPS_OVERLAY_FPS | FPS_OVERLAY_MS;
    int m = parse_fps_overlay_items(p.fps_overlay_items);
    return m > 0 ? m : FPS_OVERLAY_FPS;
}

void set_fps_overlay_mask(Profile &p, int mask) {
    std::string s;
    for (int i = 0; i < 4; i++)
        if (mask & 1 << i) s += (s.empty() ? "" : ",") + std::string(FPS_OVERLAY_ITEM_NAMES[i]);
    if (!s.empty()) p.fps_overlay_items = s;
}

void enable_fps_overlay(Profile &p) {
    if (!p.fps_overlay && p.fps_overlay_items.empty()) p.fps_overlay_items = "fps";
    p.fps_overlay = true;
}

bool perf_overlay_offer(const ProfileFile &file, const Profile &p) { return !file.settings.perf_overlay_asked && !p.fps_overlay; }

int migrate_extra_args(Profile &p) {
    std::vector<std::string> args = split_args(p.extra_args), keep;
    Profile q = p;
    int moved = 0;
    bool hud_seen = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        const std::string *next = i + 1 < args.size() ? &args[i + 1] : nullptr;
        int v = 0;
        float f = 0;
        bool take = true, value = false;
        if (a == "--sim60" || a == "--no-sim60") q.sim60 = a == "--sim60";
        else if (a == "--shadow-soft") q.shadow_soft = true;
        else if (a == "--shadow-range" || a == "--no-shadow-range") q.shadow_range = a == "--shadow-range";
        else if (a == "--text-fixes" || a == "--no-text-fixes") q.text_fixes = a == "--text-fixes";
        else if (a == "--hud-hd-glass" && !hud_seen && q.hud_style != "glass") {
            q.hud_style = "hd-glass";
            q.hud_track_opacity = q.hud_fill_opacity = q.hud_frost = -1; /* the preset's own values */
        } else if (a == "--supersample" && next && int_arg(*next, 1, 4, v) && v != 3) q.supersample = v, value = true;
        else if (a == "--shadow-scale" && next && int_arg(*next, 1, 4, v)) q.shadow_scale = v, value = true;
        else if (a == "--fx-scale" && next && int_arg(*next, 1, 4, v)) q.fx_scale = v, value = true;
        else if (a == "--frames-in-flight" && next && int_arg(*next, 1, 3, v)) q.frames_in_flight = v, value = true;
        else if (a == "--sharpen" && next && int_arg(*next, 0, 100, v)) q.sharpen = v, value = true;
        else if (a == "--bloom" || a == "--no-bloom") q.bloom = a == "--bloom";
        else if (a == "--bloom-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.bloom_strength = f, value = true;
        else if (a == "--rain-hd" || a == "--no-rain-hd") q.rain_hd = a == "--rain-hd";
        else if (a == "--rain-hd-puddles" || a == "--no-rain-hd-puddles") q.rain_puddles = a == "--rain-hd-puddles";
        else if (a == "--contact-shadows" || a == "--no-contact-shadows") q.contact_shadows = a == "--contact-shadows";
        else if (a == "--ssao" || a == "--no-ssao") q.ssao = a == "--ssao";
        else if (a == "--ssao-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 2) q.ssao_strength = f, value = true;
        else if (a == "--lamp-lights" || a == "--no-lamp-lights") q.lamp_lights = a == "--lamp-lights";
        else if (a == "--lamp-lights-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.lamp_lights_strength = f, value = true;
        else if (a == "--neon-glow" || a == "--no-neon-glow") q.neon_glow = a == "--neon-glow";
        else if (a == "--neon-glow-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.neon_glow_strength = f, value = true;
        else if (a == "--char-lighting" || a == "--no-char-lighting") q.char_lighting = a == "--char-lighting";
        else if (a == "--world-lighting" || a == "--no-world-lighting") q.world_lighting = a == "--world-lighting";
        else if (a == "--fire-light" || a == "--no-fire-light") q.fire_light = a == "--fire-light";
        else if (a == "--soft-particles" || a == "--no-soft-particles") q.soft_particles = a == "--soft-particles";
        else if (a == "--fps-overlay" || a == "--no-fps-overlay") q.fps_overlay = a == "--fps-overlay";
        else if (a == "--fps-overlay-items" && next && parse_fps_overlay_items(*next) > 0)
            set_fps_overlay_mask(q, parse_fps_overlay_items(*next)), value = true;
        else if (a == "--fps-overlay-corner" && next && (*next == "tl" || *next == "tr" || *next == "bl" || *next == "br"))
            q.fps_overlay_corner = *next, value = true;
        else if (a == "--filmic" || a == "--no-filmic") q.filmic = a == "--filmic";
        else if (a == "--sky-hd" || a == "--no-sky-hd") q.sky_hd = a == "--sky-hd";
        else if (a == "--sharp-fonts" || a == "--no-sharp-fonts") q.sharp_fonts = a == "--sharp-fonts";
        else if (a == "--sharp-faces" || a == "--no-sharp-faces") q.sharp_faces = a == "--sharp-faces";
        else if (a == "--sharp-faces-over-pack" || a == "--no-sharp-faces-over-pack") q.sharp_faces_over_pack = a == "--sharp-faces-over-pack";
        else if (a == "--skip-intro" || a == "--no-skip-intro") q.skip_intro = a == "--skip-intro";
        else if (a == "--spray-time" && next && int_arg(*next, SPRAY_TIME_MIN, SPRAY_TIME_MAX, v)) q.spray_time = v, value = true;
        else take = false;
        if (a.compare(0, 5, "--hud") == 0) hud_seen = true;
        if (!take) {
            keep.push_back(a);
            continue;
        }
        moved++;
        if (value) i++;
    }
    if (!moved) return 0;
    std::string extra;
    for (const std::string &a : keep) {
        std::string quoted;
        if (!requote(a, quoted)) return 0; /* not representable: leave the profile as typed */
        extra += (extra.empty() ? "" : " ") + quoted;
    }
    q.extra_args = extra;
    p = q;
    return moved;
}

std::string unique_profile_name(const ProfileFile &file, const std::string &base) {
    for (int n = 1;; n++) {
        std::string name = n == 1 ? base : base + " " + std::to_string(n);
        bool used = false;
        for (const Profile &p : file.profiles) used |= p.name == name;
        if (!used) return name;
    }
}

} // namespace launcher
