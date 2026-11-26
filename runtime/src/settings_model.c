/* The settings model: see settings_model.h. The rows follow launcher/src/command.cpp build_argv's
 * order, so sm_build_argv walks them once; the classes are the plan's Inventory
 *. */
#include "settings_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D SM_TAB_DISPLAY
#define E SM_TAB_EFFECTS
#define A SM_TAB_AUDIO
#define C SM_TAB_CONTROLS
#define G SM_TAB_GAME
#define M SM_TAB_MODS
#define N SM_TAB_NONE
#define MP (SM_MENU | SM_PRESET)

/* key, type, default, domain, class, tab, flags, menu prerequisite, label, description, arg kind,
 * flag, param, arg prerequisite */
#define ROW(key, type, dflt, dom, cls, tab, flags, req, label, desc, arg, flag, param, areq) \
    {key, type, dflt, dom, cls, tab, flags, req, label, desc, arg, flag, param, areq}
/* A bool flag that the command line gates the same way as the menu. */
#define ON(key, dflt, cls, tab, flags, req, label, desc, flag) \
    ROW(key, SM_BOOL, dflt, "", cls, tab, flags, req, label, desc, SM_ARG_ON, flag, "", req)
#define OFF(key, cls, tab, flags, label, desc, flag) \
    ROW(key, SM_BOOL, "true", "", cls, tab, flags, "", label, desc, SM_ARG_OFF, flag, "", "")
#define FLT(key, dom, cls, tab, flags, req, label, desc, flag, areq) \
    ROW(key, SM_FLOAT, "-1", dom, cls, tab, flags, req, label, desc, SM_ARG_FLOAT, flag, "", areq)

