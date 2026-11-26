/* Photo mode in the running game: see hle_photo.h. Threads: the pad combo is seen by the guest's
 * input filter, F9 / Escape by the window pump, the control socket's command on its own thread;
 * all of them only post requests. Everything else runs on the host thread: photo_tick after each
 * presented frame (arming and freezing), photo_view before each replay of the frozen frame (input,
 * camera, panel, capture), photo_pixels with each capture frame's readback. */
#include "hle_photo.h"
#include "hle_d3d.h"
#include "hle_input.h"
#include "hle_modmenu.h"
#include "input_map.h"
#include "photo.h"
#include "platform.h"
#include "renderer.h"
#include "render_rec.h"   /* renderer_* through the rr_* wrappers: the render64 helper's renderer under --render-process helper */
#include "video.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const char *hle_photo_dir;
int hle_photo_prepare = 1;

#define HOLD_MS 500.0       /* Back + RB held this long enters photo mode */
#define ARM_FRAMES 3        /* frames recorded with the photo settings before the freeze */
#define PREVIEW_MS 15.0     /* preview replays at most this often */
#define WARM_FRAMES 8       /* replays per tile before its samples count (temporal histories settle) */
#define CMD_TIMEOUT_S 180

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- texts (neutral Spanish, tú) ---- */
static int es;
#define T(en, sp) (es ? (sp) : (en))

/* ---- the settings photo mode turns to their best (photo.h PhKnob; restored on leaving) ---- */
#define KNOB_INT(name, expr, can_expr)                                    \
    static float get_##name(void) { return (float)(expr); }              \
    static void set_##name(float v) { expr = (int)v; }                    \
    static int can_##name(void) { return can_expr; }
KNOB_INT(ssao, hle_d3d_ssao.on, renderer_ssao_on())
KNOB_INT(ssgi, hle_d3d_ssao.ssgi, renderer_ssgi_on() && hle_d3d_ssao.mode == SSAO_MODE_GTAO)
KNOB_INT(contact, hle_d3d_contact.on, renderer_contact_on())
KNOB_INT(volfog, hle_d3d_volfog.on, renderer_volfog_on())
KNOB_INT(wshadow, hle_d3d_world_shadow.on, renderer_world_shadow_on())
KNOB_INT(bloom, hle_d3d_bloom.on, renderer_bloom_on())
KNOB_INT(lamp, hle_d3d_lamp.on, renderer_lamp_lights_on())
KNOB_INT(lshadow, hle_d3d_lamp_shadow.on, renderer_lamp_shadows_on())
KNOB_INT(shafts, hle_d3d_shafts.on, renderer_light_shafts_on() && !hle_d3d_volfog.on)
KNOB_INT(soft, hle_d3d_soft.on, renderer_soft_on())
KNOB_INT(dof, hle_d3d_dof.on, 1)   /* the cutscene focus would fight the photo's own depth of field */
static const PhKnob knobs[] = {
    {"ssao", get_ssao, set_ssao, 1, can_ssao},           {"ssgi", get_ssgi, set_ssgi, SSGI_HIGH, can_ssgi},
    {"contact", get_contact, set_contact, 1, can_contact}, {"volfog", get_volfog, set_volfog, 1, can_volfog},
    {"world-shadows", get_wshadow, set_wshadow, 1, can_wshadow}, {"bloom", get_bloom, set_bloom, 1, can_bloom},
    {"lamp-lights", get_lamp, set_lamp, 1, can_lamp},     {"lamp-shadows", get_lshadow, set_lshadow, 1, can_lshadow},
    {"light-shafts", get_shafts, set_shafts, 1, can_shafts}, {"soft-particles", get_soft, set_soft, 1, can_soft},
    {"cutscene-dof", get_dof, set_dof, 0, can_dof},
};

/* ---- state ---- */
enum { S_OFF, S_ARMING, S_FREEZING, S_ACTIVE };
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stage;                 /* S_*, host thread writes (under mx) */
static int req_enter, req_exit, req_capture;
static char req_path[1024];
static uint32_t gen_enter, gen_exit, gen_capture;   /* completed requests (command waits) */
static char last_error[256], last_photo[1100];
static int last_w, last_h, last_frames;
static PhInput cmd_move;          /* the control socket's held camera input */
static double cmd_move_until;

static PhState ph;
static PhCamera gcam;
static PhFreeCam cam;
static PhAccum acc;
static PhShot shot;
static int arm, warm, tile_last = -1;
static double last_view;
static uint16_t prev_buttons;
static uint8_t prev_analog[8];
static int swallow;               /* after leaving: the pad goes to the game once released */
static double hold_t0;
static int (*next_filter)(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);
static int (*next_escape)(void);
static void (*next_key)(int key, int down);
static void (*next_settings)(void);
static int (*next_view)(ModMenu *out, HleMenuArt *art);

