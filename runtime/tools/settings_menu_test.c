/* settings_menu: tabs, the preset row, value rows with their
 * badges, greyed rows, adjusting through the ops (record + live), the restart rows and dialog, the
 * Mods tab, the controls and the texts. Fake ops; no game data. */
#include "../src/settings_menu.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static SmValues start, now;
static int sets, lives, restarts;
static char last_key[64], last_value[256];
static const SmValues *f_start(void) { return &start; }
static const SmValues *f_now(void) { return &now; }
static int f_set(const char *k, const char *v) {
    sets++;
    snprintf(last_key, sizeof last_key, "%s", k);
    snprintf(last_value, sizeof last_value, "%s", v);
    return sm_set(&now, k, v);
}
static int is_live(const char *k) { return !strcmp(k, "music_volume") || !strcmp(k, "taa_sharpness"); }
static int f_live(const char *k, const char *v) { (void)v; if (is_live(k)) { lives++; return 1; } return 0; }
static int f_can_live(const char *k, const char *v) { (void)v; return is_live(k); }
static void f_restart(void) { restarts++; }

static const char presets[] =
    "# warriors-launcher presets\n"
    "\n[preset laptop]\nbloom = false\nssao = false\n"
    "\n[preset balanced]\nbloom = true\nssao = false\n"
    "\n[preset ultra]\nbloom = true\nssao = true\n";

static SettingsMenu s;
static double t = 1000;
/* A press and its release, 50 ms apart. */
static int press(unsigned c) {
    int ev = smenu_step(&s, c, t += 50);
    smenu_step(&s, 0, t += 50);
    return ev;
}
static int find_row(const char *label) {
    for (int i = 0; i < s.m.n; i++)
        if (!strcmp(s.m.item[i], label)) return i;
    return -1;
}
static void go_row(int r) {
    while (s.m.sel != r) press(s.m.sel < r ? MM_DOWN : MM_UP);
}
static void go_tab(int tab) {
    while (s.tab != tab) press(MM_TAB_NEXT);
}

static void reset_values(void) {
    sm_values_free(&start);
    sm_values_free(&now);
    sm_values_init(&start);
    sm_set(&start, "bloom", "true");   /* the balanced preset */
    sm_set(&start, "taa", "true");
    sm_set(&start, "mods", "rocola,dirty-clothes");
    sm_values_copy(&now, &start);
    sets = lives = restarts = 0;
}

static SmenuOps ops = {f_start, f_now, f_set, f_live, f_can_live, f_restart, 1, 1, 0};

static void test_open_tabs(void) {
    reset_values();
    smenu_init(&s, &ops, "es", presets);
    CHECK(!smenu_is_open(&s), "closed until opened");
    smenu_open(&s, t);
    CHECK(smenu_is_open(&s) && s.m.values && s.m.ntabs == SMENU_TABS, "open: a value menu with six tabs");
    CHECK(!strcmp(s.m.tabs[0], "Pantalla") && !strcmp(s.m.tabs[1], "Efectos") && !strcmp(s.m.tabs[2], "Audio") &&
          !strcmp(s.m.tabs[3], "Controles") && !strcmp(s.m.tabs[4], "Juego") && !strcmp(s.m.tabs[5], "Mods"),
          "tabs: Pantalla / Efectos / Audio / Controles / Juego / Mods");
    CHECK(!strcmp(s.m.item[0], "Preajuste") && !strcmp(s.m.value[0], "Equilibrado"), "Pantalla: Preajuste first, the detected preset");
    CHECK(find_row("Antialiasing (MSAA)") > 0 && find_row("Resplandor") < 0, "Pantalla: display rows only");
    smenu_step(&s, 0, t += 50);
    press(MM_TAB_NEXT);
    CHECK(s.tab == 1 && find_row("Resplandor") >= 0 && find_row("Preajuste") < 0, "RB / E: the Efectos tab");
    press(MM_TAB_PREV);
    press(MM_TAB_PREV);
    CHECK(s.tab == 5 && find_row("rocola") >= 0 && find_row("dirty-clothes") >= 0, "LB / Q wraps to Mods: one row per mod");
    int texture_rows = 0;
    for (int tab = 0; tab < SMENU_TABS; tab++) {
        go_tab(tab);
        texture_rows += find_row("Paquete de texturas") >= 0 || find_row("Texture pack") >= 0;
    }
    CHECK(texture_rows == 0, "free-text settings (packs, resolution) are not menu rows");
}

