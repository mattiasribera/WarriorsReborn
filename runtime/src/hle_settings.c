/* The in-game settings menu in the running game: see hle_settings.h. */
#include "hle_settings.h"
#include "hle_audio.h"
#include "hle_d3d.h"
#include "hle_input.h"
#include "hle_modmenu.h"
#include "input_map.h"
#include "neon_glow.h"
#include "normal_maps.h"
#include "renderer.h"
#include "settings_apply.h"
#include "settings_menu.h"
#include "settings_sync.h"
#include "video.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int (*hle_settings_next_filter)(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);
int (*hle_settings_next_escape)(void);

#define HOLD_MS 500.0      /* Back + Start held this long opens the menu */
#define CLOCK_IDLE_MS 300  /* the game clock still for this long: the game is paused (or loading) */
#define START_TAP_POLLS 3  /* pad polls the F10 Start press lasts */

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- live setters (host thread, at a frame boundary: src/settings_apply.h) ---- */
static void set_volume(int cls, const char *v) { hle_audio_set_class_volume(cls, (float)atof(v)); }
static void set_music(const char *v) { set_volume(HLE_AUDIO_CLASS_MUSIC, v); }
static void set_sfx(const char *v) { set_volume(HLE_AUDIO_CLASS_SFX, v); }
static void set_dialogue(const char *v) { set_volume(HLE_AUDIO_CLASS_DIALOGUE, v); }
static void set_rumble(const char *v) {
    input_rumble_enabled = sm_truthy(v);
    if (!input_rumble_enabled && input_rumble_hook) input_rumble_hook(0, 0, 0);   /* motors running now stop */
}
static void set_air(const char *v) { hle_audio_set_air(sm_truthy(v)); }
static int occl_started;   /* the level's wall map is built only when occlusion was on at start */
static int can_occlusion(const char *v) { return occl_started || !sm_truthy(v); }
static void set_occlusion(const char *v) { hle_audio_set_occlusion(sm_truthy(v)); }
static int can_taa(const char *v) { (void)v; return hle_d3d_live_taa_ok(); }
static void set_taa_sharpness(const char *v) { hle_d3d_live_taa_sharpness((float)atof(v)); }
static void set_saturation(const char *v) { hle_d3d_live_saturation((float)atof(v)); }
static int can_hdr_levels(const char *v) { (void)v; return hle_d3d_hdr.mode != HDR_OFF; }
static void set_hdr_peak(const char *v) { hle_d3d_live_hdr_levels((float)atof(v), hle_d3d_hdr.paper); }
static void set_hdr_paper(const char *v) { hle_d3d_live_hdr_levels(hle_d3d_hdr.peak, (float)atof(v)); }
static int can_parallax(const char *v) { (void)v; return hle_d3d_normal_maps.on; }
static void set_parallax(const char *v) {
    hle_d3d_normal_maps.parallax = sm_truthy(v);
    renderer_set_parallax(hle_d3d_normal_maps.parallax ? hle_d3d_normal_maps.parallax_depth : 0);
}
static int can_neon(const char *v) { (void)v; return hle_d3d_neon_glow.on; }
static void set_neon(const char *v) {
    const float f = (float)atof(v);
    hle_d3d_neon_glow.strength = f > 0 ? f : NEON_GLOW_STRENGTH_DEFAULT;
}
static int can_upscale(const char *v) { return strcmp(v, "off") && hle_d3d_live_upscale_ok(); }
static void set_upscale(const char *v) { hle_d3d_live_upscale_mode(v); }
static int can_dynres(const char *v) { (void)v; return hle_d3d_live_upscale_ok(); }
static void set_dynres(const char *v) { hle_d3d_live_dynres(sm_truthy(v)); }

static const SaLive live_table[] = {
    {"music_volume", NULL, set_music},
    {"sfx_volume", NULL, set_sfx},
    {"dialogue_volume", NULL, set_dialogue},
    {"rumble", NULL, set_rumble},
    {"audio_air", NULL, set_air},
    {"audio_occlusion", can_occlusion, set_occlusion},
    {"taa_sharpness", can_taa, set_taa_sharpness},
    {"parallax", can_parallax, set_parallax},
    {"neon_glow_strength", can_neon, set_neon},
    {"taa_upscale", can_upscale, set_upscale},
    {"dynres", can_dynres, set_dynres},
    {"saturation", NULL, set_saturation},
    {"hdr_peak", can_hdr_levels, set_hdr_peak},
    {"hdr_paper_white", can_hdr_levels, set_hdr_paper},
};

static void frame_boundary(void) { sa_frame(); }

/* ---- the menu ---- */
static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;
static SettingsMenu menu;
static ModMenu shown;          /* the drawing thread's copy */
static int shown_open;
static volatile int open_request, escape_request;
static int swallow, tap_polls;
static double hold_t0;
static uint32_t clock_last;
static double clock_moved_ms;