/* the photo settings (the panel) */
static int res_mode = 0;          /* 0 the screen, 1 2x, 2 4K */
static int ss = 1;                /* 1 2 4 */
static int samples = 64;
static float focus = 4, aperture = 0;
static PhLook look;
static int hide_chars, speed = 1; /* 0 slow 1 normal 2 fast */

/* ---- the panel ---- */
enum { R_TAKE, R_RES, R_SS, R_SAMPLES, R_FOCUS, R_APERTURE, R_EXPOSURE, R_FILTER, R_SAT, R_VIGNETTE, R_GRAIN, R_BARS, R_CHARS, R_SPEED, R_LEAVE, R_COUNT };
static ModMenu panel;
static int panel_open;
static double toast_t0 = -1;      /* when the panel was hidden (the "Y: show panel" hint), -1 none */
static const char *filter_name(int f) {
    static const char *const en[] = {"None", "Warm", "Cool", "Sepia", "Black and white", "Noir", "Faded"};
    static const char *const sp[] = {"Ninguno", "Cálido", "Frío", "Sepia", "Blanco y negro", "Noir", "Desvaído"};
    return f >= 0 && f < PH_FILTER_COUNT ? T(en[f], sp[f]) : "?";
}
static void panel_text(void) {
    static const char *const en[R_COUNT] = {"Take photo", "Resolution", "Supersampling", "Samples", "Focus distance", "Aperture (blur)",
                                            "Exposure", "Colour filter", "Saturation", "Vignette", "Grain", "Cinema bars", "Characters",
                                            "Camera speed", "Leave photo mode"};
    static const char *const sp[R_COUNT] = {"Tomar foto", "Resolución", "Supermuestreo", "Muestras", "Distancia de enfoque", "Apertura (desenfoque)",
                                            "Exposición", "Filtro de color", "Saturación", "Viñeta", "Grano", "Franjas de cine", "Personajes",
                                            "Velocidad de cámara", "Salir del modo foto"};
    char items[R_COUNT * MODMENU_TEXT_MAX] = "";
    for (int i = 0; i < R_COUNT; i++) {
        strcat(items, es ? sp[i] : en[i]);
        if (i + 1 < R_COUNT) strcat(items, "\n");
    }
    if (!panel.open) {
        modmenu_open(&panel, T("Photo mode", "Modo foto"), items, T("Y / B: hide panel (keys E / F)", "Y / B: ocultar panel (teclas E / F)"), 0, mono_ms());
        modmenu_values(&panel, 1);
        panel.side_panel = 1;
    }
    int rw = 0, rh = 0;
    hle_d3d_photo_output(&rw, &rh);
    char *v[R_COUNT];
    for (int i = 0; i < R_COUNT; i++) v[i] = panel.value[i], v[i][0] = 0;
    static const char *const res_en[] = {"Screen", "2x screen", "4K"}, *const res_sp[] = {"Pantalla", "2x pantalla", "4K"};
    snprintf(v[R_RES], MODMENU_VALUE_MAX, "%s", es ? res_sp[res_mode] : res_en[res_mode]);
    snprintf(v[R_SS], MODMENU_VALUE_MAX, "%dx", ss);
    snprintf(v[R_SAMPLES], MODMENU_VALUE_MAX, "%d", samples);
    snprintf(v[R_FOCUS], MODMENU_VALUE_MAX, "%.1f m", focus);
    if (aperture > 0) snprintf(v[R_APERTURE], MODMENU_VALUE_MAX, "%.2f", aperture);
    else snprintf(v[R_APERTURE], MODMENU_VALUE_MAX, "%s", T("Off", "No"));
    snprintf(v[R_EXPOSURE], MODMENU_VALUE_MAX, "%+.2f", look.exposure);
    snprintf(v[R_FILTER], MODMENU_VALUE_MAX, "%s", filter_name(look.filter));
    snprintf(v[R_SAT], MODMENU_VALUE_MAX, "%.0f%%", look.saturation * 100);
    snprintf(v[R_VIGNETTE], MODMENU_VALUE_MAX, "%.0f%%", look.vignette * 100);
    snprintf(v[R_GRAIN], MODMENU_VALUE_MAX, "%.0f%%", look.grain * 100);
    snprintf(v[R_BARS], MODMENU_VALUE_MAX, "%s", look.bars == PH_BARS_185 ? "1.85:1" : look.bars == PH_BARS_239 ? "2.39:1" : T("None", "No"));
    snprintf(v[R_CHARS], MODMENU_VALUE_MAX, "%s", hide_chars ? T("Hidden", "Ocultos") : T("Visible", "Visibles"));
    static const char *const sp_en[] = {"Slow", "Normal", "Fast"}, *const sp_sp[] = {"Lenta", "Normal", "Rápida"};
    snprintf(v[R_SPEED], MODMENU_VALUE_MAX, "%s", es ? sp_sp[speed] : sp_en[speed]);
    const int fw = res_mode == 2 ? 3840 : res_mode == 1 ? 2 * rw : rw, fh = rw > 0 ? (int)((int64_t)fw * rh / rw) : 0;
    switch (panel.sel) {
    case R_TAKE:
        snprintf(panel.note, sizeof panel.note, T("A: take a %dx%d photo (%d samples). It is saved in the Photos folder next to the game.",
                                                  "A: toma una foto de %dx%d (%d muestras). Se guarda en la carpeta Photos junto al juego."),
                 fw, fh, samples);
        break;
    case R_RES: snprintf(panel.note, sizeof panel.note, "%s", T("The photo's size: the screen's, twice it, or 4K (3840 wide) whatever your screen.", "El tamaño de la foto: el de la pantalla, el doble o 4K (3840 de ancho) sea cual sea tu pantalla.")); break;
    case R_SS: snprintf(panel.note, sizeof panel.note, "%s", T("Renders each photo pixel from 1, 4 or 16 pixels: finer detail, a longer capture.", "Calcula cada píxel de la foto con 1, 4 o 16 píxeles: más detalle, captura más larga.")); break;
    case R_SAMPLES: snprintf(panel.note, sizeof panel.note, "%s", T("Frames averaged into the photo: smoother edges and effects without noise.", "Imágenes promediadas en la foto: bordes suaves y efectos sin ruido.")); break;
    case R_FOCUS:
    case R_APERTURE: snprintf(panel.note, sizeof panel.note, "%s", T("Depth of field: what is at the focus distance stays sharp; a larger aperture blurs the rest more. In the photo only.", "Profundidad de campo: lo que está a la distancia de enfoque queda nítido; más apertura desenfoca más el resto. Solo en la foto.")); break;
    case R_SAT: case R_VIGNETTE: case R_GRAIN: snprintf(panel.note, sizeof panel.note, "%s", T("Applied to the saved photo (the preview does not show it).", "Se aplica a la foto guardada (la vista previa no lo muestra).")); break;
    case R_CHARS: snprintf(panel.note, sizeof panel.note, "%s", T("Hide the characters for a shot of the place alone (X).", "Oculta a los personajes para fotografiar solo el lugar (X).")); break;
    default:
        snprintf(panel.note, sizeof panel.note, "%s", T("Sticks: move and look. LT/RT: down/up. LB/RB: tilt. D-pad up/down: zoom. A: photo. Y: show / hide panel. B: leave.",
                                                        "Sticks: mover y mirar. LT/RT: bajar/subir. LB/RB: inclinar. Cruceta arriba/abajo: zoom. A: foto. Y: mostrar / ocultar panel. B: salir."));
    }
}
static float step_f(float v, int dir, float d, float lo, float hi) {
    v = roundf((v + dir * d) / d) * d;
    return v < lo ? lo : v > hi ? hi : v;
}
static void panel_adjust(int row, int dir) {
    static const int sample_steps[] = {16, 32, 64, 128, 256};
    switch (row) {
    case R_RES: res_mode = (res_mode + dir + 3) % 3; break;
    case R_SS: ss = dir > 0 ? (ss == 1 ? 2 : 4) : (ss == 4 ? 2 : 1); break;
    case R_SAMPLES: {
        int k = 0;
        while (k < 4 && sample_steps[k] < samples) k++;
        k += dir;
        samples = sample_steps[k < 0 ? 0 : k > 4 ? 4 : k];
        break;
    }
    case R_FOCUS: focus = dir > 0 ? (focus * 1.25f > 100 ? 100 : focus * 1.25f) : (focus / 1.25f < 0.5f ? 0.5f : focus / 1.25f); break;
    case R_APERTURE: aperture = step_f(aperture, dir, 0.02f, 0, 0.5f); break;
    case R_EXPOSURE: look.exposure = step_f(look.exposure, dir, 0.25f, -3, 3); break;
    case R_FILTER: look.filter = (look.filter + dir + PH_FILTER_COUNT) % PH_FILTER_COUNT; break;
    case R_SAT: look.saturation = step_f(look.saturation, dir, 0.1f, 0, 2); break;
    case R_VIGNETTE: look.vignette = step_f(look.vignette, dir, 0.1f, 0, 1); break;
    case R_GRAIN: look.grain = step_f(look.grain, dir, 0.1f, 0, 1); break;
    case R_BARS: look.bars = (look.bars + dir + PH_BARS_COUNT) % PH_BARS_COUNT; break;
    case R_CHARS: hide_chars = !hide_chars; break;
    case R_SPEED: speed = speed + dir < 0 ? 0 : speed + dir > 2 ? 2 : speed + dir; break;
    }
}