static const SmSetting rows[] = {
    ROW("xbrun", SM_PATH, "", "", SM_BOOT, N, 0, "", "game_program", "exe_desc", SM_ARG_HEAD, "", "", ""),
    ROW("disc", SM_PATH, "", "", SM_BOOT, N, 0, "", "game_folder", "game_folder_desc", SM_ARG_HEAD, "", "", ""),
    ROW("hdd", SM_PATH, "", "", SM_BOOT, N, 0, "", "save_folder", "save_folder_desc", SM_ARG_VALUE, "--hdd", "", ""),
    ROW("icon_pack", SM_PATH, "", "", SM_RESTART, N, 0, "", "icon_pack", "icon_pack_hint", SM_ARG_VALUE, "--texture-pack", "", ""),
    ROW("glyph_packs", SM_PATH, "", "", SM_RESTART, N, 0, "", "glyph_pack", "glyph_pack_hint", SM_ARG_GLYPHS, "--texture-pack", "", ""),
    ROW("texture_pack", SM_PATH, "", "", SM_RESTART, D, SM_MENU, "", "texture_pack", NULL, SM_ARG_VALUE, "--texture-pack", "", ""),
    ROW("neon_masks", SM_PATH, "", "", SM_RESTART, N, 0, "", NULL, NULL, SM_ARG_VALUE, "--texture-pack", "", "neon_glow"),
    ROW("sound_pack", SM_PATH, "", "", SM_RESTART, A, SM_MENU, "", "sound_pack", NULL, SM_ARG_VALUE, "--sound-pack", "", ""),
    ROW("resolution", SM_STR, "", "", SM_REINIT, D, MP, "", "resolution", "res_desc", SM_ARG_VALUE, "--resolution", "", ""),
    ROW("render_scale", SM_INT, "1", "1..8", SM_REINIT, D, SM_MENU, "resolution=", "render_scale", "render_scale_desc", SM_ARG_INT,
        "--render-scale", "1", "resolution="),
    ON("widescreen", "false", SM_RESTART, D, MP, "", "widescreen", "widescreen_desc", "--widescreen"),
    OFF("movie_hd", SM_PASS, D, SM_MENU, "movie_hd", "movie_hd_desc", "--no-movie-hd"),
    OFF("gamma_ramp", SM_PASS, D, SM_MENU, "gamma_ramp", "gamma_ramp_desc", "--no-gamma-ramp"),
    ON("fullscreen", "false", SM_PASS, D, MP, "", "window_mode", "window_mode_desc", "--fullscreen"),
    ROW("refresh_match", SM_BOOL, "true", "", SM_PASS, D, SM_MENU, "fullscreen", "refresh_match", "refresh_match_desc", SM_ARG_OFF,
        "--no-refresh-match", "", ""),
    ROW("msaa", SM_INT, "0", "0|2|4|8", SM_REINIT, D, MP, "", "msaa", "msaa_desc", SM_ARG_INT, "--msaa", "1", ""),
    ON("taa", "false", SM_REINIT, D, MP | SM_DEPTH, "", "taa", "taa_desc", "--taa"),
    ON("frame_gen", "false", SM_REINIT, D, SM_MENU | SM_DEPTH, "", "frame_gen", "frame_gen_desc", "--frame-gen"),
    ROW("aniso", SM_INT, "0", "0|2|4|8|16", SM_REINIT, D, MP, "", "texfilter", "texfilter_desc", SM_ARG_INT, "--aniso", "1", ""),
    ROW("present", SM_ENUM, "auto", "auto|mailbox|fifo|immediate|readback", SM_PASS, D, SM_MENU, "", "present_mode", "present_mode_desc",
        SM_ARG_VALUE, "--present", "auto", ""),
    ROW("language", SM_ENUM, "en", "en|es|fr|de|it", SM_RESTART, D, SM_MENU, "", "game_language", "game_language_desc", SM_ARG_VALUE,
        "--language", "en", ""),
    ROW("spawn_scale", SM_INT, "1", "1..8", SM_RESTART, G, SM_MENU, "", "spawn_scale", "spawn_scale_desc", SM_ARG_INT, "--spawn-scale",
        "1", ""),
    OFF("rate_fixes", SM_RESTART, G, SM_MENU, "rate_fixes", "rate_fixes_desc", "--no-rate-fixes"),
    OFF("frame_lock", SM_RESTART, G, SM_MENU, "frame_lock", "frame_lock_desc", "--no-frame-lock"),
    ROW("input_config", SM_PATH, "", "", SM_PASS, C, 0, "", "key_map", "key_map_desc", SM_ARG_VALUE, "--input-config", "", ""),
    ROW("button_icons", SM_ENUM, "xbox", "xbox|ps-color|ps-black|ps-white|kbm|auto", SM_RESTART, D, SM_MENU, "", "button_icons",
        "button_icons_desc", SM_ARG_ICONS, "--button-icons", "kbm|auto", ""),
    OFF("rumble", SM_LIVE, C, SM_MENU, "vibration", "vibration_desc", "--no-rumble"),
    ROW("warp", SM_STR, "", "", SM_BOOT, N, 0, "", "warp", "warp_desc", SM_ARG_VALUE, "--warp", "", ""),
    ROW("mods_dir", SM_PATH, "", "", SM_RESTART, N, 0, "mods!=", "mods_folder", "mods_folder_desc", SM_ARG_VALUE, "--mods", "", "mods!="),
    ROW("mods", SM_STR, "", "", SM_RESTART, M, SM_MENU, "", "mods_list", NULL, SM_ARG_VALUE, "--mod", "", ""),
    ROW("audio", SM_ENUM, "sdl", "sdl|null", SM_RESTART, N, 0, "", "audio", "audio_desc", SM_ARG_VALUE, "--audio", "sdl", ""),
    ROW("video", SM_ENUM, "sdl", "sdl|x11|null", SM_RESTART, N, 0, "", "video", "video_desc", SM_ARG_VALUE, "--video", "sdl", ""),
    ROW("renderer", SM_ENUM, "vulkan", "vulkan|none", SM_RESTART, N, 0, "", "renderer", "renderer_desc", SM_ARG_VALUE, "--renderer",
        "vulkan", ""),
    ROW("gpu_vertex_programs", SM_ENUM, "auto", "auto|on|off", SM_RESTART, N, 0, "", NULL, NULL, SM_ARG_MAP, "",
        "on=--gpu-vertex-programs|off=--no-gpu-vertex-programs", ""),
    OFF("render_targets", SM_RESTART, N, 0, "render_targets", "render_targets_desc", "--no-render-targets"),
    ROW("render64", SM_BOOL, "false", "", SM_RESTART, N, 0, "", "render64", "render64_desc", SM_ARG_ON, "--render-process", "helper", ""),
    ON("stats", "false", SM_RESTART, N, 0, "", "frame_stats", "frame_stats_desc", "--stats"),
    ON("load_profile", "false", SM_RESTART, N, 0, "", "load_profile", "load_profile_desc", "--load-profile"),
    ON("trace", "false", SM_RESTART, N, 0, "", "trace_log", "trace_log_desc", "--trace"),
    ON("sim60", "false", SM_RESTART, G, MP, "", "frame_rate", "frame_rate_60_desc", "--sim60"),
    ON("anim_interp", "false", SM_LIVE, E, MP, "", "anim_interp", "anim_interp_desc", "--anim-interp"),
    ROW("hud_style", SM_ENUM, "original", "original|hd|hd-glass|glass", SM_PASS, D, MP | SM_COLOR, "", "health_bars", "health_bars_desc",
        SM_ARG_MAP, "", "hd=--hud-hd|hd-glass=--hud-hd-glass|glass=--hud-glass", ""),
    FLT("hud_track_opacity", "0..1", SM_LIVE, D, MP, "hud_style=hd/hd-glass", "empty_opacity", "empty_opacity_desc", "--hud-hd-track-opacity",
        "hud_style=hd/hd-glass"),
    FLT("hud_fill_opacity", "0..1", SM_LIVE, D, MP, "hud_style=hd/hd-glass", "fill_opacity", "fill_opacity_desc", "--hud-hd-fill-opacity",
        "hud_style=hd/hd-glass"),
    FLT("hud_frost", "0..32", SM_LIVE, D, MP, "hud_style=hd/hd-glass", "frost", "frost_desc", "--hud-hd-frost", "hud_style=hd/hd-glass"),
    FLT("hud_height", "0.5..4", SM_LIVE, D, MP, "hud_style=hd/hd-glass", "height", "height_desc", "--hud-hd-height", "hud_style=hd/hd-glass"),
    ROW("supersample", SM_INT, "1", "1|2|4", SM_REINIT, D, MP, "", "supersampling", "supersampling_desc", SM_ARG_INT, "--supersample", "1", ""),
    ROW("fsr", SM_ENUM, "off", "off|ultra|quality|balanced|performance", SM_REINIT, D, MP, "", "fsr", "fsr_desc", SM_ARG_VALUE, "--fsr",
        "off", ""),
    ROW("taa_upscale", SM_ENUM, "off", "off|native|ultra|quality|balanced|performance", SM_REINIT, D, MP | SM_DEPTH, "", "taa_upscale",
        "taa_upscale_desc", SM_ARG_VALUE, "--taa-upscale", "off", ""),
    ROW("upscaler", SM_ENUM, "taa", "taa|fsr3", SM_REINIT, D, MP, "taa_upscale!=off", "upscaler", "upscaler_desc", SM_ARG_VALUE,
        "--upscaler", "taa", "taa_upscale!=off"),
    FLT("taa_sharpness", "0..1", SM_LIVE, D, MP, "taa|taa_upscale!=off", "taa_sharpness", "taa_sharpness_desc", "--taa-sharpness",
        "taa|taa_upscale!=off"),
    ROW("dynres", SM_BOOL, "false", "", SM_LIVE, D, MP, "taa_upscale!=off", "dynres", "dynres_desc", SM_ARG_ON, "--dynres", "auto",
        "taa_upscale!=off"),
    ROW("vrs", SM_BOOL, "false", "", SM_REINIT, D, MP, "", "vrs", "vrs_desc", SM_ARG_ON, "--vrs", "auto", ""),
    /* the final grade's saturation and the HDR10 output */
    ROW("saturation", SM_FLOAT, "1", "0.5..2", SM_LIVE, D, SM_MENU, "", "saturation", "saturation_desc", SM_ARG_FLOAT_NE, "--saturation", "", ""),
    ROW("hdr", SM_ENUM, "off", "off|auto|on", SM_RESTART, D, SM_MENU, "", "hdr", "hdr_desc", SM_ARG_VALUE, "--hdr", "off", ""),
    ROW("hdr_peak", SM_INT, "1000", "400..10000", SM_LIVE, D, SM_MENU, "hdr!=off", "hdr_peak", "hdr_peak_desc", SM_ARG_INT_NE, "--hdr-peak", "",
        "hdr!=off"),
    ROW("hdr_paper_white", SM_INT, "200", "80..500", SM_LIVE, D, SM_MENU, "hdr!=off", "hdr_paper_white", "hdr_paper_white_desc", SM_ARG_INT_NE,
        "--hdr-paper-white", "", "hdr!=off"),
    ROW("shadow_scale", SM_INT, "1", "1..4", SM_PASS, E, MP, "", "shadow_quality", "shadow_quality_desc", SM_ARG_INT, "--shadow-scale", "1", ""),
    ON("shadow_soft", "false", SM_PASS, E, MP, "", "soft_shadows", "soft_shadows_desc", "--shadow-soft"),
    ON("shadow_range", "false", SM_PASS, E, MP, "", "shadow_range", "shadow_range_desc", "--shadow-range"),
    ROW("frames_in_flight", SM_INT, "0", "0..3", SM_REINIT, D, MP, "", "fif", "fif_desc", SM_ARG_INT, "--frames-in-flight", "0", ""),
    ROW("sharpen", SM_INT, "0", "0..100", SM_PASS, D, SM_MENU, "", "sharpness", "sharpness_desc", SM_ARG_INT_CLAMP, "--sharpen", "0", ""),
    ON("bloom", "false", SM_PASS, E, MP | SM_COLOR, "", "bloom", "bloom_desc", "--bloom"),
    FLT("bloom_strength", "0.1..3", SM_PASS, E, MP, "bloom", "bloom_strength", "bloom_strength_desc", "--bloom-strength", "bloom"),
    ON("rain_hd", "false", SM_PASS, E, MP | SM_DEPTH, "", "rain_hd", "rain_hd_desc", "--rain-hd"),
    ON("rain_puddles", "false", SM_PASS, E, MP, "rain_hd", "rain_puddles", "rain_puddles_desc", "--rain-hd-puddles"),
    ON("wet_light_streaks", "false", SM_PASS, E, MP, "rain_hd", "wet_light_streaks", "wet_light_streaks_desc", "--wet-light-streaks"),
    ON("materials", "false", SM_PASS, E, MP, "rain_hd", "materials", "materials_desc", "--materials"),
    ON("damp_streets", "false", SM_PASS, E, MP | SM_DEPTH, "", "damp_streets", "damp_streets_desc", "--damp-streets"),
    ROW("reflections", SM_ENUM, "classic", "classic|sssr", SM_PASS, E, MP, "rain_hd|damp_streets", "reflections", "reflections_desc",
        SM_ARG_LISTED, "--reflections", "sssr", "rain_hd|damp_streets"),
    ON("ssao", "false", SM_PASS, E, MP | SM_DEPTH, "", "ssao", "ssao_desc", "--ssao"),
    FLT("ssao_strength", "0.1..2", SM_PASS, E, MP, "ssao", "ssao_strength", "ssao_strength_desc", "--ssao-strength", "ssao"),
    ROW("ssao_mode", SM_ENUM, "classic", "classic|gtao", SM_PASS, E, MP, "ssao", "ssao_mode", "ssao_mode_desc", SM_ARG_VALUE,
        "--ssao-mode", "classic", "ssao"),
    ROW("gtao_quality", SM_ENUM, "high", "low|medium|high", SM_PASS, E, MP, "ssao&ssao_mode=gtao", "gtao_quality", "gtao_quality_desc",
        SM_ARG_VALUE, "--gtao-quality", "high", "ssao&ssao_mode=gtao"),
    /* bounce lighting rides on GTAO's passes: the switch passes no flag of its own, its quality is
     * --ssgi's value (always written, an unknown one left out like build_argv) */
    ROW("ssgi", SM_BOOL, "false", "", SM_PASS, E, MP | SM_DEPTH, "ssao&ssao_mode=gtao", "ssgi", "ssgi_desc", SM_ARG_NONE, "", "", ""),
    ROW("ssgi_quality", SM_ENUM, "medium", "low|medium|high", SM_PASS, E, MP, "ssgi&ssao&ssao_mode=gtao", "ssgi_quality",
        "ssgi_quality_desc", SM_ARG_LISTED, "--ssgi", "low|medium|high", "ssgi&ssao&ssao_mode=gtao"),
    FLT("ssgi_strength", "0.1..3", SM_PASS, E, MP, "ssgi&ssao&ssao_mode=gtao", "ssgi_strength", "ssgi_strength_desc", "--ssgi-strength",
        "ssgi&ssao&ssao_mode=gtao"),
    ON("contact_shadows", "false", SM_PASS, E, MP | SM_DEPTH, "", "contact_shadows", "contact_shadows_desc", "--contact-shadows"),
    ON("lamp_lights", "false", SM_PASS, E, MP | SM_DEPTH, "", "lamp_lights", "lamp_lights_desc", "--lamp-lights"),
    FLT("lamp_lights_strength", "0.1..3", SM_PASS, E, MP, "lamp_lights", "lamp_lights_strength", "lamp_lights_strength_desc",
        "--lamp-lights-strength", "lamp_lights"),
    ON("neon_glow", "false", SM_PASS, E, MP | SM_BOOT_PACK, "", "neon_glow", "neon_glow_desc", "--neon-glow"),
    FLT("neon_glow_strength", "0.1..3", SM_LIVE, E, MP, "neon_glow", "neon_glow_strength", "neon_glow_strength_desc", "--neon-glow-strength",
        "neon_glow"),
    ON("neon_reflections", "false", SM_PASS, E, MP, "neon_glow", "neon_reflections", "neon_reflections_desc", "--neon-reflections"),
    ON("char_lighting", "false", SM_PASS, E, MP, "", "char_lighting", "char_lighting_desc", "--char-lighting"),
    ON("hair", "false", SM_PASS, E, MP, "char_lighting", "hair", "hair_desc", "--hair"),
    ON("skin", "false", SM_PASS, E, MP, "char_lighting", "skin", "skin_desc", "--skin"),
    ON("eyes", "false", SM_PASS, E, MP, "char_lighting", "eyes", "eyes_desc", "--eyes"),
    ON("cloth", "false", SM_PASS, E, MP, "char_lighting", "cloth", "cloth_desc", "--cloth"),
    ON("world_lighting", "false", SM_PASS, E, MP, "", "world_lighting", "world_lighting_desc", "--world-lighting"),
    ON("normal_maps", "false", SM_RESTART, E, MP, "", "normal_maps", "normal_maps_desc", "--normal-maps"),
    /* shown under the relief only, but the command line passes them on their own */
    ROW("dry_highlights", SM_BOOL, "false", "", SM_PASS, E, MP, "normal_maps", "dry_highlights", "dry_highlights_desc", SM_ARG_ON,
        "--dry-highlights", "", ""),
    ROW("parallax", SM_BOOL, "false", "", SM_LIVE, E, MP, "normal_maps", "parallax", "parallax_desc", SM_ARG_ON, "--parallax", "", ""),
    ON("fire_light", "false", SM_PASS, E, MP, "", "fire_light", "fire_light_desc", "--fire-light"),
    ON("level_lights", "false", SM_PASS, E, MP, "", "level_lights", "level_lights_desc", "--level-lights"),
    ROW("level_lights_cheap", SM_BOOL, "false", "", SM_PASS, E, MP, "level_lights", "level_lights_cheap", "level_lights_cheap_desc",
        SM_ARG_ON, "--level-lights-max", "12", "level_lights"),   /* profile.h LEVEL_LIGHTS_CHEAP_MAX */
    ON("lamp_shadows", "false", SM_PASS, E, MP, "", "lamp_shadows", "lamp_shadows_desc", "--lamp-shadows"),
    ON("world_shadows", "false", SM_PASS, E, MP, "", "world_shadows", "world_shadows_desc", "--world-shadows"),
    ROW("world_shadows_low", SM_BOOL, "false", "", SM_PASS, E, MP, "world_shadows", "world_shadows_low", "world_shadows_low_desc",
        SM_ARG_ON, "--world-shadows-size", "1024", "world_shadows"),
    ON("light_shafts", "false", SM_PASS, E, MP | SM_DEPTH, "", "light_shafts", "light_shafts_desc", "--light-shafts"),
    ON("light_shafts_cheap", "false", SM_PASS, E, SM_MENU, "light_shafts", "light_shafts_cheap", "light_shafts_cheap_desc",
       "--light-shafts-cheap"),
    ON("volfog", "false", SM_PASS, E, MP | SM_DEPTH, "", "volfog", "volfog_desc", "--volfog"),
    ROW("volfog_quality", SM_ENUM, "", "|low|medium|high", SM_PASS, E, MP, "volfog", "volfog_quality", "volfog_quality_desc", SM_ARG_ALWAYS,
        "--volfog-quality", "low|medium|high", "volfog"),   /* empty = high: the launcher's default, not the runtime's */
    ON("soft_particles_opt_in", "false", SM_PASS, E, MP | SM_DEPTH, "", "soft_particles", "soft_particles_desc", "--soft-particles"),
    ON("fps_overlay", "false", SM_PASS, D, MP, "", "perf_overlay_show", "perf_overlay_show_desc", "--fps-overlay"),
    ROW("fps_overlay_items", SM_STR, "", "", SM_PASS, D, SM_MENU, "fps_overlay", "perf_overlay_section", NULL, SM_ARG_FPS_ITEMS,
        "--fps-overlay-items", "", "fps_overlay"),
    ROW("fps_overlay_corner", SM_ENUM, "", "|tl|tr|bl|br", SM_PASS, D, SM_MENU, "fps_overlay", "perf_overlay_corner", NULL, SM_ARG_VALUE,
        "--fps-overlay-corner", "tl", "fps_overlay"),
    ON("filmic", "false", SM_PASS, E, MP, "", "filmic", "filmic_desc", "--filmic"),
    ON("auto_exposure", "false", SM_PASS, E, MP, "", "auto_exposure", "auto_exposure_desc", "--auto-exposure"),
    ON("cutscene_dof", "false", SM_PASS, E, MP | SM_DEPTH, "", "cutscene_dof", "cutscene_dof_desc", "--cutscene-dof"),
    ON("sky_hd", "false", SM_PASS, E, MP, "", "sky_hd", "sky_hd_desc", "--sky-hd"),
    OFF("sharp_fonts", SM_PASS, D, MP, "sharp_fonts", "sharp_fonts_desc", "--no-sharp-fonts"),
    ON("sharp_faces", "false", SM_PASS, D, MP, "", "sharp_faces", "sharp_faces_desc", "--sharp-faces"),
    ON("sharp_faces_over_pack", "false", SM_PASS, D, MP, "sharp_faces&texture_pack!=", "sharp_faces_over_pack", "sharp_faces_over_pack_desc",
       "--sharp-faces-over-pack"),
    ROW("draw_distance", SM_INT, "1", "1..4", SM_PASS, D, MP, "", "draw_distance", "draw_distance_desc", SM_ARG_DRAW_DISTANCE, "", "", ""),
    ROW("fx_scale", SM_INT, "1", "1..4", SM_RESTART, E, MP, "", "fx_scale", "fx_scale_desc", SM_ARG_INT, "--fx-scale", "1", ""),
    ROW("spray_time", SM_INT, "110", "100..200", SM_PASS, G, SM_MENU, "", "spray", "spray_desc", SM_ARG_INT_NE, "--spray-time", "", ""),
    ROW("dialogue_volume", SM_INT, "0", "-40..12", SM_LIVE, A, SM_MENU, "", "dialogue_volume", "dialogue_volume_desc", SM_ARG_VOLUME,
        "--dialogue-volume", "", ""),
    ROW("music_volume", SM_INT, "0", "-40..12", SM_LIVE, A, SM_MENU, "", "music_volume", "music_volume_desc", SM_ARG_VOLUME,
        "--music-volume", "", ""),
    ROW("sfx_volume", SM_INT, "0", "-40..12", SM_LIVE, A, SM_MENU, "", "sfx_volume", "sfx_volume_desc", SM_ARG_VOLUME, "--sfx-volume", "", ""),
    ON("audio_hrtf", "false", SM_PASS, A, SM_MENU, "", "audio_hrtf", "audio_hrtf_desc", "--audio-hrtf"),
    OFF("audio_air", SM_LIVE, A, SM_MENU, "audio_air", "audio_air_desc", "--no-audio-air"),
    OFF("audio_occlusion", SM_LIVE, A, SM_MENU, "audio_occlusion", "audio_occlusion_desc", "--no-audio-occlusion"),
    OFF("audio_reverb_geo", SM_PASS, A, SM_MENU, "audio_reverb_geo", "audio_reverb_geo_desc", "--no-audio-reverb-geo"),
    OFF("audio_pathing", SM_PASS, A, SM_MENU, "audio_pathing", "audio_pathing_desc", "--no-audio-pathing"),
    OFF("text_fixes", SM_RESTART, G, SM_MENU, "fix_typos", "fix_typos_desc", "--no-text-fixes"),
    OFF("script_fixes", SM_RESTART, G, SM_MENU, "script_fixes", "script_fixes_desc", "--no-script-fixes"),
    ON("skip_intro", "false", SM_BOOT, N, 0, "", "skip_intro", "skip_intro_desc", "--skip-intro"),
    ON("smart_allies", "false", SM_PASS, G, SM_MENU, "", "smart_allies", "smart_allies_desc", "--smart-allies"),
    ON("enemy_tactics", "false", SM_PASS, G, SM_MENU, "", "enemy_tactics", "enemy_tactics_desc", "--enemy-tactics"),
    ROW("extra_args", SM_STR, "", "", SM_RESTART, N, 0, "", "extra_args", "extra_args_desc", SM_ARG_EXTRA, "", "", ""),
    ROW("preset", SM_ENUM, "custom", "laptop|balanced|gaming|ultra|integrated|custom", SM_BOOT, N, 0, "", "preset", NULL, SM_ARG_NONE, "",
        "", ""),
};

