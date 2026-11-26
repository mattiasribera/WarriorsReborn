#include "profile.h"
#include "command.h"
#include "platform.h"
#include "../../runtime/src/settings_model.h"
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
    {"button_icons", &Profile::button_icons}, {"glyph_packs", &Profile::glyph_packs},
    {"icon_pack", &Profile::icon_pack},
    {"neon_masks", &Profile::neon_masks},
    {"language", &Profile::language},    {"input_config", &Profile::input_config},
    {"warp", &Profile::warp},
    {"mods", &Profile::mods},           {"mods_dir", &Profile::mods_dir},
    {"audio", &Profile::audio},          {"video", &Profile::video},
    {"renderer", &Profile::renderer},    {"hud_style", &Profile::hud_style},
    {"fsr", &Profile::fsr},
    {"ssao_mode", &Profile::ssao_mode},  {"gtao_quality", &Profile::gtao_quality},
    {"ssgi_quality", &Profile::ssgi_quality},
    {"reflections", &Profile::reflections},
    {"taa_upscale", &Profile::taa_upscale},
    {"upscaler", &Profile::upscaler},
    {"hdr", &Profile::hdr},
    {"gpu_vertex_programs", &Profile::gpu_vertex_programs},
    {"preset", &Profile::preset},        {"extra_args", &Profile::extra_args},
    {"fps_overlay_items", &Profile::fps_overlay_items}, {"fps_overlay_corner", &Profile::fps_overlay_corner},
    {"volfog_quality", &Profile::volfog_quality},
};
static const IntKey int_keys[] = {
    {"render_scale", &Profile::render_scale}, {"msaa", &Profile::msaa},
    {"aniso", &Profile::aniso},               {"spawn_scale", &Profile::spawn_scale},
    {"supersample", &Profile::supersample},   {"shadow_scale", &Profile::shadow_scale},
    {"fx_scale", &Profile::fx_scale},
    {"frames_in_flight", &Profile::frames_in_flight}, {"spray_time", &Profile::spray_time},
    {"dialogue_volume", &Profile::dialogue_volume}, {"music_volume", &Profile::music_volume},
    {"sfx_volume", &Profile::sfx_volume},
    {"hdr_peak", &Profile::hdr_peak}, {"hdr_paper_white", &Profile::hdr_paper_white},
    {"sharpen", &Profile::sharpen},           {"draw_distance", &Profile::draw_distance},
};
static const BoolKey bool_keys[] = {
    {"widescreen", &Profile::widescreen},   {"fullscreen", &Profile::fullscreen},
    {"refresh_match", &Profile::refresh_match},
    {"rate_fixes", &Profile::rate_fixes},   {"frame_lock", &Profile::frame_lock},
    {"render_targets", &Profile::render_targets}, {"render64", &Profile::render64}, {"stats", &Profile::stats},
    {"load_profile", &Profile::load_profile},
    {"trace", &Profile::trace},             {"shadow_soft", &Profile::shadow_soft},
    {"sim60", &Profile::sim60},             {"text_fixes", &Profile::text_fixes},
    {"script_fixes", &Profile::script_fixes},
    {"rumble", &Profile::rumble},           {"bloom", &Profile::bloom},
    {"shadow_range", &Profile::shadow_range}, {"rain_hd", &Profile::rain_hd},
    {"ssao", &Profile::ssao},               {"movie_hd", &Profile::movie_hd},
    {"gamma_ramp", &Profile::gamma_ramp},
    {"filmic", &Profile::filmic},
    {"auto_exposure", &Profile::auto_exposure},
    {"cutscene_dof", &Profile::cutscene_dof},
    {"dynres", &Profile::dynres},
    {"vrs", &Profile::vrs},
    {"taa", &Profile::taa},
    {"frame_gen", &Profile::frame_gen},
    {"anim_interp", &Profile::anim_interp},
    {"sky_hd", &Profile::sky_hd},
    {"sharp_fonts", &Profile::sharp_fonts},
    {"sharp_faces", &Profile::sharp_faces},
    {"sharp_faces_over_pack", &Profile::sharp_faces_over_pack},
    {"skip_intro", &Profile::skip_intro},
    {"smart_allies", &Profile::smart_allies},
    {"enemy_tactics", &Profile::enemy_tactics},
    {"rain_puddles", &Profile::rain_puddles},
    {"wet_light_streaks", &Profile::wet_light_streaks},
    {"materials", &Profile::materials},
    {"damp_streets", &Profile::damp_streets},
    {"neon_reflections", &Profile::neon_reflections},
    {"contact_shadows", &Profile::contact_shadows},
    {"lamp_lights", &Profile::lamp_lights},
    {"ssgi", &Profile::ssgi},
    {"neon_glow", &Profile::neon_glow},
    {"char_lighting", &Profile::char_lighting},
    {"hair", &Profile::hair},
    {"skin", &Profile::skin},
    {"eyes", &Profile::eyes},
    {"cloth", &Profile::cloth},
    {"world_lighting", &Profile::world_lighting},
    {"normal_maps", &Profile::normal_maps},
    {"dry_highlights", &Profile::dry_highlights},
    {"parallax", &Profile::parallax},
    {"level_lights", &Profile::level_lights},
    {"level_lights_cheap", &Profile::level_lights_cheap},
    {"fire_light", &Profile::fire_light},
    {"lamp_shadows", &Profile::lamp_shadows},
    {"world_shadows", &Profile::world_shadows},
    {"world_shadows_low", &Profile::world_shadows_low},
    {"light_shafts", &Profile::light_shafts},
    {"light_shafts_cheap", &Profile::light_shafts_cheap},
    {"volfog", &Profile::volfog},
    {"soft_particles_opt_in", &Profile::soft_particles},
    {"fps_overlay", &Profile::fps_overlay},
    {"audio_hrtf", &Profile::audio_hrtf},
    {"audio_air", &Profile::audio_air},
    {"audio_occlusion", &Profile::audio_occlusion},
    {"audio_reverb_geo", &Profile::audio_reverb_geo},
    {"audio_pathing", &Profile::audio_pathing},
};
static const FloatKey float_keys[] = {
    {"hud_track_opacity", &Profile::hud_track_opacity}, {"hud_fill_opacity", &Profile::hud_fill_opacity},
    {"hud_frost", &Profile::hud_frost},                 {"hud_height", &Profile::hud_height},
    {"bloom_strength", &Profile::bloom_strength},     {"ssao_strength", &Profile::ssao_strength},
    {"lamp_lights_strength", &Profile::lamp_lights_strength},
    {"ssgi_strength", &Profile::ssgi_strength},
    {"neon_glow_strength", &Profile::neon_glow_strength},
    {"taa_sharpness", &Profile::taa_sharpness},
    {"saturation", &Profile::saturation},
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
    if (key == "hair") p.hair_saved = true;
    if (key == "skin") p.skin_saved = true;
    if (key == "eyes") p.eyes_saved = true;
    if (key == "cloth") p.cloth_saved = true;
    if (key == "world_lighting") p.world_lighting_saved = true;
    if (key == "fire_light") p.fire_light_saved = true;
    if (key == "level_lights") p.level_lights_saved = true;
    if (key == "lamp_shadows") p.lamp_shadows_saved = true;
    if (key == "world_shadows") p.world_shadows_saved = true;
    if (key == "world_shadows_low") p.world_shadows_low_saved = true;
    if (key == "light_shafts") p.light_shafts_saved = true;
    if (key == "volfog") p.volfog_saved = true;
    if (key == "wet_light_streaks") p.wet_light_streaks_saved = true;
    if (key == "materials") p.materials_saved = true;
    if (key == "damp_streets") p.damp_streets_saved = true;
    if (key == "neon_reflections") p.neon_reflections_saved = true;
    if (key == "normal_maps") p.normal_maps_saved = true;
    if (key == "dry_highlights") p.dry_highlights_saved = true;
    if (key == "parallax") p.parallax_saved = true;
    if (key == "skip_intro") p.skip_intro_saved = true;
    if (key == "dynres") p.dynres_saved = true;
    if (key == "vrs") p.vrs_saved = true;
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
    else if (key == "crash_seen") s.crash_seen = value;
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
        const bool high = saved == Preset::Gaming || saved == Preset::Ultra;
        if (!p.char_lighting_saved && (saved == Preset::Balanced || high)) p.char_lighting = true;
        /* world_lighting likewise: a profile saved as Quality takes it on. */
        if (!p.world_lighting_saved && high) p.world_lighting = true;
        /* fire_light likewise. */
        if (!p.fire_light_saved && high) p.fire_light = true;
        /* level_lights likewise. */
        if (!p.level_lights_saved && high) p.level_lights = true, p.level_lights_cheap = false;
        /* lamp_shadows likewise. */
        if (!p.lamp_shadows_saved && saved == Preset::Ultra) p.lamp_shadows = true;
        /* world_shadows likewise. */
        if (!p.world_shadows_saved && high) p.world_shadows = true;
        /* light_shafts likewise. */
        if (!p.light_shafts_saved && high) p.light_shafts = true;
        /* volfog likewise. */
        if (!p.volfog_saved && high) p.volfog = true;
        /* wet_light_streaks likewise. */
        if (!p.wet_light_streaks_saved && high) p.wet_light_streaks = true;
        /* materials likewise. */
        if (!p.materials_saved && high) p.materials = true;
        /* damp_streets likewise. */
        if (!p.damp_streets_saved && high) p.damp_streets = true;
        /* neon_reflections likewise. */
        if (!p.neon_reflections_saved && high) p.neon_reflections = true;
        /* normal_maps likewise. */
        if (!p.normal_maps_saved && high) p.normal_maps = true;
        /* dry_highlights likewise. */
        if (!p.dry_highlights_saved && high) p.dry_highlights = true;
        /* hair likewise. */
        if (!p.hair_saved && high) p.hair = true;
        /* skin likewise. */
        if (!p.skin_saved && high) p.skin = true;
        /* eyes and cloth likewise. */
        if (!p.eyes_saved && high) p.eyes = true;
        if (!p.cloth_saved && high) p.cloth = true;
        /* parallax likewise. */
        if (!p.parallax_saved && high) p.parallax = true;
        /* dynres likewise: the presets with temporal upscaling turn it on. */
        if (!p.dynres_saved && saved != Preset::Custom && saved != Preset::Ultra) {
            Profile want;
            apply_preset(want, saved, "");
            p.dynres = want.dynres;
        }
        /* vrs likewise: the presets that turn it on. */
        if (!p.vrs_saved && saved != Preset::Custom) {
            Profile want;
            apply_preset(want, saved, "");
            p.vrs = want.vrs;
        }
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
    if (!st.crash_seen.empty()) s += "crash_seen = " + st.crash_seen + "\n";
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

std::vector<std::pair<std::string, std::string>> profile_pairs(const Profile &p) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const StrKey &k : str_keys) out.push_back({k.key, k.field == &Profile::preset ? preset_key(detect_preset(p)) : p.*k.field});
    for (const IntKey &k : int_keys) out.push_back({k.key, std::to_string(p.*k.field)});
    for (const BoolKey &k : bool_keys) out.push_back({k.key, p.*k.field ? "true" : "false"});
    for (const FloatKey &k : float_keys) out.push_back({k.key, format_float(p.*k.field)});
    return out;
}