/* ---- entering and leaving (host thread) ---- */
static void finish_locked(uint32_t *gen) {
    (*gen)++;
    pthread_cond_broadcast(&cv);
}
static void leave(const char *why) {
    if (ph.stage == PH_CAPTURING) {
        ph_capture_end(&ph);
        ph_accum_free(&acc);
        pthread_mutex_lock(&mx);
        snprintf(last_error, sizeof last_error, "capture cancelled");
        finish_locked(&gen_capture);
        pthread_mutex_unlock(&mx);
    }
    const int restored = ph.stage != PH_OFF ? ph_exit(&ph) : 0;
    hle_d3d_photo_freeze(0);
    modmenu_close(&panel);
    panel_open = 0;
    toast_t0 = -1;
    swallow = 1;
    printf("[photo] left photo mode (%s): %d settings restored\n", why, restored > 0 ? restored : 0);
    fflush(stdout);
    pthread_mutex_lock(&mx);
    stage = S_OFF;
    finish_locked(&gen_exit);
    pthread_mutex_unlock(&mx);
}

/* After each presented frame of the running game. */
static void photo_tick(void) {
    if (next_settings) next_settings();
    pthread_mutex_lock(&mx);
    const int want = req_enter, st = stage;
    req_enter = 0;
    pthread_mutex_unlock(&mx);
    if (st == S_OFF && want) {
        float wvp[4][4];
        PhCamera c;
        const int has = hle_d3d_photo_camera(wvp);
        if (!has || ph_camera_from_wvp((const float (*)[4])wvp, &c)) {
            printf("[photo] not available here: this frame has no 3D camera (a menu, a movie or a loading screen)\n");
            if (has)
                for (int r = 0; r < 4; r++) printf("[photo]   camera row %d: %g %g %g %g (not a perspective camera)\n", r, wvp[r][0], wvp[r][1], wvp[r][2], wvp[r][3]);
            fflush(stdout);
            pthread_mutex_lock(&mx);
            snprintf(last_error, sizeof last_error, "no 3D camera in this frame");
            finish_locked(&gen_enter);
            pthread_mutex_unlock(&mx);
            return;
        }
        const int n = ph_enter(&ph);
        printf("[photo] entering photo mode: %d settings raised to their best for the photo\n", n);
        for (int i = 0; i < ph.nknobs; i++)
            if (ph.changed[i]) printf("[photo]   %s %g -> %g\n", knobs[i].name, ph.saved[i], knobs[i].photo);
        fflush(stdout);
        arm = ARM_FRAMES;
        pthread_mutex_lock(&mx);
        stage = S_ARMING;
        pthread_mutex_unlock(&mx);
    } else if (st == S_ARMING && --arm <= 0) {
        hle_d3d_photo_freeze(1);
        pthread_mutex_lock(&mx);
        stage = S_FREEZING;
        pthread_mutex_unlock(&mx);
    }
}