static void test_adjust_live_and_queued(void) {
    reset_values();
    smenu_init(&s, &ops, "es", presets);
    smenu_open(&s, t);
    smenu_step(&s, 0, t += 50);
    go_tab(2);
    int r = find_row("Volumen de la música");
    CHECK(r >= 0 && !strcmp(s.m.value[r], "0 dB") && !strcmp(s.m.badge[r], "al instante"), "Audio: music volume 0 dB, live badge");
    go_row(r);
    CHECK(press(MM_RIGHT) == MM_EV_ADJUST && !strcmp(last_key, "music_volume") && !strcmp(last_value, "2") && lives == 1,
          "right: music volume +2 dB recorded and applied live");
    CHECK(!strcmp(s.m.value[r], "+2 dB") && smenu_queued_count(&s) == 0 && find_row("Reiniciar ahora") < 0,
          "a live change: new value shown, nothing waits for the restart");
    go_tab(1);
    r = find_row("Resplandor");
    CHECK(r >= 0 && !strcmp(s.m.value[r], "Sí") && !strcmp(s.m.badge[r], "al reiniciar"), "Efectos: bloom on, badge al reiniciar");
    go_row(r);
    press(MM_LEFT);
    CHECK(!strcmp(last_key, "bloom") && !strcmp(last_value, "false") && !strcmp(s.m.value[r], "No") && smenu_queued_count(&s) == 1,
          "bloom off: recorded, queued for the restart");
    int p = find_row("Aplicar al reiniciar (1 cambio)"), rs = find_row("Reiniciar ahora");
    CHECK(p >= 0 && rs == p + 1 && rs == s.m.n - 1, "the tab ends with Aplicar al reiniciar (1 cambio) and Reiniciar ahora");
    int b = find_row("Intensidad del resplandor");
    CHECK(b >= 0 && s.m.grey[b], "bloom strength greyed once bloom is off");
    go_row(b);
    sets = 0;
    CHECK(press(MM_RIGHT) == MM_EV_NONE && sets == 0, "a greyed row does not change");
    CHECK(strstr(s.m.note, "Resplandor") != NULL, "its note names the prerequisite");
    go_row(p);
    CHECK(strstr(s.m.note, "Resplandor") != NULL, "the pending row's note lists the queued change");
    go_row(r);
    press(MM_RIGHT);
    CHECK(smenu_queued_count(&s) == 0 && find_row("Reiniciar ahora") < 0, "bloom back to its start value: nothing queued");
    go_tab(0);
    CHECK(!strcmp(s.m.value[0], "Equilibrado"), "the preset is detected again");
    int h = -1;
    go_tab(1);
    h = find_row("Cabello mejorado");
    CHECK(h >= 0 && s.m.grey[h], "hair greyed without character lighting");
    go_row(h);
    CHECK(strstr(s.m.note, "Iluminación de personajes") != NULL, "hair's note: needs Iluminación de personajes");
}

static void test_restart(void) {
    reset_values();
    smenu_init(&s, &ops, "es", presets);
    smenu_open(&s, t);
    smenu_step(&s, 0, t += 50);
    go_tab(0);
    int r = find_row("Antialiasing (MSAA)");
    go_row(r);
    press(MM_RIGHT);
    CHECK(!strcmp(last_key, "msaa") && !strcmp(last_value, "2") && !strcmp(s.m.badge[r], "al reiniciar"), "MSAA 2x queued");
    int rs = find_row("Reiniciar ahora");
    go_row(rs);
    CHECK(press(MM_OK) == MM_EV_CHOOSE && s.m.confirm_open && restarts == 0, "Reiniciar ahora asks first");
    CHECK(press(MM_BACK) == MM_EV_CONFIRM && !s.m.confirm_open && restarts == 0 && smenu_is_open(&s), "B: no restart");
    press(MM_OK);
    CHECK(press(MM_OK) == MM_EV_CONFIRM && restarts == 1, "yes: restart");
    CHECK(press(MM_BACK) == MM_EV_CANCEL && !smenu_is_open(&s), "B closes the menu");
}