#define COUNT ((int)(sizeof rows / sizeof rows[0]))

/* The settings that read the depth target and the colour target (renderer/vk_renderer.cpp
 * create_targets: the depth target is sampleable only with one of them on at start; the colour
 * target is TRANSIENT without bloom or a glass HUD). */
static const char DEPTH_READERS[] =
    "ssao|rain_hd|damp_streets|soft_particles_opt_in|lamp_lights|contact_shadows|light_shafts|volfog|cutscene_dof|taa|frame_gen|"
    "taa_upscale!=off";
static const char COLOR_READERS[] = "bloom|hud_style=glass/hd-glass";

int sm_count(void) { return COUNT; }
const SmSetting *sm_setting(int i) { return i >= 0 && i < COUNT ? &rows[i] : NULL; }

int sm_find(const char *key) {
    for (int i = 0; i < COUNT; i++)
        if (!strcmp(rows[i].key, key)) return i;
    return -1;
}

const char *sm_tab_key(SmTab tab) {
    static const char *const keys[] = {"none", "display", "effects", "audio", "controls", "game", "mods"};
    return (unsigned)tab < sizeof keys / sizeof keys[0] ? keys[tab] : "none";
}

/* ---- values ---- */

static int parse_bool(const char *v, int *out) {
    if (!strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "on")) return *out = 1, 1;
    if (!strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0") || !strcmp(v, "off")) return *out = 0, 1;
    return 0;
}