int merge_pending(ProfileFile &file, const std::string &text, std::vector<std::string> *warnings) {
    std::string profile;
    std::vector<std::pair<std::string, std::string>> keys;
    bool magic = false, section = false;
    size_t pos = 0;
    while (pos < text.size()) {   /* read all of it first: a bad file changes nothing */
        size_t nl = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!magic) {
            if (line.empty()) continue;
            if (line != SM_PENDING_MAGIC) return -1;
            magic = true;
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            if (section || line.back() != ']') return -1;
            std::string name = trim(line.substr(1, line.size() - 2));
            if (name.compare(0, sizeof PROFILE_PREFIX - 1, PROFILE_PREFIX)) return -1;
            profile = trim(name.substr(sizeof PROFILE_PREFIX - 1));
            section = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || !section) return -1;
        keys.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
    }
    if (!section) return -1;
    Profile *p = nullptr;
    for (Profile &q : file.profiles)
        if (q.name == profile) p = &q;
    if (!p) return -1;
    int set = 0;
    for (const auto &kv : keys) {
        const SmSetting *s = sm_setting(sm_find(kv.first.c_str()));
        bool bad = false;
        if (!s || !(s->flags & SM_MENU)) {
            if (warnings) warnings->push_back("pending: key '" + kv.first + "' is not an in-game setting (ignored)");
        } else if (!sm_valid(s, kv.second.c_str()) || !set_key(*p, kv.first, kv.second, &bad) || bad) {
            if (warnings) warnings->push_back("pending: bad value '" + kv.second + "' for '" + kv.first + "' (ignored)");
        } else {
            set++;
        }
    }
    p->preset = preset_key(detect_preset(*p));
    return set;
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
    /* Ultra is Quality at full resolution with the lamps' character shadows:
     * every `gaming` choice below is on in both. */
    bool ultra = preset == Preset::Ultra, gaming = preset == Preset::Gaming || ultra, balanced = preset == Preset::Balanced;
    /* Integrated graphics: the profile tuned by hand on an Intel Iris Xe laptop (driver 101.6913), from
     * about 20-25 fps to 55-60. Quality's lighting and materials at 1600x900 with balanced temporal upscaling
     * (941x529 for 900p) and dynamic resolution, without the passes that cost the most there. */
    bool igpu = preset == Preset::Integrated;
    /* the swapchain stays at the display's mode in fullscreen; the game renders 1600x900 into it */
    p.resolution = igpu ? "1600x900" : "1920x1080";
    p.fsr = "off";
    p.widescreen = p.fullscreen = true;
    /* Temporal anti-aliasing: about 0.25-0.3 ms on an RTX 2060 at 1080p and it
     * smooths what MSAA misses (fences, wires, shimmer), so Quality pairs it with MSAA 2x (about the
     * cost of MSAA 4x alone) and Balanced uses it instead of MSAA 2x; Performance stays without
     * either (an integrated GPU pays about 1.3 ms at 1080p). */
    p.taa = gaming || balanced || igpu;
    /* Temporal upscaling (--taa-upscale quality: the scene at 1280x720 for 1080p, rebuilt by the TAA
     * resolve): the per-pixel lighting is most of the frame, so at 1080p it saved
     * 5.5 ms of an RTX 2060's 13.2 in the hideout with every Quality light on. Quality and Balanced
     * use it, so Quality holds 60 FPS on an RTX 2050-class laptop GPU and Balanced fits an Iris Xe; Ultra
     * renders at full resolution. Quality uses ultra (1477x831 for 1080p):
     * with the sharper resolve and RCAS at 0.5 its fine detail measured at
     * native 1080p's (1.05) against 0.69 for quality as first shipped, for about 1 ms more on a 2060
     * in the hideout (an RTX 2050 estimate within its 13-14 ms budget). */
    p.taa_upscale = balanced ? "quality" : gaming && !ultra ? "ultra" : igpu ? "balanced" : "off";
    p.upscaler = "taa";   /* --upscaler: our TAA resolve in every preset */
    p.msaa = gaming ? 2 : 0;
    p.supersample = 1; /* off in every preset: it costs 4x the pixels and slows most PCs */
    /* Anisotropic filtering 16x keeps the street readable at grazing angles (with the upscaler's
     * negative LOD bias) for a small cost on dedicated GPUs; Balanced and
     * Performance, which aim at integrated GPUs where texture bandwidth is short, keep 4x. */
    p.aniso = gaming || igpu ? 16 : 4;   /* 16x measured fine on the Iris Xe */
    p.shadow_scale = gaming ? 2 : 1;
    p.shadow_soft = gaming || balanced || igpu;
    p.shadow_range = gaming || igpu;
    p.frames_in_flight = preset == Preset::Laptop ? 2 : 0;
    p.sim60 = true;
    /* Smooth animation blends draws that change every other frame: the
     * 30 FPS mode. With sim60 (every preset) the game already changes everything every frame, so it
     * stays off; the toggle is for players who choose 30 FPS. */
    p.anim_interp = false;
    p.hud_style = "hd-glass";
    p.hud_track_opacity = p.hud_fill_opacity = p.hud_frost = p.hud_height = -1;
    p.bloom = gaming || balanced || igpu;
    p.bloom_strength = -1;
    p.taa_sharpness = -1; /* the runtime's default */
    /* Dynamic resolution (--dynres auto): with temporal upscaling the render
     * size drops for the frames whose GPU time would miss 60 fps and comes back when there is room,
     * so a heavy street does not stutter on an RTX 2050 or an Iris Xe; on a fast GPU it never moves.
     * Every preset that upscales has it; Ultra renders at full size. */
    p.dynres = p.taa_upscale != "off";
    /* Variable rate shading (--vrs auto): measured on an RTX 2060 in mission 7's street
     * (GPU ms of the scene draws): Quality 2.18 -> 1.92 (the rate pass 0.07, about 0.2 ms net, 0.35 on an
     * RTX 2050; hideout 2.27 -> 1.77, rain 2.18 -> 1.73), Ultra 2.90 -> 2.44 (0.38 net); Balanced (720p, no MSAA) saves 0.08, less than the rate
     * pass, and Performance has no TAA to hide the coarser shading: off in both. Off on GPUs without it. */
    p.vrs = gaming;
    p.rain_hd = gaming || balanced || igpu;
    p.rain_puddles = gaming || balanced || igpu; /* about 0.05 ms at 1080p: cheap enough for Balanced */
    /* lamps mirrored in the wet ground: only while it rains, inside the wet pass
     * */
    p.wet_light_streaks = gaming || igpu;
    /* the wet ground by surface material: only while it rains, a few lookups per wet pixel at half
     * size */
    p.materials = gaming || igpu;
    p.ssao = gaming || igpu;
    p.ssao_strength = -1;
    /* XeGTAO High in Quality: crisper contact lines at classic's cost, and with the temporal pass as
     * stable as classic under TAA; classic where SSAO is off by default. */
    p.ssao_mode = gaming || igpu ? "gtao" : "classic";
    p.gtao_quality = igpu ? "medium" : "high";   /* medium on the Iris Xe: about 0.3 ms at 941x529 */
    p.contact_shadows = gaming; /* 0.3-0.7 ms at 1080p: Quality only */
    /* Bounce lighting (--ssgi): off in every preset. Measured in game it is a
     * faint, local tint next to neon and lit walls at the default strength for about 0.3-0.6 ms on
     * an RTX 2060 (x1.8 on an RTX 2050), so it is the player's choice (Display > Lighting). */
    p.ssgi = false;
    p.ssgi_quality = "medium";
    p.ssgi_strength = -1;
    p.lamp_lights = gaming || igpu;
    p.lamp_lights_strength = -1;
    p.neon_glow = gaming || balanced || igpu; /* one texture sample on the masked signs' draws only: Balanced too */
    p.neon_glow_strength = -1;
    p.char_lighting = gaming || balanced || igpu; /* well under 0.3 ms at 1080p (characters only): Balanced too */
    p.world_lighting = gaming || igpu; /* the whole lit world: Quality only */
    /* relief from the relief-pack companion maps, shaded by the per-pixel lights above: Quality only
     * (launch adds the pack, or leaves the flag out without it: add_relief_pack) */
    p.normal_maps = gaming || igpu;
    /* metal and glass highlights from the relief pack's material channels: about 0.1-0.2 ms on an
     * RTX 2060 at 1080p, with the relief: Quality only */
    p.dry_highlights = gaming || igpu;
    /* better hair on the characters (with their lighting): Quality only */
    p.hair = gaming || igpu;
    /* realistic skin on the characters (with their lighting): Quality only */
    p.skin = gaming || igpu;
    /* realistic eyes and clothing materials (with their lighting): Quality only */
    p.eyes = gaming || igpu;
    p.cloth = gaming || igpu;
    /* damp night streets without rain: the wet ground's pass at half size in dry night frames,
     * Quality only */
    p.damp_streets = gaming;
    /* neon signs as lights of the wet and damp ground's streaks (at most 8): Quality
     * only (with the damp streets and the wet light streaks) */
    p.neon_reflections = gaming;
    /* the wet ground's reflections: FidelityFX SSSR costs about 0.13-0.27 ms on an RTX 2060 at 1080p
     * but looked about the same as the classic march in play, rough ground a little dimmer
     *, so every preset keeps classic and SSSR is the player's choice */
    p.reflections = "classic";
    /* relief with depth from the height in the relief pack's normal maps: about 0.1-0.2 ms on an RTX 2060
     * at 1080p MSAA 4, with the relief: Quality only */
    p.parallax = gaming || igpu;
    p.fire_light = gaming || igpu;   /* up to 8 more lights in the lit draws */
    p.level_lights = gaming || igpu;   /* the level's lights instead of the halos */
    p.level_lights_cheap = igpu;               /* at most 12 of them: the integrated graphics preset */
    /* A ray march per lit pixel towards each lamp: 3.0 ms of an RTX 2060's 8.8 at 1280x720 in the
     * hideout, more than every other light feature together: Ultra only. */
    p.lamp_shadows = ultra;
    /* About 0.55 ms on an RTX 2060 at 1080p MSAA 4 with the 2048 map;
     * the 1024 map is a manual toggle for integrated GPUs. */
    p.world_shadows = gaming || igpu;
    p.world_shadows_low = igpu;           /* the 1024 map in the integrated graphics preset */
    p.light_shafts = gaming;              /* ~0.5-1 ms at 1080p on a mid-range card: Quality only */
    /* Volumetric fog: high quality measured within ~0.7 ms on an RTX 2060 at 1080p;
     * Quality only (the runtime skips the light shafts while it runs). */
    p.volfog = gaming || igpu;   /* the runtime skips the light shafts while it runs: the integrated preset leaves them off */
    p.volfog_quality = igpu ? "low" : "";
    p.soft_particles = igpu;  /* opt-in (Display) in every other preset; cheap enough at 941x529 on the Iris Xe */
    p.sharp_fonts = true;     /* cheap (one upscale on a worker thread, ~64 MB of video memory): every preset */
    /* Draw distance and effect pools: Balanced takes the mildest step up, Performance goes back
     * to the shipped values, Quality leaves them as they are (older Quality profiles keep their
     * own choice and stay Quality). */
    if (balanced || igpu) p.draw_distance = p.fx_scale = 2;
    else if (preset == Preset::Laptop) p.draw_distance = p.fx_scale = 1;
    p.sky_hd = gaming || balanced || igpu;
    p.sharp_faces = gaming || balanced || igpu; /* cheap: 1.3 MB per 256 skin (5.3 MB per 512), filtered off the frame thread */
    p.sharp_faces_over_pack = false;    /* the user's choice over their pack: no preset turns it on (not in the match) */
    p.fps_overlay = false;              /* Show FPS: a display aid, no preset turns it on (not in the match) */
    p.filmic = gaming || balanced || igpu; /* the cinematic look is cheap: on in Balanced and Quality */
    p.auto_exposure = gaming || balanced || igpu; /* inside the filmic pass: about 0.02 ms */
    /* only in cutscenes, inside the filmic pass: about 0.3 ms on an RTX 2060 at 1080p
     *; nothing in gameplay */
    p.cutscene_dof = gaming || balanced || igpu;
    p.skip_intro = true; /* every preset; a Game option, so detect_preset leaves it out */
    std::string texpack = path_join(install_dir, TEXPACK_DIR);
    if (p.texture_pack.empty() && is_dir(texpack)) p.texture_pack = texpack;
    std::string masks = path_join(install_dir, NEON_MASKS_DIR);
    if (p.neon_masks.empty() && !install_dir.empty() && is_dir(masks)) p.neon_masks = masks;
}