static void test_preset(void) {
    reset_values();
    smenu_init(&s, &ops, "es", presets);
    smenu_open(&s, t);
    smenu_step(&s, 0, t += 50);
    go_row(0);
    press(MM_RIGHT);
    CHECK(!strcmp(s.m.value[0], "Ultra") && !strcmp(sm_get(&now, "ssao"), "true") && smenu_detect_preset(&s, &now) == 2,
          "preset right: Ultra sets its keys");
    press(MM_LEFT);
    press(MM_LEFT);
    CHECK(!strcmp(s.m.value[0], "Rendimiento") && !strcmp(sm_get(&now, "bloom"), "false"), "preset left twice: Rendimiento (laptop)");
    go_row(find_row("Antialiasing (MSAA)"));
    press(MM_RIGHT);
    CHECK(!strcmp(s.m.value[0], "Rendimiento"), "MSAA is not a preset key of this file: the preset still matches");
    go_tab(1);
    go_row(find_row("Oclusión ambiental"));
    press(MM_RIGHT);
    go_tab(0);
    CHECK(!strcmp(s.m.value[0], "Personalizado"), "ssao on without bloom: no preset matches, Personalizado");
    SettingsMenu none;
    smenu_init(&none, &ops, "es", NULL);
    smenu_open(&none, t);
    CHECK(strcmp(none.m.item[0], "Preajuste") != 0, "no presets file: no Preajuste row");
    smenu_free(&none);
}

static void test_mods_tab(void) {
    reset_values();
    smenu_init(&s, &ops, "es", presets);
    smenu_open(&s, t);
    smenu_step(&s, 0, t += 50);
    go_tab(5);
    int r = find_row("rocola");
    CHECK(!strcmp(s.m.value[r], "Sí") && !strcmp(s.m.badge[r], "al reiniciar"), "mods: on, al reiniciar");
    go_row(r);
    press(MM_LEFT);
    CHECK(!strcmp(last_key, "mods") && !strcmp(last_value, "dirty-clothes") && !strcmp(s.m.value[r], "No"), "a mod off: the list without it");
    press(MM_RIGHT);
    CHECK(!strcmp(last_value, "rocola,dirty-clothes"), "back on: the start order");
}

static void test_no_mods(void) {
    reset_values();
    sm_set(&start, "mods", "");
    sm_set(&now, "mods", "");
    smenu_init(&s, &ops, "es", presets);
    smenu_open(&s, t);
    smenu_step(&s, 0, t += 50);
    go_tab(4);
    go_tab(5);
    CHECK(s.m.n == 1 && !strcmp(s.m.item[0], "Ningún mod activo") && find_row("Duración del aerosol") < 0,
          "Mods without mods: one info row, not the previous tab's rows");
    sets = 0;
    press(MM_RIGHT);
    press(MM_OK);
    CHECK(sets == 0 && smenu_is_open(&s), "the info row changes nothing");
}