/* ---- capture (host thread) ---- */
static int exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0;
}
static void capture_begin(const char *path) {
    int rw = 0, rh = 0;
    hle_d3d_photo_output(&rw, &rh);
    PhPlanIn in = {rw, rh, ss, res_mode == 2 ? 3840 : res_mode == 1 ? 2 * rw : rw, samples, -1};
    PhPlan pre;   /* samples are per tile: every photo pixel gets as many */
    if (!ph_plan(&in, &pre)) in.frames = samples * pre.tiles_x * pre.tiles_y;
    if (ph_capture_begin(&ph, &in) || ph_accum_init(&acc, &ph.plan)) {
        if (ph.stage == PH_CAPTURING) ph_capture_end(&ph);
        printf("[photo] capture refused: %dx%d output, resolution %d, %dx supersampling\n", rw, rh, res_mode, ss);
        pthread_mutex_lock(&mx);
        snprintf(last_error, sizeof last_error, "capture plan refused (output %dx%d)", rw, rh);
        finish_locked(&gen_capture);
        pthread_mutex_unlock(&mx);
        return;
    }
    snprintf(req_path, sizeof req_path, "%s", path);
    warm = 0;
    tile_last = -1;
    printf("[photo] capture: %dx%d from %d frames (%dx%d tiles of %dx%d, %d samples each, %dx supersampling)\n", ph.plan.final_w,
           ph.plan.final_h, ph.plan.frames, ph.plan.tiles_x, ph.plan.tiles_y, ph.plan.crop_w, ph.plan.crop_h, ph.plan.per_tile, ph.plan.ss);
    fflush(stdout);
}
static void capture_finish(void) {
    const double t0 = mono_ms();
    const PhPlan p = ph.plan;
    float *rgb = malloc((size_t)p.final_w * p.final_h * 3 * sizeof *rgb);
    uint32_t *px = malloc((size_t)p.final_w * p.final_h * 4);
    char err[256] = "out of memory", path[1100];
    int ok = 0;
    path[0] = 0;
    if (rgb && px) {
        ph_accum_resolve(&acc, rgb);
        look.seed = (uint32_t)time(NULL);
        ph_finish(&look, rgb, p.final_w, p.final_h, px);
        if (req_path[0]) snprintf(path, sizeof path, "%s", req_path);
        else {
            const char *dir = hle_photo_dir;
            static char def[1024];
            if (!dir) snprintf(def, sizeof def, "%sPhotos", plat_program_dir()), dir = def;
            plat_mkdir(dir);
            const time_t now = time(NULL);
            struct tm tm;
#ifdef _WIN32
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            if (ph_photo_path(path, sizeof path, dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, exists) < 0)
                snprintf(err, sizeof err, "photo folder path too long"), path[0] = 0;
        }
        ok = path[0] && !ph_png_write(path, px, p.final_w, p.final_h, p.final_w, err, sizeof err);
    }
    if (ok) printf("[photo] saved %s (%dx%d, %d frames, %.0f ms to finish)\n", path, p.final_w, p.final_h, acc.added, mono_ms() - t0);
    else printf("[photo] ERROR: the photo was not saved: %s\n", err);
    fflush(stdout);
    free(rgb);
    free(px);
    pthread_mutex_lock(&mx);
    if (ok) snprintf(last_photo, sizeof last_photo, "%s", path), last_w = p.final_w, last_h = p.final_h, last_frames = acc.added, last_error[0] = 0;
    else snprintf(last_error, sizeof last_error, "%s", err);
    finish_locked(&gen_capture);
    pthread_mutex_unlock(&mx);
    ph_accum_free(&acc);
    ph_capture_end(&ph);
}
static void photo_pixels(const uint32_t *px, uint32_t w, uint32_t h) {
    if (ph.stage != PH_CAPTURING || warm >= 0) return;   /* warm >= 0: a settling replay (only warm < 0 counts) */
    (void)h;
    if ((int)w == ph.plan.out_w) ph_accum_add(&acc, &shot, px, (int)w);
}