bool sharp_faces_over_pack_offered(const Profile &p) { return p.sharp_faces && !p.texture_pack.empty(); }

bool ssgi_used(const Profile &p) { return p.ssgi && p.ssao && p.ssao_mode == "gtao"; }

void choose_fsr(Profile &p, const std::string &mode) {
    p.fsr = mode;
    if (mode != "off") p.taa_upscale = "off";
}

void choose_taa_upscale(Profile &p, const std::string &mode) {
    p.taa_upscale = mode;
    if (mode != "off") p.fsr = "off";
}

void choose_upscaler(Profile &p, const std::string &upscaler) {
    p.upscaler = upscaler;
    if (p.taa_upscale == "off") choose_taa_upscale(p, "quality");
}

const char *upscaler_shown(const Profile &p) { return p.taa_upscale == "off" ? "" : p.upscaler.c_str(); }

Preset detect_preset(const Profile &p) {
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated}) {
        Profile want = p;
        want.texture_pack = "";
        apply_preset(want, preset, "");
        if (p.resolution == want.resolution && p.widescreen == want.widescreen && p.fullscreen == want.fullscreen &&
            p.msaa == want.msaa && p.taa == want.taa && p.supersample == want.supersample && p.aniso == want.aniso &&
            p.shadow_scale == want.shadow_scale && p.shadow_soft == want.shadow_soft && p.shadow_range == want.shadow_range &&
            p.frames_in_flight == want.frames_in_flight && p.sim60 == want.sim60 && p.anim_interp == want.anim_interp && p.hud_style == want.hud_style &&
            p.hud_track_opacity < 0 && p.hud_fill_opacity < 0 && p.hud_frost < 0 && p.hud_height < 0 && p.bloom == want.bloom &&
            p.bloom_strength < 0 && p.taa_sharpness < 0 && p.rain_hd == want.rain_hd && p.ssao == want.ssao && p.ssao_strength < 0 &&
            (!p.ssao || (p.ssao_mode == want.ssao_mode && (p.ssao_mode != "gtao" || p.gtao_quality == want.gtao_quality))) &&
            ssgi_used(p) == ssgi_used(want) && (!ssgi_used(p) || (p.ssgi_quality == want.ssgi_quality && p.ssgi_strength < 0)) &&
            p.lamp_lights == want.lamp_lights && p.lamp_lights_strength < 0 && p.char_lighting == want.char_lighting &&
            p.world_lighting == want.world_lighting && p.fire_light == want.fire_light && p.lamp_shadows == want.lamp_shadows &&
            p.light_shafts == want.light_shafts && p.volfog == want.volfog && p.level_lights == want.level_lights &&
            (!p.level_lights || p.level_lights_cheap == want.level_lights_cheap) &&
            p.world_shadows == want.world_shadows && (!p.world_shadows || p.world_shadows_low == want.world_shadows_low) &&
            p.neon_glow == want.neon_glow && p.neon_glow_strength < 0 &&
            p.soft_particles == want.soft_particles &&
            p.sharp_fonts == want.sharp_fonts &&
            p.filmic == want.filmic && p.auto_exposure == want.auto_exposure && p.cutscene_dof == want.cutscene_dof && p.sky_hd == want.sky_hd && p.sharp_faces == want.sharp_faces &&
            p.rain_puddles == want.rain_puddles && p.wet_light_streaks == want.wet_light_streaks && p.materials == want.materials && p.damp_streets == want.damp_streets && p.neon_reflections == want.neon_reflections && p.reflections == want.reflections && p.normal_maps == want.normal_maps && p.dry_highlights == want.dry_highlights && p.hair == want.hair && p.skin == want.skin && p.eyes == want.eyes && p.cloth == want.cloth && p.parallax == want.parallax && p.contact_shadows == want.contact_shadows && p.fsr == want.fsr && p.taa_upscale == want.taa_upscale && p.upscaler == want.upscaler && p.dynres == want.dynres && p.vrs == want.vrs &&
            /* only Balanced and Integrated cover them: Performance and Quality match any draw distance and pools */
            ((preset != Preset::Balanced && preset != Preset::Integrated) ||
             (p.draw_distance == want.draw_distance && p.fx_scale == want.fx_scale)) &&
            /* only Integrated covers the fog quality (low) */
            (preset != Preset::Integrated || !p.volfog || p.volfog_quality == want.volfog_quality))
            return preset;
    }
    return Preset::Custom;
}

