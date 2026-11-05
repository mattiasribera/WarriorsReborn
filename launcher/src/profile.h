/* Launcher profiles: one xbrun binary + the user's game copy (disc and hdd folders) + the
 * runtime options (runtime/src/main.c). Profiles live in a plain-text file next to the launcher:
 *
 *   [launcher]
 *   selected = stable
 *   [profile stable]
 *   xbrun = C:\Warriors\TheWarriors.exe   (key kept for older profiles; any game executable)
 *   disc = C:\Warriors\disc
 *   ...
 *
 * Keys the launcher does not know are kept with their profile, and sections it does not know
 * are kept verbatim, so a newer launcher's file survives a round trip. Comments are dropped. */
#ifndef LAUNCHER_PROFILE_H
#define LAUNCHER_PROFILE_H
#include <string>
#include <utility>
#include <vector>

namespace launcher {

constexpr int SPRAY_TIME_DEFAULT = 110; /* runtime/src/hle_ratefix.h RATEFIX_SPRAY_DEFAULT */
constexpr int SPRAY_TIME_MIN = 100, SPRAY_TIME_MAX = 200;

/* Every value at its default makes the runtime run as with no option at all. */
struct Profile {
    std::string name = "default";
    /* Game copy */
    std::string xbrun, disc, hdd;
    /* Graphics */
    std::string resolution;          /* "WxH", empty = 640x480 (or render_scale) */
    int render_scale = 1;            /* 1-8, used when resolution is empty */
    int msaa = 0;                    /* 0 = off, 2, 4, 8 */
    int aniso = 0;                   /* 0 = off, 2, 4, 8, 16 */
    bool widescreen = false, fullscreen = false;
    bool movie_hd = true;            /* false: --no-movie-hd (the 640x480 movies) */
    std::string present = "auto";    /* auto|mailbox|fifo|immediate|readback */
    std::string texture_pack;
    std::string sound_pack;          /* --sound-pack DIR: <cue name>.wav replacements (src/sndpack.h) */
    /* Button icons: xbox (the game's own), kbm = key caps and mouse drawings for the key map
     * (--button-icons kbm), auto = kbm or the controller's icons by the last used device
     * (--button-icons auto). icon_pack: an optional folder with the user's own button-icon atlas,
     * layered as the first --texture-pack (before texture_pack). */
    std::string button_icons = "xbox";
    std::string icon_pack;
    /* Language */
    std::string language = "en";     /* en|es|fr|de|it */
    /* Gameplay */
    int spawn_scale = 1;             /* 1-8 */
    bool rate_fixes = true, frame_lock = true;
    std::string input_config;
    bool rumble = true;              /* false: --no-rumble */
    std::string warp;                /* --warp MISSION[:CHECKPOINT] or levelNN[:CHECKPOINT], empty = off */
    /* Advanced */
    std::string audio = "sdl";       /* sdl|null */
    std::string video = "sdl";       /* sdl|x11|null */
    std::string renderer = "vulkan"; /* vulkan|none */
    bool render_targets = true, stats = false, trace = false;
    /* Launcher v2 controls (formerly typed in extra_args) */
    int supersample = 1;             /* --supersample 1|2|4 */
    int shadow_scale = 1;            /* --shadow-scale 1-4 */
    int fx_scale = 1;                /* --fx-scale 1 Normal, 2 More, 4 Maximum (effect pools; 1 = left out) */
    bool shadow_soft = false;        /* --shadow-soft */
    bool shadow_range = false;       /* --shadow-range (character shadows stop short of floors far below) */
    int frames_in_flight = 0;        /* --frames-in-flight 1-3, 0 = left out (runtime default 2) */
    int sharpen = 0;                 /* --sharpen 1-100 (contrast-adaptive sharpening), 0 = off */
    bool bloom = false;              /* --bloom (lights glow into the 3D scene) */
    float bloom_strength = -1;       /* --bloom-strength 0.1-3 after --bloom, < 0 = left out (runtime default 0.6) */
    bool rain_hd = false;            /* --rain-hd (finer rain at several depths while it rains) */
    bool rain_puddles = false;       /* --rain-hd-puddles after --rain-hd (puddles with drop ripples on the wet ground) */
    bool ssao = false;               /* --ssao (ambient occlusion of the 3D world) */
    float ssao_strength = -1;        /* --ssao-strength 0.1-2 after --ssao, < 0 = left out (runtime default 0.8) */
    bool contact_shadows = false;    /* --contact-shadows (the ground right under people and props darkens) */
    bool lamp_lights = false;        /* --lamp-lights (lamps, fire and neon light the world around them) */
    float lamp_lights_strength = -1; /* --lamp-lights-strength 0.1-3 after --lamp-lights, < 0 = left out (runtime default 1) */
    bool soft_particles = false;     /* --soft-particles (smoke, fire and dust fade where they meet the ground and walls); opt-in:
                                      * off in every preset, saved as soft_particles_opt_in */
    bool filmic = false;             /* --filmic (1979-film look of the 3D scene: tonemap, grade, halation) */
    bool sky_hd = false;             /* --sky-hd (procedural night sky, moon, skyline windows and haze) */
    bool sharp_fonts = true;         /* false: --no-sharp-fonts (the runtime's 4x font atlases are on by default; on in
                                      * every preset) */
    bool sharp_faces = false;        /* --sharp-faces (character skins upscaled 2x at runtime from the game's own textures) */
    /* Draw distance preset: 1 = as shipped (no flags), 2-4 = farther fog/far plane and small props
     * (--draw-distance and --prop-detail, see draw_distance_args in command.cpp). */
    int draw_distance = 1;
    bool sim60 = false;              /* --sim60 */
    std::string hud_style = "original"; /* original|hd|hd-glass|glass */
    /* --hud-hd-track-opacity, -fill-opacity, -frost, -height after the style flag; < 0 = left
     * out. Only for the hd and hd-glass styles. */
    float hud_track_opacity = -1, hud_fill_opacity = -1, hud_frost = -1, hud_height = -1;
    int spray_time = SPRAY_TIME_DEFAULT;           /* --spray-time 100-200 percent, 110 = left out (runtime default) */
    bool text_fixes = true;          /* false: --no-text-fixes */
    std::string preset = "custom";   /* laptop|balanced|gaming|custom: detect_preset() when saved */
    std::string extra_args;          /* appended as typed (split like a shell, no escapes) */
    std::vector<std::pair<std::string, std::string>> unknown;
};

/* The [launcher] section besides "selected". */
struct LauncherSettings {
    int setup_done = -1;             /* -1 = not in the file (an older launcher wrote it) */
    bool close_on_start = false, show_advanced = false;
    std::string last_page, last_log;
    std::string ui_language;         /* the launcher's own language: en|es, empty = the system's */
    bool backup_saves = true;        /* copy the save before each PLAY (backup.h) */
    int backup_count = 5;            /* backups kept per save folder, 1-20 */
    std::string ui_scale;            /* interface size: auto (or empty)|1|1.25|1.5 (uiscale.h) */
    std::vector<std::pair<std::string, std::string>> unknown;
};

/* A controller's own settings (the Controls page's Controllers panel), for every profile, in a
 * [controller KEY] section: KEY is the SDL GUID in lower case, with #K for the Kth (2 and up) of
 * identical controllers attached at once (controller_key). */
struct ControllerPref {
    std::string key;
    std::string name;                /* the name shown instead of SDL's, empty = SDL's */
    int player = 0;                  /* 1-4 (--pad-slot), 0 = Auto */
    std::vector<std::pair<std::string, std::string>> unknown;
};

struct ProfileFile {
    std::vector<Profile> profiles;
    std::string selected;
    LauncherSettings settings;
    std::vector<ControllerPref> controllers; /* written when they have a name, a player or unknown keys */
    std::vector<std::string> other_sections; /* unknown sections, raw lines incl. the header */
};

/* Parses the file text; returns false on a malformed line (out is still filled with what was
 * readable). Warnings (unknown keys, bad values, bad lines) go to *warnings when given. */
bool parse_ini(const std::string &text, ProfileFile &out, std::vector<std::string> *warnings);
std::string write_ini(const ProfileFile &file);

bool load_ini(const std::string &path, ProfileFile &out, std::vector<std::string> *warnings, std::string *err);
bool save_ini(const std::string &path, const ProfileFile &file, std::string *err);

/* Moves the options a control now covers (--sim60/--no-sim60, --hud-hd-glass, --supersample N,
 * --shadow-scale N, --shadow-soft, --shadow-range / --no-shadow-range, --frames-in-flight N, --sharpen N, --bloom / --no-bloom,
 * --bloom-strength F, --rain-hd / --no-rain-hd, --rain-hd-puddles / --no-rain-hd-puddles, --ssao / --no-ssao, --contact-shadows / --no-contact-shadows, --ssao-strength F, --lamp-lights / --no-lamp-lights, --lamp-lights-strength F, --soft-particles / --no-soft-particles, --sharp-fonts / --no-sharp-fonts, --filmic / --no-filmic, --sky-hd / --no-sky-hd, --sharp-faces / --no-sharp-faces, --fx-scale N (1-4), --spray-time N, --text-fixes / --no-text-fixes) from extra_args into their fields, so the command line stays the same: the
 * fields come before extra_args and the runtime keeps the last value of each. A value the runtime
 * would reject stays typed, and so does --hud-hd-glass after another --hud option (its order
 * matters) or on top of the glass style (--soft-particles-distance F stays typed). parse_ini calls it
 * for every profile. Returns the number of options moved. */
int migrate_extra_args(Profile &p);

/* Graphics presets. Each sets the covered fields: resolution, widescreen, fullscreen, msaa,
 * supersample, aniso, shadow_scale, shadow_soft, shadow_range, frames_in_flight, sim60, hud_style (the HUD
 * overrides cleared), bloom (its strength cleared), rain_hd, rain_puddles, ssao (its strength cleared), contact_shadows, lamp_lights (its strength cleared), soft_particles (off in every preset: opt-in), sharp_fonts (on in every preset), filmic, sky_hd and sharp_faces, plus install_dir/texpack-v3 as the texture
 * pack when that folder exists and none is chosen. Laptop (Performance): 1920x1080 fullscreen
 * widescreen, aniso 4, 2 frames in flight, sim60, hd-glass HUD, no bloom, no better rain, no puddles, no ambient
 * occlusion, no contact shadows, no lamp lights, no filmic look, no enhanced sky, and draw distance and effect pools back to as shipped (1).
 * Balanced: the same with MSAA 2, soft shadows at shadow scale 1, bloom, better rain with puddles, draw
 * distance 2 and effect pools 2, the filmic look and the enhanced sky, no lamp lights (the mildest step up; both part of its match), frames in flight
 * left to the runtime. Gaming (Quality): MSAA 4, shadow scale 2, soft shadows, shadow range,
 * bloom, better rain with puddles, ambient occlusion, contact shadows, lamp lights, the filmic look and the enhanced sky, frames in flight left to the runtime,
 * draw distance and pools left as they are. Supersampling stays off in all (opt in by hand). Any
 * other combination is Custom. The order is the cards' order. */
enum class Preset { Laptop, Balanced, Gaming, Custom };
void apply_preset(Profile &p, Preset preset, const std::string &install_dir);
Preset detect_preset(const Profile &p);
const char *preset_key(Preset preset);          /* laptop|balanced|gaming|custom */
Preset preset_from_key(const std::string &key); /* Custom for an unknown key */

/* The profile a first run and the New button start from: preset (gpu.h's preset_for_gpu picks it
 * from the PC's graphics card; Custom gives Balanced) with English and stats. The texture pack is
 * install_dir/texpack-v3,
 * set only when its folder exists, so a missing pack never becomes a broken --texture-pack.
 * Plain Profile() keeps the runtime defaults, so saved profiles parse as before. */
Profile recommended_profile(const std::string &install_dir, Preset preset = Preset::Gaming);

/* A name not used by another profile in file, based on base ("base", "base 2", ...). */
std::string unique_profile_name(const ProfileFile &file, const std::string &base);

/* The ControllerPref key of the nth (1-based) attached controller with an SDL GUID string. */
std::string controller_key(const std::string &guid, int nth);
ControllerPref *find_controller(ProfileFile &file, const std::string &key);   /* nullptr when absent */
const ControllerPref *find_controller(const ProfileFile &file, const std::string &key);
ControllerPref &controller_pref(ProfileFile &file, const std::string &key);   /* added when absent */
/* Player 1-4 (or 0, Auto) for a controller; a player another controller had goes back to Auto. */
void set_controller_player(ProfileFile &file, const std::string &key, int player);

/* A float as the ini and the command line write it (%.3g: 0.25, 4, 1.5). */
std::string format_float(float v);

} // namespace launcher
#endif