/* profile.cpp parse_int / parse_float: the whole text, within +-1000000. */
static int parse_int(const char *v, long *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (!*v || *end || n < -1000000 || n > 1000000) return 0;
    *out = n;
    return 1;
}

static int parse_float(const char *v, float *out) {
    char *end;
    double d = strtod(v, &end);
    if (!*v || *end || d < -1000000 || d > 1000000) return 0;
    *out = (float)d;
    return 1;
}

static void format_float(char *buf, size_t n, float v) { snprintf(buf, n, "%.3g", (double)v); }

/* 1 when the |-separated list holds value (an empty entry matches ""). */
static int in_list(const char *list, const char *value) {
    size_t len = strlen(value);
    for (const char *p = list;;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == len && !strncmp(p, value, n)) return 1;
        if (!bar) return 0;
        p = bar + 1;
    }
}

/* "LO..HI": 1 and the bounds. */
static int range(const char *dom, double *lo, double *hi) {
    const char *dots = strstr(dom, "..");
    if (!dots) return 0;
    *lo = strtod(dom, NULL);
    *hi = strtod(dots + 2, NULL);
    return 1;
}

int sm_valid(const SmSetting *s, const char *value) {
    if (!s || !value) return 0;
    double lo, hi;
    switch (s->type) {
    case SM_BOOL: {
        int b;
        return parse_bool(value, &b);
    }
    case SM_INT: {
        long n;
        if (!parse_int(value, &n)) return 0;
        if (range(s->domain, &lo, &hi)) return n >= lo && n <= hi;
        char buf[16];
        snprintf(buf, sizeof buf, "%ld", n);
        return in_list(s->domain, buf);
    }
    case SM_FLOAT: {
        float f;
        if (!parse_float(value, &f)) return 0;
        return f < 0 || !range(s->domain, &lo, &hi) || (f >= lo - 1e-6 && f <= hi + 1e-6);
    }
    case SM_ENUM: return in_list(s->domain, value);
    default: return !strchr(value, '\n') && !strchr(value, '\r');
    }
}