/* ---- the replay view (host thread, before each replay while frozen) ---- */
static int pressed(const XPad *p, int analog_i, uint16_t bit) {
    if (bit) return (p->buttons & bit) && !(prev_buttons & bit);
    return p->analog[analog_i] > 0x40 && prev_analog[analog_i] <= 0x40;
}
static float stick(int16_t v) {
    const float f = v / 32767.0f, dz = 0.18f;
    return fabsf(f) < dz ? 0 : (f > 0 ? (f - dz) : (f + dz)) / (1 - dz);
}
static void fill_view(HleD3dPhotoView *v, const PhClipXform *t, float lens_x, float lens_y) {
    PhMat d, dr, m, ms;
    ph_freecam_view_delta(&cam, lens_x, lens_y, focus, &d);
    ph_mat_rotation_only(&d, &dr);
    ph_clip_matrix(&gcam, &d, cam.zoom, t, &m);
    ph_clip_matrix(&gcam, &dr, cam.zoom, t, &ms);
    memcpy(v->m, m.m, sizeof v->m);
    memcpy(v->m_sky, ms.m, sizeof v->m_sky);
    memcpy(v->d, d.m, sizeof v->d);
    v->sx = t->sx * cam.zoom, v->sy = t->sy * cam.zoom, v->ox = t->ox, v->oy = t->oy;
    v->hide_hud = 1;
    v->hide_chars = hide_chars;
}
static int photo_view(HleD3dPhotoView *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&mx);
    const int st = stage, want_exit = req_exit, want_cap = req_capture;
    char path[1024];
    snprintf(path, sizeof path, "%s", req_path);
    req_exit = 0;   /* req_capture is cleared once the capture starts (below) */
    const PhInput cm = cmd_move;
    const int cm_on = mono_ms() < cmd_move_until;
    pthread_mutex_unlock(&mx);
    if (st == S_OFF || st == S_ARMING) return 0;
    if (st == S_FREEZING) {   /* the game just froze: the camera starts at the frozen frame's */
        float wvp[4][4];
        if (!hle_d3d_photo_camera(wvp) || ph_camera_from_wvp((const float (*)[4])wvp, &gcam)) {
            pthread_mutex_lock(&mx);
            snprintf(last_error, sizeof last_error, "no 3D camera in the frozen frame");
            finish_locked(&gen_enter);
            pthread_mutex_unlock(&mx);
            leave("no 3D camera in the frozen frame");
            return 0;
        }
        const float up[3] = {gcam.view[0][2], gcam.view[1][2], gcam.view[2][2]};   /* world +z in the view */
        ph_freecam_reset(&cam, up, 6.0f);
        XPad p;
        uint32_t pkt;
        input_get_port(0, &p, &pkt);
        prev_buttons = p.buttons;   /* buttons held on the way in do nothing until released */
        memcpy(prev_analog, p.analog, sizeof prev_analog);
        panel_open = 1;
        panel_text();
        printf("[photo] photo mode on: camera at %.2f %.2f %.2f, projection %.3f %.3f\n", gcam.eye[0], gcam.eye[1], gcam.eye[2], gcam.px, gcam.py);
        fflush(stdout);
        pthread_mutex_lock(&mx);
        stage = S_ACTIVE;
        finish_locked(&gen_enter);
        pthread_mutex_unlock(&mx);
        last_view = 0;
    }
    if (want_exit) {
        leave("asked");
        return 0;
    }
    if (ph.stage == PH_CAPTURING) {   /* one capture frame per poll: no pacing */
        if (!ph_capture_next(&ph, &shot)) {
            capture_finish();
            return 0;
        }
        const int tile = shot.tile_y * ph.plan.tiles_x + shot.tile_x;
        if (tile != tile_last) tile_last = tile, warm = WARM_FRAMES;
        if (warm > 0) {   /* a new tile: replays that settle the history before its samples count */
            warm--;
            ph.frame--;   /* this shot again */
        } else {
            warm = -1;
        }
        fill_view(v, &shot.t, aperture * shot.lens_u, aperture * shot.lens_v);
        v->readback = 1;
        panel.progress = ph_capture_progress(&ph);
        if (panel.progress <= 0) panel.progress = 0.01f;
        return 1;
    }
    const double now = mono_ms();
    if (now - last_view < PREVIEW_MS) return 0;
    const float dt = last_view ? (float)((now - last_view) / 1000) : 0;
    last_view = now;
    XPad p;
    uint32_t pkt;
    input_get_port(0, &p, &pkt);
    PhInput in = {{0}, 0, 0, 0, 0, 1};
    in.move[0] = stick(p.lx), in.move[2] = stick(p.ly);
    in.move[1] = (p.analog[7] - p.analog[6]) / 255.0f;
    in.yaw = stick(p.rx), in.pitch = stick(p.ry);
    in.roll = (p.analog[4] > 0x40) - (p.analog[5] > 0x40);
    in.speed = (speed == 0 ? 0.3f : speed == 2 ? 3.0f : 1.0f) * (p.buttons & 0x40 ? 3.0f : 1.0f);
    if (panel_open) {   /* the d-pad drives the panel */
        int idx = 0;
        const unsigned c = modmenu_controls(p.buttons, p.analog, 0, 0) & ~(unsigned)MM_BACK;
        const int b = pressed(&p, 1, 0) || pressed(&p, 3, 0) || pressed(&p, 0, 0x20);   /* B, Y or Back close it */
        const int ev = modmenu_step(&panel, c, now, &idx);
        if (ev == MM_EV_ADJUST) panel_adjust(idx, panel.adjust);
        if (ev == MM_EV_CHOOSE) {
            if (idx == R_TAKE) capture_begin("");
            else if (idx == R_LEAVE) {
                leave("the panel");
                return 0;
            } else panel_adjust(idx, 1);
        }
        if (b) panel_open = 0, toast_t0 = now;   /* the whole picture; a hint says how to get the panel back */
        else panel_text();
    } else {
        in.zoom = (p.buttons & 1 ? 1.0f : 0) - (p.buttons & 2 ? 1.0f : 0);
        if (pressed(&p, 0, 0)) capture_begin("");
        else if (pressed(&p, 2, 0)) hide_chars = !hide_chars;
        else if (pressed(&p, 3, 0)) panel_open = 1, toast_t0 = -1, panel_text();
        else if (pressed(&p, 1, 0) || pressed(&p, 0, 0x20)) {
            leave("B / Back");
            return 0;
        }
        if (pressed(&p, 0, 0x80)) {   /* R3: the camera back where it started */
            const float up[3] = {cam.up[0], cam.up[1], cam.up[2]};
            ph_freecam_reset(&cam, up, 6.0f);
        }
    }
    prev_buttons = p.buttons;
    memcpy(prev_analog, p.analog, sizeof prev_analog);
    if (cm_on) {
        for (int k = 0; k < 3; k++) in.move[k] += cm.move[k];
        in.yaw += cm.yaw, in.pitch += cm.pitch, in.roll += cm.roll, in.zoom += cm.zoom;
    }
    ph_freecam_step(&cam, &in, dt);
    if (want_cap && ph.stage == PH_ACTIVE) {
        pthread_mutex_lock(&mx);
        req_capture = 0;
        pthread_mutex_unlock(&mx);
        capture_begin(path);
    }
    PhClipXform t;
    ph_clip_identity(&t);
    fill_view(v, &t, 0, 0);
    return 1;
}

