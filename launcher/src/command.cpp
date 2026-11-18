#include "command.h"
#include "platform.h"

namespace launcher {

/* runtime/src/hle_d3d.h HLE_D3D_MAX_PACKS: one more --texture-pack stops the game at startup. */
static const int RUNTIME_MAX_TEXTURE_PACKS = 8;

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
    if (p.neon_glow) opt_str(a, "--texture-pack", p.neon_masks); /* mask-only pack after it (its masks only) */
    opt_str(a, "--sound-pack", p.sound_pack);
    if (!p.resolution.empty()) opt_str(a, "--resolution", p.resolution);
    else opt_int(a, "--render-scale", p.render_scale, 1);
    opt_flag(a, "--widescreen", p.widescreen);
    opt_flag(a, "--no-movie-hd", !p.movie_hd);
    opt_flag(a, "--no-gamma-ramp", !p.gamma_ramp);
    opt_flag(a, "--fullscreen", p.fullscreen);
    opt_flag(a, "--no-refresh-match", !p.refresh_match);
    opt_int(a, "--msaa", p.msaa, 1);
    opt_flag(a, "--taa", p.taa);
    opt_flag(a, "--frame-gen", p.frame_gen);
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
    if (!p.mods.empty()) {   /* the folder only matters when a mod runs */
        opt_str(a, "--mods", p.mods_dir);
        opt_str(a, "--mod", p.mods);
    }
    opt_str(a, "--audio", p.audio, "sdl");
    opt_str(a, "--video", p.video, "sdl");
    opt_str(a, "--renderer", p.renderer, "vulkan");
    opt_flag(a, "--gpu-vertex-programs", p.gpu_vertex_programs == "on");
    opt_flag(a, "--no-gpu-vertex-programs", p.gpu_vertex_programs == "off");
    opt_flag(a, "--no-render-targets", !p.render_targets);
    opt_flag(a, "--stats", p.stats);
    opt_flag(a, "--load-profile", p.load_profile);
    opt_flag(a, "--trace", p.trace);
    opt_flag(a, "--sim60", p.sim60);
    opt_flag(a, "--anim-interp", p.anim_interp);
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
    opt_str(a, "--fsr", p.fsr, "off");
    opt_str(a, "--taa-upscale", p.taa_upscale, "off");
    if (p.taa || p.taa_upscale != "off") opt_float(a, "--taa-sharpness", p.taa_sharpness);
    if (p.dynres && p.taa_upscale != "off") a.push_back("--dynres"), a.push_back("auto");   /* it varies the temporal upscaling's size */
    opt_int(a, "--shadow-scale", p.shadow_scale, 1);
    opt_flag(a, "--shadow-soft", p.shadow_soft);
    opt_flag(a, "--shadow-range", p.shadow_range);
    opt_int(a, "--frames-in-flight", p.frames_in_flight, 0);
    if (p.sharpen > 0) opt_int(a, "--sharpen", p.sharpen > 100 ? 100 : p.sharpen, 0);
    opt_flag(a, "--bloom", p.bloom);
    if (p.bloom) opt_float(a, "--bloom-strength", p.bloom_strength);
    opt_flag(a, "--rain-hd", p.rain_hd);
    if (p.rain_hd) opt_flag(a, "--rain-hd-puddles", p.rain_puddles);
    if (p.rain_hd) opt_flag(a, "--wet-light-streaks", p.wet_light_streaks);
    if (p.rain_hd) opt_flag(a, "--materials", p.materials);
    opt_flag(a, "--damp-streets", p.damp_streets);
    if ((p.rain_hd || p.damp_streets) && p.reflections == "sssr") opt_str(a, "--reflections", p.reflections, "classic");   /* classic: the default, unknown: left out */
    opt_flag(a, "--ssao", p.ssao);
    if (p.ssao) opt_float(a, "--ssao-strength", p.ssao_strength);
    if (p.ssao) opt_str(a, "--ssao-mode", p.ssao_mode, "classic");
    if (p.ssao && p.ssao_mode == "gtao") opt_str(a, "--gtao-quality", p.gtao_quality, "high");
    opt_flag(a, "--contact-shadows", p.contact_shadows);
    opt_flag(a, "--lamp-lights", p.lamp_lights);
    if (p.lamp_lights) opt_float(a, "--lamp-lights-strength", p.lamp_lights_strength);
    opt_flag(a, "--neon-glow", p.neon_glow);
    if (p.neon_glow) opt_float(a, "--neon-glow-strength", p.neon_glow_strength);
    if (p.neon_glow) opt_flag(a, "--neon-reflections", p.neon_reflections);
    opt_flag(a, "--char-lighting", p.char_lighting);
    if (p.char_lighting) opt_flag(a, "--hair", p.hair); /* shades the lit character draws: needs them */
    if (p.char_lighting) opt_flag(a, "--skin", p.skin); /* likewise */
    if (p.char_lighting) opt_flag(a, "--eyes", p.eyes); /* likewise */
    if (p.char_lighting) opt_flag(a, "--cloth", p.cloth); /* likewise */
    opt_flag(a, "--world-lighting", p.world_lighting);
    opt_flag(a, "--normal-maps", p.normal_maps); /* its pack: add_relief_pack at launch */
    opt_flag(a, "--dry-highlights", p.dry_highlights);
    opt_flag(a, "--parallax", p.parallax);
    opt_flag(a, "--fire-light", p.fire_light);
    opt_flag(a, "--level-lights", p.level_lights);
    if (p.level_lights && p.level_lights_cheap) {
        a.push_back("--level-lights-max");
        a.push_back(LEVEL_LIGHTS_CHEAP_MAX);
    }
    opt_flag(a, "--lamp-shadows", p.lamp_shadows);
    opt_flag(a, "--world-shadows", p.world_shadows);
    if (p.world_shadows && p.world_shadows_low) {
        a.push_back("--world-shadows-size");
        a.push_back("1024");
    }
    opt_flag(a, "--light-shafts", p.light_shafts);
    if (p.light_shafts) opt_flag(a, "--light-shafts-cheap", p.light_shafts_cheap);
    opt_flag(a, "--volfog", p.volfog);
    if (p.volfog) {   /* the runtime's default is medium: the launcher's is high */
        a.push_back("--volfog-quality");
        a.push_back(p.volfog_quality == "low" || p.volfog_quality == "medium" ? p.volfog_quality : "high");
    }
    opt_flag(a, "--soft-particles", p.soft_particles);
    opt_flag(a, "--fps-overlay", p.fps_overlay);
    if (p.fps_overlay && fps_overlay_mask(p) != (FPS_OVERLAY_FPS | FPS_OVERLAY_MS)) {   /* fps + ms: the runtime's default */
        Profile q;
        set_fps_overlay_mask(q, fps_overlay_mask(p));
        a.push_back("--fps-overlay-items");
        a.push_back(q.fps_overlay_items);
    }
    if (p.fps_overlay && !p.fps_overlay_corner.empty() && p.fps_overlay_corner != "tl") {
        a.push_back("--fps-overlay-corner");
        a.push_back(p.fps_overlay_corner);
    }
    opt_flag(a, "--filmic", p.filmic);
    opt_flag(a, "--auto-exposure", p.auto_exposure);
    opt_flag(a, "--cutscene-dof", p.cutscene_dof);
    opt_flag(a, "--sky-hd", p.sky_hd);
    opt_flag(a, "--no-sharp-fonts", !p.sharp_fonts);
    opt_flag(a, "--sharp-faces", p.sharp_faces);
    opt_flag(a, "--sharp-faces-over-pack", p.sharp_faces_over_pack && sharp_faces_over_pack_offered(p));
    draw_distance_args(a, p.draw_distance);
    opt_int(a, "--fx-scale", p.fx_scale, 1);
    if (p.spray_time != SPRAY_TIME_DEFAULT) opt_int(a, "--spray-time", p.spray_time, 0);
    const struct { const char *flag; int db; } volumes[] = {
        {"--dialogue-volume", p.dialogue_volume}, {"--music-volume", p.music_volume}, {"--sfx-volume", p.sfx_volume}};
    for (const auto &v : volumes)
        if (v.db) {
            a.push_back(v.flag);
            a.push_back(std::to_string(v.db < AUDIO_VOLUME_MIN_DB ? AUDIO_VOLUME_MIN_DB : v.db > AUDIO_VOLUME_MAX_DB ? AUDIO_VOLUME_MAX_DB : v.db));
        }
    opt_flag(a, "--audio-hrtf", p.audio_hrtf);
    opt_flag(a, "--no-audio-air", !p.audio_air);
    opt_flag(a, "--no-audio-occlusion", !p.audio_occlusion);
    opt_flag(a, "--no-audio-reverb-geo", !p.audio_reverb_geo);
    opt_flag(a, "--no-audio-pathing", !p.audio_pathing);
    opt_flag(a, "--no-text-fixes", !p.text_fixes);
    opt_flag(a, "--skip-intro", p.skip_intro);
    opt_flag(a, "--smart-allies", p.smart_allies);
    opt_flag(a, "--enemy-tactics", p.enemy_tactics);
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

const char *const RELIEF_PACK_DIR = "relief-pack";

void add_relief_pack(std::vector<std::string> &argv, const std::string &dir, bool found) {
    size_t flag = argv.size();
    int packs = 0;
    bool listed = false;
    const std::string want = strip_separators(dir);
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i] == "--normal-maps") flag = i;
        if (argv[i] == "--texture-pack" && i + 1 < argv.size()) {
            const std::string v = strip_separators(argv[i + 1]);
            listed |= v == want || v == RELIEF_PACK_DIR; /* the plain name: relative to the game's folder */
            packs++;
        }
    }
    if (flag == argv.size()) return; /* the toggle is off */
    if (!found && !listed) {
        argv.erase(argv.begin() + (long)flag); /* no companion maps to use: the flag would do nothing */
        return;
    }
    if (listed || packs >= RUNTIME_MAX_TEXTURE_PACKS) return;
    argv.push_back("--texture-pack");
    argv.push_back(dir);
}

void add_auto_gpu_vertex_programs(std::vector<std::string> &argv, const Profile &p, const GpuDetection &gpu) {
    if (p.gpu_vertex_programs == "on" || p.gpu_vertex_programs == "off" || gpu.tier != "t0" || p.fsr != "off" || p.taa_upscale != "off") return;
    for (const std::string &x : argv)
        if (x == "--gpu-vertex-programs" || x == "--no-gpu-vertex-programs") return;
    argv.push_back("--no-gpu-vertex-programs");
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

void add_bench_args(std::vector<std::string> &argv, const std::string &report) {
    argv.push_back("--bench");
    argv.push_back("--bench-out");
    argv.push_back(report);
}

std::string bench_report_name(const std::string &profile, const std::string &stamp) {
    std::string s;
    for (char c : profile) s += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return "bench-" + (s.empty() ? std::string("profile") : s) + "-" + stamp + ".txt";
}

} // namespace launcher