/* The value as the ini writes it, in buf (or value itself). */
static const char *normalize(const SmSetting *s, const char *value, char *buf, size_t n) {
    if (s->type == SM_BOOL) {
        int b = 0;
        parse_bool(value, &b);
        return b ? "true" : "false";
    }
    if (s->type == SM_INT) {
        long v = 0;
        parse_int(value, &v);
        snprintf(buf, n, "%ld", v);
        return buf;
    }
    if (s->type == SM_FLOAT) {
        float f = 0;
        parse_float(value, &f);
        format_float(buf, n, f);
        return buf;
    }
    return value;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int sm_values_init(SmValues *vals) {
    vals->v = calloc((size_t)COUNT, sizeof *vals->v);
    if (!vals->v) return -1;
    for (int i = 0; i < COUNT; i++)
        if (!(vals->v[i] = dup_str(rows[i].dflt))) return sm_values_free(vals), -1;
    return 0;
}

int sm_values_copy(SmValues *dst, const SmValues *src) {
    dst->v = calloc((size_t)COUNT, sizeof *dst->v);
    if (!dst->v) return -1;
    for (int i = 0; i < COUNT; i++)
        if (!(dst->v[i] = dup_str(src->v[i]))) return sm_values_free(dst), -1;
    return 0;
}

void sm_values_free(SmValues *vals) {
    if (!vals->v) return;
    for (int i = 0; i < COUNT; i++) free(vals->v[i]);
    free(vals->v);
    vals->v = NULL;
}

int sm_set(SmValues *vals, const char *key, const char *value) {
    int i = sm_find(key);
    if (i < 0) return -1;
    if (!sm_valid(&rows[i], value)) return -2;
    char buf[32];
    char *d = dup_str(normalize(&rows[i], value, buf, sizeof buf));
    if (!d) return -3;
    free(vals->v[i]);
    vals->v[i] = d;
    return 0;
}

const char *sm_get(const SmValues *vals, const char *key) {
    int i = sm_find(key);
    return i < 0 ? NULL : vals->v[i];
}

int sm_truthy(const char *v) { return *v && strcmp(v, "false") && strcmp(v, "0") && strcmp(v, "off"); }

/* One atom (KEY, KEY=V1/V2, KEY!=V1/V2) of length n. */
static int atom(const SmValues *vals, const char *a, size_t n) {
    char key[64];
    size_t k = 0;
    while (k < n && a[k] != '=' && a[k] != '!') k++;
    if (k == 0 || k >= sizeof key) return 0;
    memcpy(key, a, k), key[k] = 0;
    const char *v = sm_get(vals, key);
    if (!v) return 0;
    if (k == n) return sm_truthy(v);
    int neg = a[k] == '!';
    const char *list = a + k + (neg ? 2 : 1), *end = a + n;
    int hit = 0;
    for (const char *p = list;;) {
        const char *slash = memchr(p, '/', (size_t)(end - p));
        size_t len = slash ? (size_t)(slash - p) : (size_t)(end - p);
        if (strlen(v) == len && !strncmp(p, v, len)) hit = 1;
        if (!slash) break;
        p = slash + 1;
    }
    return neg ? !hit : hit;
}

int sm_expr(const SmValues *vals, const char *e) {
    if (!*e) return 1;
    for (const char *clause = e;;) {
        const char *amp = strchr(clause, '&');
        const char *cend = amp ? amp : clause + strlen(clause);
        int any = 0;
        for (const char *p = clause; p < cend;) {
            const char *bar = memchr(p, '|', (size_t)(cend - p));
            const char *aend = bar ? bar : cend;
            any |= atom(vals, p, (size_t)(aend - p));
            p = bar ? bar + 1 : cend;
        }
        if (!any) return 0;
        if (!amp) return 1;
        clause = amp + 1;
    }
}

int sm_available(const SmValues *vals, int i) { return i >= 0 && i < COUNT && sm_expr(vals, rows[i].requires); }

/* ---- command line ---- */

static int push(SmArgv *a, const char *s) {
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 32;
        char **na = realloc(a->a, (size_t)cap * sizeof *na);
        if (!na) return -1;
        a->a = na, a->cap = cap;
    }
    if (!(a->a[a->n] = dup_str(s))) return -1;
    a->n++;
    return 0;
}