/* The overlay: the panel, the cinema bars and the capture's progress. */
static int photo_menu_view(ModMenu *out, HleMenuArt *art) {
    if (stage != S_ACTIVE) return next_view ? next_view(out, art) : 0;
    *out = panel;
    out->open = 1;
    out->hidden = !panel_open || ph.stage == PH_CAPTURING;
    out->progress = ph.stage == PH_CAPTURING ? panel.progress : 0;
    if (!panel_open && ph.stage != PH_CAPTURING && toast_t0 >= 0) {   /* the hint after hiding the panel, then nothing at all */
        out->toast_alpha = ph_toast_alpha(mono_ms() - toast_t0);
        snprintf(out->toast, sizeof out->toast, "%s", T("Y (E): show panel", "Y (E): mostrar panel"));
        if (out->toast_alpha <= 0) toast_t0 = -1;
    }
    int rw = 0, rh = 0;
    hle_d3d_photo_output(&rw, &rh);
    out->bars = rh > 0 ? (float)ph_bars_rows(look.bars, rw, rh) / rh : 0;
    memset(art, 0, sizeof *art);
    hle_modmenu_font(art);
    return 1;
}

/* ---- entry points from other threads ---- */
static int input_filter(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly) {
    if (swallow) {   /* the buttons that left photo mode stay with it until released */
        if (!(buttons & 0x3f) && !analog[0] && !analog[1] && !analog[3] && !analog[4]) swallow = 0;
        return 1;
    }
    if ((buttons & 0x20) && analog[4] > 0x40) {   /* Back + RB (black) held */
        const double t = mono_ms();
        if (!hold_t0) hold_t0 = t;
        else if (t - hold_t0 >= HOLD_MS) {
            hold_t0 = 0;
            pthread_mutex_lock(&mx);
            if (stage == S_OFF) req_enter = 1;
            pthread_mutex_unlock(&mx);
            printf("[photo] Back + RB: photo mode requested\n");
            fflush(stdout);
        }
    } else {
        hold_t0 = 0;
    }
    return next_filter ? next_filter(buttons, analog, lx, ly) : 0;
}
static void key_hook(int key, int down) {
    if (key == INPUT_KEY_F1 + 8 && down && input_key_control(key) < 0) {   /* F9 unless the player bound it */
        pthread_mutex_lock(&mx);
        if (stage == S_OFF) req_enter = 1;
        else if (stage == S_ACTIVE) req_exit = 1;
        pthread_mutex_unlock(&mx);
    }
    if (next_key) next_key(key, down);
}
static int escape_hook(void) {
    pthread_mutex_lock(&mx);
    const int active = stage != S_OFF;
    if (active) req_exit = 1;
    pthread_mutex_unlock(&mx);
    return active ? 1 : next_escape ? next_escape() : 0;
}
int hle_photo_active(void) {
    pthread_mutex_lock(&mx);
    const int a = stage != S_OFF;
    pthread_mutex_unlock(&mx);
    return a;
}