const char *preset_key(Preset preset) {
    switch (preset) {
    case Preset::Laptop: return "laptop";
    case Preset::Balanced: return "balanced";
    case Preset::Gaming: return "gaming";
    case Preset::Ultra: return "ultra";
    case Preset::Integrated: return "integrated";
    default: return "custom";
    }
}

Preset preset_from_key(const std::string &key) {
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated})
        if (key == preset_key(preset)) return preset;
    return Preset::Custom;
}

Profile recommended_profile(const std::string &install_dir, Preset preset) {
    if (preset == Preset::Custom) preset = Preset::Balanced;
    Profile p;
    apply_preset(p, preset, install_dir);
    p.language = "en";
    p.stats = true;
    std::string glyphs = path_join(install_dir, "glyph-packs");
    if (is_dir(path_join(glyphs, "ps-black"))) {
        p.button_icons = "ps-black";
        p.glyph_packs = glyphs;
    }
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
        else if (a == "--script-fixes" || a == "--no-script-fixes") q.script_fixes = a == "--script-fixes";
        else if (a == "--hud-hd-glass" && !hud_seen && q.hud_style != "glass") {
            q.hud_style = "hd-glass";
            q.hud_track_opacity = q.hud_fill_opacity = q.hud_frost = -1; /* the preset's own values */
        } else if (a == "--supersample" && next && int_arg(*next, 1, 4, v) && v != 3) q.supersample = v, value = true;
        else if (a == "--shadow-scale" && next && int_arg(*next, 1, 4, v)) q.shadow_scale = v, value = true;
        else if (a == "--fsr" && next && (*next == "off" || *next == "ultra" || *next == "quality" || *next == "balanced" || *next == "performance")) q.fsr = *next, value = true;
        else if (a == "--reflections" && next && (*next == "classic" || *next == "sssr")) q.reflections = *next, value = true;
        else if (a == "--taa-upscale" && next && (*next == "off" || *next == "native" || *next == "ultra" || *next == "quality" || *next == "balanced" || *next == "performance"))
            q.taa_upscale = *next, value = true;
        else if (a == "--upscaler" && next && (*next == "taa" || *next == "fsr3")) q.upscaler = *next, value = true;
        else if (a == "--taa-sharpness" && next && parse_float(*next, f) && f >= 0 && f <= 1) q.taa_sharpness = f, value = true;
        else if (a == "--dynres" && next && *next == "auto") q.dynres = true, value = true;   /* a typed target in ms stays typed */
        else if (a == "--no-dynres") q.dynres = false;
        else if (a == "--vrs" && next && (*next == "auto" || *next == "off")) q.vrs = *next == "auto", value = true;   /* quality / performance stay typed */
        else if (a == "--saturation" && next && parse_float(*next, f) && f >= 0.5f && f <= 2) q.saturation = f, value = true;
        else if (a == "--hdr" && next && (*next == "off" || *next == "auto" || *next == "on")) q.hdr = *next, value = true;
        else if (a == "--hdr-peak" && next && int_arg(*next, 400, 10000, v)) q.hdr_peak = v, value = true;
        else if (a == "--hdr-paper-white" && next && int_arg(*next, 80, 500, v)) q.hdr_paper_white = v, value = true;
        else if (a == "--fx-scale" && next && int_arg(*next, 1, 4, v)) q.fx_scale = v, value = true;
        else if (a == "--frames-in-flight" && next && int_arg(*next, 1, 3, v)) q.frames_in_flight = v, value = true;
        else if (a == "--sharpen" && next && int_arg(*next, 0, 100, v)) q.sharpen = v, value = true;
        else if (a == "--bloom" || a == "--no-bloom") q.bloom = a == "--bloom";
        else if (a == "--bloom-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.bloom_strength = f, value = true;
        else if (a == "--rain-hd" || a == "--no-rain-hd") q.rain_hd = a == "--rain-hd";
        else if (a == "--rain-hd-puddles" || a == "--no-rain-hd-puddles") q.rain_puddles = a == "--rain-hd-puddles";
        else if (a == "--wet-light-streaks" || a == "--no-wet-light-streaks") q.wet_light_streaks = a == "--wet-light-streaks";
        else if (a == "--materials" || a == "--no-materials") q.materials = a == "--materials";
        else if (a == "--damp-streets" || a == "--no-damp-streets") q.damp_streets = a == "--damp-streets";
        else if (a == "--neon-reflections" || a == "--no-neon-reflections") q.neon_reflections = a == "--neon-reflections";
        else if (a == "--contact-shadows" || a == "--no-contact-shadows") q.contact_shadows = a == "--contact-shadows";
        else if (a == "--ssao" || a == "--no-ssao") q.ssao = a == "--ssao";
        else if (a == "--ssao-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 2) q.ssao_strength = f, value = true;
        else if (a == "--ssao-mode" && next && (*next == "classic" || *next == "gtao")) q.ssao_mode = *next, value = true;
        else if (a == "--gtao-quality" && next && (*next == "low" || *next == "medium" || *next == "high")) q.gtao_quality = *next, value = true;
        else if (a == "--ssgi" && next && *next == "off") q.ssgi = false, value = true;
        else if (a == "--ssgi" && next && (*next == "low" || *next == "medium" || *next == "high")) q.ssgi = true, q.ssgi_quality = *next, value = true;
        else if (a == "--ssgi-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.ssgi_strength = f, value = true;
        else if (a == "--lamp-lights" || a == "--no-lamp-lights") q.lamp_lights = a == "--lamp-lights";
        else if (a == "--lamp-lights-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.lamp_lights_strength = f, value = true;
        else if (a == "--neon-glow" || a == "--no-neon-glow") q.neon_glow = a == "--neon-glow";
        else if (a == "--neon-glow-strength" && next && parse_float(*next, f) && f >= 0.1f && f <= 3) q.neon_glow_strength = f, value = true;
        else if (a == "--char-lighting" || a == "--no-char-lighting") q.char_lighting = a == "--char-lighting";
        else if (a == "--hair" || a == "--no-hair") q.hair = a == "--hair";
        else if (a == "--skin" || a == "--no-skin") q.skin = a == "--skin";
        else if (a == "--eyes" || a == "--no-eyes") q.eyes = a == "--eyes";
        else if (a == "--cloth" || a == "--no-cloth") q.cloth = a == "--cloth";
        else if (a == "--world-lighting" || a == "--no-world-lighting") q.world_lighting = a == "--world-lighting";
        else if (a == "--normal-maps" || a == "--no-normal-maps") q.normal_maps = a == "--normal-maps";
        else if (a == "--dry-highlights" || a == "--no-dry-highlights") q.dry_highlights = a == "--dry-highlights";
        else if (a == "--parallax" || a == "--no-parallax") q.parallax = a == "--parallax";
        else if (a == "--fire-light" || a == "--no-fire-light") q.fire_light = a == "--fire-light";
        else if (a == "--level-lights" || a == "--no-level-lights") q.level_lights = a == "--level-lights";
        else if (a == "--level-lights-max" && next && *next == LEVEL_LIGHTS_CHEAP_MAX) q.level_lights_cheap = true, value = true;
        else if (a == "--lamp-shadows" || a == "--no-lamp-shadows") q.lamp_shadows = a == "--lamp-shadows";
        else if (a == "--world-shadows" || a == "--no-world-shadows") q.world_shadows = a == "--world-shadows";
        else if (a == "--world-shadows-size" && next && (*next == "1024" || *next == "2048")) q.world_shadows_low = *next == "1024", value = true;
        else if (a == "--light-shafts" || a == "--no-light-shafts") q.light_shafts = a == "--light-shafts";
        else if (a == "--light-shafts-cheap" || a == "--no-light-shafts-cheap") q.light_shafts_cheap = a == "--light-shafts-cheap";
        else if (a == "--volfog" || a == "--no-volfog") q.volfog = a == "--volfog";
        else if (a == "--volfog-quality" && next && (*next == "low" || *next == "medium" || *next == "high"))
            q.volfog_quality = *next == "high" ? "" : *next, value = true;
        else if (a == "--soft-particles" || a == "--no-soft-particles") q.soft_particles = a == "--soft-particles";
        else if (a == "--fps-overlay" || a == "--no-fps-overlay") q.fps_overlay = a == "--fps-overlay";
        else if (a == "--fps-overlay-items" && next && parse_fps_overlay_items(*next) > 0)
            set_fps_overlay_mask(q, parse_fps_overlay_items(*next)), value = true;
        else if (a == "--fps-overlay-corner" && next && (*next == "tl" || *next == "tr" || *next == "bl" || *next == "br"))
            q.fps_overlay_corner = *next, value = true;
        else if (a == "--filmic" || a == "--no-filmic") q.filmic = a == "--filmic";
        else if (a == "--auto-exposure" || a == "--no-auto-exposure") q.auto_exposure = a == "--auto-exposure";
        else if (a == "--cutscene-dof" || a == "--no-cutscene-dof") q.cutscene_dof = a == "--cutscene-dof";
        else if (a == "--taa" || a == "--no-taa") q.taa = a == "--taa";
        else if (a == "--frame-gen" || a == "--no-frame-gen") q.frame_gen = a == "--frame-gen";
        else if (a == "--anim-interp" || a == "--no-anim-interp") q.anim_interp = a == "--anim-interp";
        else if (a == "--sky-hd" || a == "--no-sky-hd") q.sky_hd = a == "--sky-hd";
        else if (a == "--sharp-fonts" || a == "--no-sharp-fonts") q.sharp_fonts = a == "--sharp-fonts";
        else if (a == "--sharp-faces" || a == "--no-sharp-faces") q.sharp_faces = a == "--sharp-faces";
        else if (a == "--sharp-faces-over-pack" || a == "--no-sharp-faces-over-pack") q.sharp_faces_over_pack = a == "--sharp-faces-over-pack";
        else if (a == "--skip-intro" || a == "--no-skip-intro") q.skip_intro = a == "--skip-intro";
        else if (a == "--smart-allies" || a == "--no-smart-allies") q.smart_allies = a == "--smart-allies";
        else if (a == "--enemy-tactics" || a == "--no-enemy-tactics") q.enemy_tactics = a == "--enemy-tactics";
        else if (a == "--spray-time" && next && int_arg(*next, SPRAY_TIME_MIN, SPRAY_TIME_MAX, v)) q.spray_time = v, value = true;
        else if (a == "--dialogue-volume" && next && int_arg(*next, AUDIO_VOLUME_MIN_DB, AUDIO_VOLUME_MAX_DB, v)) q.dialogue_volume = v, value = true;
        else if (a == "--music-volume" && next && int_arg(*next, AUDIO_VOLUME_MIN_DB, AUDIO_VOLUME_MAX_DB, v)) q.music_volume = v, value = true;
        else if (a == "--sfx-volume" && next && int_arg(*next, AUDIO_VOLUME_MIN_DB, AUDIO_VOLUME_MAX_DB, v)) q.sfx_volume = v, value = true;
        else if (a == "--audio-hrtf" || a == "--no-audio-hrtf") q.audio_hrtf = a == "--audio-hrtf";
        else if (a == "--audio-air" || a == "--no-audio-air") q.audio_air = a == "--audio-air";
        else if (a == "--audio-occlusion" || a == "--no-audio-occlusion") q.audio_occlusion = a == "--audio-occlusion";
        else if (a == "--audio-reverb-geo" || a == "--no-audio-reverb-geo") q.audio_reverb_geo = a == "--audio-reverb-geo";
        else if (a == "--audio-pathing" || a == "--no-audio-pathing") q.audio_pathing = a == "--audio-pathing";
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