void sm_argv_free(SmArgv *a) {
    for (int i = 0; i < a->n; i++) free(a->a[i]);
    free(a->a);
    a->a = NULL, a->n = a->cap = 0;
}

static int is_windows_path(const char *p) { return strchr(p, '\\') || (p[0] && p[1] == ':'); }

/* command.cpp strip_separators: trailing / and \ off, but not a drive root's ("C:\"). */
static void strip_separators(char *p) {
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\') && !(n == 3 && p[1] == ':')) p[--n] = 0;
}

/* dir + separator (when it does not end in one) + name, as a new string. */
static char *with_name(const char *dir, const char *name) {
    size_t n = strlen(dir), m = strlen(name);
    char *s = malloc(n + m + 2);
    if (!s) return NULL;
    memcpy(s, dir, n + 1);
    strip_separators(s);
    n = strlen(s);
    char sep = is_windows_path(s) ? '\\' : '/';
    if (n && s[n - 1] != sep) s[n++] = sep;
    memcpy(s + n, name, m + 1);
    return s;
}

static const char *const FPS_ITEMS[] = {"fps", "ms", "cpu", "gpu"};

/* profile.cpp parse_fps_overlay_items: the mask of a "fps,cpu" list, -1 for an empty, unknown or
 * malformed one. */