/* Waits (mx held) until *gen moves past g0; 0, or -1 on the timeout. */
static int wait_gen(const uint32_t *gen, uint32_t g0) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += CMD_TIMEOUT_S;
    while (*gen == g0)
        if (pthread_cond_timedwait(&cv, &mx, &dl)) return *gen == g0 ? -1 : 0;
    return 0;
}
static const char *stage_name(int s) { return s == S_ACTIVE ? "active" : s == S_OFF ? "off" : "entering"; }
static size_t reply_ok(char *r, size_t cap, const char *what) {
    int n = snprintf(r, cap, "{\"ok\":true,\"cmd\":\"photo\",\"state\":\"%s\"%s%s}\n", stage_name(stage), what && *what ? "," : "", what ? what : "");
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
static size_t reply_err(char *r, size_t cap, const char *e) {
    int n = snprintf(r, cap, "{\"ok\":false,\"cmd\":\"photo\",\"error\":\"%s\"}\n", e);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
static int set_value(const char *k, const char *val) {
    const float f = (float)atof(val);
    if (!strcmp(k, "resolution")) res_mode = !strcmp(val, "4k") ? 2 : !strcmp(val, "2x") ? 1 : 0;
    else if (!strcmp(k, "ss")) ss = f >= 4 ? 4 : f >= 2 ? 2 : 1;
    else if (!strcmp(k, "samples")) samples = f < 1 ? 1 : f > 1024 ? 1024 : (int)f;
    else if (!strcmp(k, "exposure")) look.exposure = f < -3 ? -3 : f > 3 ? 3 : f;
    else if (!strcmp(k, "saturation")) look.saturation = f < 0 ? 0 : f > 2 ? 2 : f;
    else if (!strcmp(k, "vignette")) look.vignette = f < 0 ? 0 : f > 1 ? 1 : f;
    else if (!strcmp(k, "grain")) look.grain = f < 0 ? 0 : f > 1 ? 1 : f;
    else if (!strcmp(k, "focus")) focus = f < 0.5f ? 0.5f : f > 100 ? 100 : f;
    else if (!strcmp(k, "aperture")) aperture = f < 0 ? 0 : f > 0.5f ? 0.5f : f;
    else if (!strcmp(k, "characters")) hide_chars = !strcmp(val, "off");
    else if (!strcmp(k, "bars")) look.bars = !strcmp(val, "2.39") ? PH_BARS_239 : !strcmp(val, "1.85") ? PH_BARS_185 : PH_BARS_NONE;
    else if (!strcmp(k, "filter")) {
        int i = 0;
        while (i < PH_FILTER_COUNT && strcmp(ph_filter_key(i), val)) i++;
        if (i == PH_FILTER_COUNT) return -1;
        look.filter = i;
    } else return -1;
    return 0;
}
size_t hle_photo_command(const char *args, char *reply, size_t cap) {
    char verb[16] = "", a[1024] = "", b[64] = "";
    while (*args == ' ') args++;
    sscanf(args, "%15s %1023s %63s", verb, a, b);
    char extra[1400];
    pthread_mutex_lock(&mx);
    size_t n;
    if (!strcmp(verb, "enter")) {
        if (stage == S_ACTIVE) n = reply_ok(reply, cap, NULL);
        else {
            const uint32_t g0 = gen_enter;
            last_error[0] = 0;
            req_enter = 1;
            if (wait_gen(&gen_enter, g0)) n = reply_err(reply, cap, "timed out entering photo mode");
            else if (stage != S_ACTIVE) n = reply_err(reply, cap, last_error[0] ? last_error : "not entered");
            else n = reply_ok(reply, cap, NULL);
        }
    } else if (!strcmp(verb, "exit")) {
        if (stage == S_OFF) n = reply_ok(reply, cap, NULL);
        else {
            const uint32_t g0 = gen_exit;
            req_exit = 1;
            n = wait_gen(&gen_exit, g0) ? reply_err(reply, cap, "timed out leaving photo mode") : reply_ok(reply, cap, NULL);
        }
    } else if (!strcmp(verb, "status")) {
        snprintf(extra, sizeof extra, "\"last_photo\":\"%s\",\"camera\":[%.3f,%.3f,%.3f],\"zoom\":%.3f", last_photo, cam.pos[0], cam.pos[1], cam.pos[2],
                 cam.zoom);
        n = reply_ok(reply, cap, extra);
    } else if (!strcmp(verb, "capture")) {
        if (stage != S_ACTIVE) n = reply_err(reply, cap, "not in photo mode (photo enter first)");
        else {
            const uint32_t g0 = gen_capture;
            snprintf(req_path, sizeof req_path, "%s", a);
            last_error[0] = 0;
            req_capture = 1;
            if (wait_gen(&gen_capture, g0)) n = reply_err(reply, cap, "timed out capturing");
            else if (last_error[0]) n = reply_err(reply, cap, last_error);
            else {
                snprintf(extra, sizeof extra, "\"path\":\"%s\",\"width\":%d,\"height\":%d,\"frames\":%d", last_photo, last_w, last_h, last_frames);
                n = reply_ok(reply, cap, extra);
            }
        }
    } else if (!strcmp(verb, "set")) {
        n = set_value(a, b) ? reply_err(reply, cap, "unknown photo setting or value") : reply_ok(reply, cap, NULL);
    } else if (!strcmp(verb, "move")) {
        float f[7] = {0};
        sscanf(args, "%*s %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]);
        cmd_move = (PhInput){{f[0], f[1], f[2]}, f[3], f[4], f[5], f[6], 1};
        cmd_move_until = mono_ms() + 1000;
        n = reply_ok(reply, cap, NULL);
    } else {
        n = reply_err(reply, cap, "usage: photo enter|exit|status|capture [PATH]|set KEY VALUE|move R U F [YAW PITCH ROLL ZOOM]");
    }
    pthread_mutex_unlock(&mx);
    return n;
}

void hle_photo_install(const char *lang) {
    es = lang && !strcmp(lang, "es");
    ph_init(&ph, knobs, (int)(sizeof knobs / sizeof knobs[0]));
    ph_look_default(&look);
    next_filter = input_menu_filter;
    input_menu_filter = input_filter;
    next_key = input_key_hook;
    input_key_hook = key_hook;
    next_escape = video_escape_hook;
    video_escape_hook = escape_hook;
    next_settings = hle_d3d_settings_hook;
    hle_d3d_settings_hook = photo_tick;
    next_view = hle_d3d_menu_view;
    hle_d3d_menu_view = photo_menu_view;
    hle_d3d_photo_view = photo_view;
    hle_d3d_photo_pixels = photo_pixels;
    hle_d3d_photo_prepare_hook = &hle_photo_prepare;
    printf("[photo] photo mode: F9 or Back + RB held (%s; photos in %s%s)\n", es ? "es" : "en", hle_photo_dir ? hle_photo_dir : plat_program_dir(),
           hle_photo_dir ? "" : "Photos");
}