static int ops_set(const char *k, const char *v) {
    const int r = settings_sync_set(k, v);
    if (r && r != -3) printf("[settings] %s = %s refused (%d)\n", k, v, r);
    return r;
}
static int ops_live(const char *k, const char *v) {
    const int live = sa_request(settings_sync_start(), k, v);
    printf("[settings] %s = %s: %s\n", k, v, live ? "live at the next frame" : "on restart");
    fflush(stdout);
    return live;
}
static int ops_can_live(const char *k, const char *v) { return sa_can_live(settings_sync_start(), k, v); }
static void ops_restart(void) {
    printf("[settings] restart requested from the menu: the launcher relaunches with the changes\n");
    fflush(stdout);
    settings_sync_restart();
}

static void publish(void) {
    pthread_mutex_lock(&view_lock);
    shown = menu.m;
    shown_open = menu.m.open;
    pthread_mutex_unlock(&view_lock);
}

static int menu_view(ModMenu *out, HleMenuArt *art) {
    pthread_mutex_lock(&view_lock);
    const int open = shown_open;
    if (open) *out = shown;
    pthread_mutex_unlock(&view_lock);
    if (!open) return 0;
    memset(art, 0, sizeof *art);
    hle_modmenu_font(art);
    return 1;
}

static void open_menu(double t, const char *how, int tap, const char *pause) {
    smenu_open(&menu, t);
    tap_polls = tap ? START_TAP_POLLS : 0;
    publish();
    printf("[settings] menu open (%s): %s\n", how, pause);
    fflush(stdout);
}

void (*hle_settings_next_key)(int key, int down);

static void key_hook(int key, int down) {
    if (key == INPUT_KEY_F1 + 9 && down) open_request = 1;   /* F10 */
    else if (hle_settings_next_key && !menu.m.open) hle_settings_next_key(key, down);
}

static int escape_hook(void) {
    if (shown_open) {
        escape_request = 1;
        return 1;
    }
    return hle_settings_next_escape ? hle_settings_next_escape() : 0;
}

static int input_filter(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly) {
    const double t = mono_ms();
    uint32_t ms;
    if (hle_d3d_game_clock_ms(&ms) && ms != clock_last) clock_last = ms, clock_moved_ms = t;
    if (!menu.m.open) {
        const int f10 = open_request;
        open_request = 0;
        escape_request = 0;
        if ((buttons & 0x30) == 0x30) {   /* Back + Start held: the game got Start (its pause) on the way */
            if (!hold_t0) hold_t0 = t;
            else if (t - hold_t0 >= HOLD_MS) {
                hold_t0 = 0;
                open_menu(t, "Back + Start", 0, "the game got Start itself (its pause screen)");
            }
        } else {
            hold_t0 = 0;
        }
        if (!menu.m.open && f10) {
            const int running = t - clock_moved_ms < CLOCK_IDLE_MS;
            open_menu(t, "F10", running, running ? "Start pressed for the game: its pause screen stays behind the menu"
                                                 : "the game clock is still (a pause screen or a load): no Start press");
        }
        if (!menu.m.open) {
            if (swallow) {
                if (!(buttons & 0x3f) && !analog[0] && !analog[1] && !analog[4] && !analog[5]) swallow = 0;
                return 1;
            }
            return hle_settings_next_filter ? hle_settings_next_filter(buttons, analog, lx, ly) : 0;
        }
    }
    const unsigned c = smenu_controls(buttons, analog, lx, ly, input_key_down('q'), input_key_down('e'));
    if (escape_request || open_request) {   /* Escape or F10 again */
        escape_request = open_request = 0;
        smenu_close(&menu);
    } else {
        smenu_step(&menu, c, t);
    }
    if (!menu.m.open) {
        swallow = 1;   /* until every button is released */
        printf("[settings] menu closed\n");
        fflush(stdout);
    }
    publish();
    if (tap_polls > 0) {
        tap_polls--;
        return tap_polls ? 2 : 1;   /* Start held, then released on the last poll */
    }
    return 1;
}

/* The launcher's presets file (W7), read once. */
static char *read_file(const char *path) {
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    char *buf = malloc(1 << 16);
    size_t n = buf ? fread(buf, 1, (1 << 16) - 1, f) : 0;
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

void hle_settings_install(const char *disc_dir, const char *game_language) {
    const char *ui = settings_sync_ui_language();
    const char *lang = ui && *ui ? ui : game_language && !strcmp(game_language, "Spanish") ? "es" : "en";
    char *presets = read_file(settings_sync_presets_path());
    const SmenuOps ops = {settings_sync_start, settings_sync_now, ops_set, ops_live, ops_can_live,
                          settings_sync_saved() ? ops_restart : NULL, settings_sync_saved(), settings_sync_restart_ok(), 0};
    smenu_init(&menu, &ops, lang, presets);
    free(presets);
    occl_started = hle_d3d_audio_occlusion;
    sa_register(live_table, (int)(sizeof live_table / sizeof live_table[0]));
    hle_modmenu_set_disc(disc_dir);
    input_key_hook = key_hook;
    input_menu_filter = input_filter;
    video_escape_hook = escape_hook;
    hle_d3d_menu_view = menu_view;
    hle_d3d_settings_hook = frame_boundary;
    printf("[settings] in-game settings menu: F10 or Back + Start held (%s; %d presets; %s)\n", lang, menu.npresets,
           settings_sync_saved() ? "changes saved to the launcher profile" : "no launcher: changes last this session");
}