static int fps_items_mask(const char *s) {
    int mask = 0;
    for (const char *p = s;;) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        int bit = -1;
        for (int i = 0; i < 4; i++)
            if (strlen(FPS_ITEMS[i]) == n && !strncmp(p, FPS_ITEMS[i], n)) bit = i;
        if (bit < 0) return -1;
        mask |= 1 << bit;
        if (!comma) return mask;
        p = comma + 1;
    }
}

/* command.cpp split_args: whitespace splits, "..." and '...' group, backslashes are literal. */
static int split_args(SmArgv *a, const char *s) {
    size_t n = strlen(s);
    char *cur = malloc(n + 1);
    if (!cur) return -1;
    size_t len = 0;
    int in_arg = 0, rc = 0;
    char quote = 0;
    for (const char *c = s; *c && !rc; c++) {
        if (quote) {
            if (*c == quote) quote = 0;
            else cur[len++] = *c;
        } else if (*c == '"' || *c == '\'') {
            quote = *c;
            in_arg = 1;
        } else if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') {
            if (in_arg) cur[len] = 0, rc = push(a, cur);
            len = 0;
            in_arg = 0;
        } else {
            cur[len++] = *c;
            in_arg = 1;
        }
    }
    if (in_arg && !rc) cur[len] = 0, rc = push(a, cur);
    free(cur);
    return rc;
}

static int push_int(SmArgv *a, const char *flag, long v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", v);
    return push(a, flag) || push(a, buf);
}

static int push_float(SmArgv *a, const char *flag, float v) {
    char buf[32];
    format_float(buf, sizeof buf, v);
    return push(a, flag) || push(a, buf);
}