static void test_badges_and_english(void) {
    reset_values();
    SmenuOps o = ops;
    o.live_tiers = 1;
    smenu_init(&s, &o, "es", presets);
    CHECK(!strcmp(smenu_badge(&s, sm_find("bloom"), "false"), "breve pausa"), "live tiers: class b = breve pausa");
    CHECK(!strcmp(smenu_badge(&s, sm_find("msaa"), "4"), "pantalla negra ~1 s"), "live tiers: class c = pantalla negra ~1 s");
    CHECK(!strcmp(smenu_badge(&s, sm_find("mods"), "x"), "al reiniciar"), "class d = al reiniciar");
    CHECK(!strcmp(smenu_badge(&s, sm_find("music_volume"), "2"), "al instante"), "live = al instante");
    CHECK(!strcmp(smenu_badge(&s, sm_find("rumble"), "false"), "al reiniciar"), "class a without a live setter = al reiniciar");
    smenu_init(&s, &ops, "en", presets);
    smenu_open(&s, t);
    CHECK(!strcmp(s.m.tabs[0], "Display") && !strcmp(s.m.tabs[1], "Effects") && !strcmp(s.m.item[0], "Preset") &&
          !strcmp(s.m.value[0], "Balanced"), "English texts");
    CHECK(!strcmp(smenu_badge(&s, sm_find("msaa"), "4"), "on restart") && !strcmp(smenu_badge(&s, sm_find("music_volume"), "2"), "live"),
          "English badges");
    char b[64];
    CHECK(!strcmp(smenu_value_label("bloom", "true", 0, b, sizeof b), "On") && !strcmp(smenu_value_label("bloom", "false", 1, b, sizeof b), "No") &&
          !strcmp(smenu_value_label("sfx_volume", "-6", 1, b, sizeof b), "-6 dB") &&
          !strcmp(smenu_value_label("taa_upscale", "quality", 1, b, sizeof b), "Calidad") &&
          !strcmp(smenu_value_label("taa_sharpness", "-1", 1, b, sizeof b), "Auto") &&
          !strcmp(smenu_value_label("msaa", "4", 1, b, sizeof b), "4x") && !strcmp(smenu_value_label("msaa", "0", 1, b, sizeof b), "No"),
          "value labels");
    CHECK(smenu_adjust(sm_find("taa_sharpness"), "-1", 1, b, sizeof b) == 0 && !strcmp(b, "0") &&
          smenu_adjust(sm_find("taa_sharpness"), "0", -1, b, sizeof b) == 0 && !strcmp(b, "-1") &&
          smenu_adjust(sm_find("taa_sharpness"), "0.5", 1, b, sizeof b) == 0 && !strcmp(b, "0.55") &&
          smenu_adjust(sm_find("taa_sharpness"), "1", 1, b, sizeof b) == -1, "float adjust: default, steps, end");
    CHECK(smenu_adjust(sm_find("saturation"), "1", 1, b, sizeof b) == 0 && !strcmp(b, "1.1") &&
              smenu_adjust(sm_find("saturation"), "0.5", -1, b, sizeof b) == -1 && smenu_adjust(sm_find("saturation"), "2", 1, b, sizeof b) == -1,
          "saturation adjust: 0.1 steps, stops at 0.5 and 2 (no \"Auto\")");
    CHECK(!strcmp(smenu_value_label("hdr", "on", 1, b, sizeof b), "Activado") && !strcmp(smenu_value_label("hdr", "auto", 0, b, sizeof b), "Auto") &&
              !strcmp(smenu_value_label("hdr_peak", "1000", 0, b, sizeof b), "1000 nits"),
          "hdr labels: on, auto, nits");
    CHECK(smenu_adjust(sm_find("hdr"), "off", 1, b, sizeof b) == 0 && !strcmp(b, "auto") && smenu_adjust(sm_find("hdr_peak"), "1000", 1, b, sizeof b) == 0 &&
              !strcmp(b, "1480"),
          "hdr adjust: off -> auto, peak in 480-nit steps (%s)", b);
    CHECK(smenu_adjust(sm_find("msaa"), "8", 1, b, sizeof b) == -1 && smenu_adjust(sm_find("msaa"), "0", 1, b, sizeof b) == 0 && !strcmp(b, "2") &&
          smenu_adjust(sm_find("texture_pack"), "", 1, b, sizeof b) == -1, "list adjust; free text not adjustable");
    smenu_free(&s);
}

static void test_controls(void) {
    uint8_t an[8] = {0};
    CHECK(smenu_controls(0x1, an, 0, 0, 0, 0) == MM_UP && smenu_controls(0x8, an, 0, 0, 0, 0) == MM_RIGHT, "controls: d-pad");
    an[5] = 0xff;
    CHECK(smenu_controls(0, an, 0, 0, 0, 0) == MM_TAB_PREV, "controls: LB (white) = previous tab");
    an[5] = 0, an[4] = 0xff;
    CHECK(smenu_controls(0, an, 0, 0, 0, 0) == MM_TAB_NEXT, "controls: RB (black) = next tab");
    an[4] = 0, an[3] = 0xff;
    CHECK(smenu_controls(0, an, 0, 0, 0, 1) == MM_TAB_NEXT, "controls: E (also the game's Y) = next tab, Y is not OK");
    an[3] = 0;
    CHECK(smenu_controls(0, an, 0, 0, 1, 0) == MM_TAB_PREV, "controls: Q = previous tab");
    an[0] = 0xff;
    CHECK(smenu_controls(0x10, an, 0, 0, 0, 0) == MM_OK && smenu_controls(0x20, (uint8_t[8]){0}, 0, 0, 0, 0) == MM_BACK, "controls: A / Start, Back");
}

int main(void) {
    sm_values_init(&start);
    sm_values_init(&now);
    test_open_tabs();
    test_adjust_live_and_queued();
    test_restart();
    test_preset();
    test_mods_tab();
    test_no_mods();
    test_badges_and_english();
    test_controls();
    smenu_free(&s);
    sm_values_free(&start);
    sm_values_free(&now);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