static int emit(const SmValues *vals, int i, SmArgv *a) {
    const SmSetting *s = &rows[i];
    const char *v = vals->v[i];
    if (!sm_expr(vals, s->arg_requires)) return 0;
    long n = 0;
    float f = 0;
    double lo = 0, hi = 0;
    switch (s->arg) {
    case SM_ARG_NONE:
    case SM_ARG_HEAD: return 0;
    case SM_ARG_ON:
        if (strcmp(v, "true")) return 0;
        return push(a, s->flag) || (*s->param && push(a, s->param));
    case SM_ARG_OFF: return strcmp(v, "false") ? 0 : push(a, s->flag);
    case SM_ARG_VALUE: return !*v || !strcmp(v, s->param) ? 0 : push(a, s->flag) || push(a, v);
    case SM_ARG_LISTED: return in_list(s->param, v) ? push(a, s->flag) || push(a, v) : 0;
    case SM_ARG_ALWAYS: {
        const char *last = strrchr(s->param, '|');
        return push(a, s->flag) || push(a, *v && in_list(s->param, v) ? v : last ? last + 1 : s->param);
    }
    case SM_ARG_MAP:
        for (const char *p = s->param; *p;) {   /* "value=--flag|..." */
            const char *eq = strchr(p, '='), *bar = strchr(p, '|');
            if (!eq) return 0;
            size_t vn = (size_t)(eq - p);
            if (strlen(v) == vn && !strncmp(p, v, vn)) {
                char flag[64];
                size_t fn = bar ? (size_t)(bar - eq - 1) : strlen(eq + 1);
                if (fn >= sizeof flag) return 0;
                memcpy(flag, eq + 1, fn), flag[fn] = 0;
                return push(a, flag);
            }
            if (!bar) return 0;
            p = bar + 1;
        }
        return 0;
    case SM_ARG_INT:
        parse_int(v, &n);
        return n > strtol(s->param, NULL, 10) ? push_int(a, s->flag, n) : 0;
    case SM_ARG_INT_CLAMP:
        parse_int(v, &n);
        range(s->domain, &lo, &hi);
        return n > strtol(s->param, NULL, 10) ? push_int(a, s->flag, n > hi ? (long)hi : n) : 0;
    case SM_ARG_INT_NE:
        parse_int(v, &n);
        return n > 0 && n != strtol(s->dflt, NULL, 10) ? push_int(a, s->flag, n) : 0;
    case SM_ARG_VOLUME:
        parse_int(v, &n);
        range(s->domain, &lo, &hi);
        return n ? push_int(a, s->flag, n < lo ? (long)lo : n > hi ? (long)hi : n) : 0;
    case SM_ARG_FLOAT:
        parse_float(v, &f);
        return f >= 0 ? push_float(a, s->flag, f) : 0;
    case SM_ARG_FLOAT_NE:
        parse_float(v, &f);
        return f >= 0 && f != strtof(s->dflt, NULL) ? push_float(a, s->flag, f) : 0;
    case SM_ARG_GLYPHS: {   /* command.cpp glyph_pack_dir */
        const char *icons = sm_get(vals, "button_icons");
        if (!*v || (strcmp(icons, "ps-black") && strcmp(icons, "ps-white") && strcmp(icons, "ps-color"))) return 0;
        char *dir = with_name(v, icons);
        int rc = !dir || push(a, s->flag) || push(a, dir);
        free(dir);
        return rc ? -1 : 0;
    }
    case SM_ARG_ICONS:   /* command.cpp build_argv: ps-* without a glyph pack folder, the runtime's own icons */
        if (in_list(s->param, v)) return push(a, s->flag) || push(a, v);
        if (*sm_get(vals, "glyph_packs") || !in_list("ps-black|ps-white|ps-color", v)) return 0;
        return push(a, s->flag) || push(a, v);
    case SM_ARG_FPS_ITEMS: {   /* profile.cpp fps_overlay_mask; fps + ms is the runtime's default */
        int mask = *v ? fps_items_mask(v) : 3;
        if (mask <= 0) mask = 1;
        if (mask == 3) return 0;
        char list[32] = "";
        for (int b = 0; b < 4; b++)
            if (mask & 1 << b) strcat(list, *list ? "," : ""), strcat(list, FPS_ITEMS[b]);
        return push(a, s->flag) || push(a, list);
    }
    case SM_ARG_DRAW_DISTANCE: {   /* command.cpp draw_distance_args */
        static const float far_scale[] = {1.5f, 2, 3}, prop_scale[] = {2, 4, 8};
        parse_int(v, &n);
        if (n < 2 || n > 4) return 0;
        return push_float(a, "--draw-distance", far_scale[n - 2]) || push_float(a, "--prop-detail", prop_scale[n - 2]);
    }
    case SM_ARG_EXTRA: return split_args(a, v);
    }
    return 0;
}

int sm_build_argv(const SmValues *vals, SmArgv *out) {
    char *disc = dup_str(sm_get(vals, "disc"));
    if (!disc) return -1;
    strip_separators(disc);
    char *xbe = with_name(disc, "default.xbe");
    int rc = !xbe || push(out, sm_get(vals, "xbrun")) || push(out, xbe) || push(out, disc);
    free(xbe);
    free(disc);
    for (int i = 0; i < COUNT && !rc; i++) rc = emit(vals, i, out);
    return rc ? -1 : 0;
}

/* ---- classes ---- */

int sm_class_rank(char cls) {
    switch (cls) {
    case SM_LIVE: return 0;
    case SM_PASS: return 1;
    case SM_REINIT: return 2;
    case SM_RESTART: return 3;
    default: return 4;
    }
}

char sm_change_class(const SmValues *start, int i, const char *value) {
    const SmSetting *s = sm_setting(i);
    if (!s || !strcmp(start->v[i], value)) return SM_LIVE;
    char cls = s->cls;
    if (!strcmp(s->key, "taa_upscale") && strcmp(start->v[i], "off") && strcmp(value, "off"))
        return SM_LIVE;   /* renderer_taa_render_size: the mode while it stays on */
    if ((s->flags & SM_BOOT_PACK) && !sm_truthy(start->v[i]) && sm_truthy(value)) return SM_RESTART;
    if (s->flags & (SM_DEPTH | SM_COLOR)) {   /* the start values with this one changed */
        char *v[COUNT];
        memcpy(v, start->v, sizeof v);
        v[i] = (char *)value;
        const SmValues after = {v};
        int depth = (s->flags & SM_DEPTH) && !sm_expr(start, DEPTH_READERS) && sm_expr(&after, DEPTH_READERS);
        int color = (s->flags & SM_COLOR) && !sm_expr(start, COLOR_READERS) && sm_expr(&after, COLOR_READERS);
        if ((depth || color) && sm_class_rank(cls) < sm_class_rank(SM_REINIT)) cls = SM_REINIT;
    }
    return cls;
}

char sm_changes_class(const SmValues *start, const SmValues *now) {
    char worst = SM_LIVE;
    for (int i = 0; i < COUNT; i++) {
        char c = sm_change_class(start, i, now->v[i]);
        if (sm_class_rank(c) > sm_class_rank(worst)) worst = c;
    }
    return worst;
}

/* ---- pending file ---- */

int sm_pending_format(char *buf, size_t n, const char *profile, const SmValues *start, const SmValues *now) {
    if (strchr(profile, ']') || strchr(profile, '\n') || strchr(profile, '\r')) return -1;
    size_t at = 0;
    int len = snprintf(buf, n, "%s\n[profile %s]\n", SM_PENDING_MAGIC, profile);
    if (len < 0) return -1;
    at = (size_t)len;
    for (int i = 0; i < COUNT; i++) {
        if (!(rows[i].flags & SM_MENU) || !strcmp(start->v[i], now->v[i])) continue;
        len = snprintf(at < n ? buf + at : NULL, at < n ? n - at : 0, "%s = %s\n", rows[i].key, now->v[i]);
        if (len < 0) return -1;
        at += (size_t)len;
    }
    return (int)at;
}
