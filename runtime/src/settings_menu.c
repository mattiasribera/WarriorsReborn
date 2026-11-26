/* The in-game settings menu: see settings_menu.h. */
#include "settings_menu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- texts ---- */

/* The launcher's UI strings (X(id, English, Spanish)): the same labels and descriptions as its pages. */
static const struct {
    const char *id, *en, *es;
} i18n[] = {
#define X(id, en, es) {#id, en, es},
#include "../../launcher/src/i18n_strings.h"
#undef X
};

const char *smenu_text(const char *id, int es) {
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof i18n / sizeof i18n[0]; i++)
        if (!strcmp(i18n[i].id, id)) return es ? i18n[i].es : i18n[i].en;
    return NULL;
}

/* The menu's own texts. */
enum {
    T_TAB_DISPLAY, T_TAB_EFFECTS, T_TAB_AUDIO, T_TAB_CONTROLS, T_TAB_GAME, T_TAB_MODS,
    T_TITLE, T_FOOTER, T_ON, T_OFF, T_AUTO, T_LIVE, T_PASS, T_REINIT, T_RESTART_BADGE,
    T_PENDING, T_RESTART_ROW, T_RESTART_Q, T_YES, T_NO, T_NEEDS, T_PENDING_NONE, T_PENDING_SAVED, T_PENDING_SESSION,
    T_RESTART_NOTE, T_RESTART_NOTE_EXIT, T_PRESET_NOTE, T_MOD_NOTE, T_PENDING_EXIT, T_NO_MODS, T_NO_MODS_NOTE, T_COUNT
};
static const char *const texts[T_COUNT][2] = {
    {"Display", "Pantalla"},
    {"Effects", "Efectos"},
    {"Audio", "Audio"},
    {"Controls", "Controles"},
    {"Game", "Juego"},
    {"Mods", "Mods"},
    {"Settings", "Ajustes"},
    {"LB/RB or Q/E: tab   Left/Right: change   B or Esc: close", "LB/RB o Q/E: pestaña   Izq./Der.: cambiar   B o Esc: cerrar"},
    {"On", "Sí"},
    {"Off", "No"},
    {"Auto", "Auto"},
    {"live", "al instante"},
    {"short pause", "breve pausa"},
    {"~1 s black", "pantalla negra ~1 s"},
    {"on restart", "al reiniciar"},
    {"Apply on restart (%d change%s)", "Aplicar al reiniciar (%d cambio%s)"},
    {"Restart now", "Reiniciar ahora"},
    {"Restart the game now to apply the changes?", "¿Reiniciar el juego ahora para aplicar los cambios?"},
    {"Restart", "Reiniciar"},
    {"Cancel", "Cancelar"},
    {"Depends on \"%s\". ", "Depende de «%s». "},
    {"Nothing waits for a restart.", "Nada espera un reinicio."},
    {"They are saved to your launcher profile: ", "Se guardan en tu perfil del launcher: "},
    {"Game started without the launcher: these changes last this session only: ", "Juego abierto sin el launcher: estos cambios duran solo esta sesión: "},
    {"The game closes and the launcher opens it again with your changes. You return to the main menu: anything not saved is lost.",
     "El juego se cierra y el launcher lo vuelve a abrir con tus cambios. Vuelves al menú principal: lo que no hayas guardado se pierde."},
    {"The game closes with your changes saved; play again from the launcher to use them.",
     "El juego se cierra con tus cambios guardados; vuelve a JUGAR desde el launcher para usarlos."},
    {"Applies one of the launcher's presets to every setting it covers.", "Aplica uno de los preajustes del launcher a todos los ajustes que cubre."},
    {"Turns this mod on or off from the next start.", "Activa o desactiva este mod desde el próximo inicio."},
    {"Play again from the launcher to apply them.", "Vuelve a JUGAR desde el launcher para aplicarlos."},
    {"No mods running", "Ningún mod activo"},
    {"Choose mods on the launcher's Mods page.", "Elige mods en la página Mods del launcher."},
};
static const char *tx(const SettingsMenu *s, int id) { return texts[id][s->es ? 1 : 0]; }

/* Labels of enum values: value, English, Spanish. */
static const char *const value_labels[][3] = {
    {"off", "Off", "No"}, {"native", "Native", "Nativo"}, {"ultra", "Ultra quality", "Ultra calidad"},
    {"quality", "Quality", "Calidad"}, {"balanced", "Balanced", "Equilibrado"}, {"performance", "Performance", "Rendimiento"},
    {"low", "Low", "Baja"}, {"medium", "Medium", "Media"}, {"high", "High", "Alta"}, {"classic", "Classic", "Clásico"},
    {"gtao", "GTAO", "GTAO"}, {"sssr", "SSSR", "SSSR"}, {"auto", "Auto", "Auto"}, {"mailbox", "Mailbox", "Mailbox"},
    {"fifo", "V-Sync", "V-Sync"}, {"immediate", "Immediate", "Inmediato"}, {"readback", "Readback", "Copia (readback)"},
    {"original", "Original", "Original"}, {"hd", "HD", "HD"}, {"hd-glass", "HD glass", "HD vidrio"}, {"glass", "Glass", "Vidrio"},
    {"xbox", "Xbox", "Xbox"}, {"ps-color", "PlayStation colour", "PlayStation color"}, {"ps-black", "PlayStation black", "PlayStation negro"},
    {"ps-white", "PlayStation white", "PlayStation blanco"}, {"kbm", "Keyboard and mouse", "Teclado y ratón"}, {"taa", "TAA", "TAA"},
    {"fsr3", "FSR 3", "FSR 3"}, {"en", "English", "Inglés"}, {"es", "Spanish", "Español"}, {"fr", "French", "Francés"},
    {"de", "German", "Alemán"}, {"it", "Italian", "Italiano"}, {"tl", "Top left", "Arriba izquierda"}, {"tr", "Top right", "Arriba derecha"},
    {"bl", "Bottom left", "Abajo izquierda"}, {"br", "Bottom right", "Abajo derecha"}, {"on", "On", "Activado"},
};

const char *smenu_value_label(const char *key, const char *value, int es, char *buf, size_t n) {
    const SmSetting *st = sm_setting(sm_find(key));
    const char *on = texts[T_ON][es != 0], *off = texts[T_OFF][es != 0], *aut = texts[T_AUTO][es != 0];
    if (!value) value = "";
    snprintf(buf, n, "%s", value);
    if (!st) return buf;
    const size_t kl = strlen(key);
    if (st->type == SM_BOOL) snprintf(buf, n, "%s", sm_truthy(value) ? on : off);
    else if (kl > 7 && !strcmp(key + kl - 7, "_volume")) {
        const int db = atoi(value);
        snprintf(buf, n, db > 0 ? "+%d dB" : "%d dB", db);
    } else if (!strcmp(key, "msaa") || !strcmp(key, "aniso") || !strcmp(key, "supersample") || !strcmp(key, "render_scale")) {
        const int v = atoi(value);
        if (v <= 1) snprintf(buf, n, "%s", off);
        else snprintf(buf, n, "%dx", v);
    } else if (!strcmp(key, "hdr_peak") || !strcmp(key, "hdr_paper_white")) snprintf(buf, n, "%d nits", atoi(value));
    else if (!strcmp(key, "frames_in_flight") && atoi(value) == 0) snprintf(buf, n, "%s", aut);
    else if (st->type == SM_FLOAT && strtod(value, NULL) < 0) snprintf(buf, n, "%s", aut);
    else if (st->type == SM_ENUM) {
        if (!value[0]) snprintf(buf, n, "%s", aut);
        for (size_t i = 0; i < sizeof value_labels / sizeof value_labels[0]; i++)
            if (!strcmp(value_labels[i][0], value)) snprintf(buf, n, "%s", value_labels[i][es ? 2 : 1]);
    }
    return buf;
}

/* ---- values ---- */

/* The index'th '|' choice of a domain into buf; 0, or -1 past the end. */
static int choice(const char *dom, int index, char *buf, size_t n) {
    const char *p = dom;
    for (int k = 0; k < index; k++) {
        p = strchr(p, '|');
        if (!p) return -1;
        p++;
    }
    const char *e = strchr(p, '|');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    snprintf(buf, n, "%.*s", (int)len, p);
    return 0;
}

static double nice_step(double raw) {
    static const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (steps[i] >= raw * 0.75) return steps[i];
    return 10;
}

int smenu_adjust(int i, const char *value, int dir, char *buf, size_t n) {
    const SmSetting *s = sm_setting(i);
    if (!s || !dir || !value) return -1;
    dir = dir > 0 ? 1 : -1;
    if (s->type == SM_BOOL) {
        if (sm_truthy(value) == (dir > 0)) return -1;
        snprintf(buf, n, "%s", dir > 0 ? "true" : "false");
        return 0;
    }
    const int range = strstr(s->domain, "..") != NULL;
    if (s->type == SM_ENUM || (s->type == SM_INT && !range)) {
        char c[64];
        int at = 0;
        for (int k = 0; choice(s->domain, k, c, sizeof c) == 0; k++)
            if (!strcmp(c, value) || (s->type == SM_INT && atoi(c) == atoi(value))) { at = k; break; }
        return at + dir < 0 ? -1 : choice(s->domain, at + dir, buf, n);
    }
    if (!range || (s->type != SM_INT && s->type != SM_FLOAT)) return -1;
    const double lo = strtod(s->domain, NULL), hi = strtod(strstr(s->domain, "..") + 2, NULL), v = strtod(value, NULL);
    if (s->type == SM_INT) {
        int step = (int)((hi - lo) / 20);
        if (step < 1) step = 1;
        int x = atoi(value) + dir * step;
        if (x < (int)lo) x = (int)lo;
        if (x > (int)hi) x = (int)hi;
        if (x == atoi(value)) return -1;
        snprintf(buf, n, "%d", x);
        return 0;
    }
    /* floats: below lo is "left out" (the launcher's -1: the runtime's default, "Auto") */
    const double step = nice_step((hi - lo) / 20);
    if (v < lo - 1e-9) {
        if (dir < 0) return -1;
        snprintf(buf, n, "%.3g", lo);
        return 0;
    }
    if (dir < 0 && v <= lo + 1e-9) {
        if (strtod(s->dflt, NULL) >= 0) return -1;   /* a row with its own default (saturation): lo is the end */
        snprintf(buf, n, "-1");
        return 0;
    }
    double x = v + dir * step;
    if (x > hi + 1e-9) {
        if (v >= hi - 1e-9) return -1;
        x = hi;
    }
    if (x < lo) x = lo;
    x = round(x * 1000) / 1000;
    snprintf(buf, n, "%.3g", x);
    return 0;
}

/* ---- presets ---- */

static void presets_parse(SettingsMenu *s, const char *text) {
    char line[512];
    int cur = -1;
    size_t at[SMENU_PRESETS] = {0}, cap[SMENU_PRESETS] = {0};
    for (const char *p = text ? text : ""; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = e ? e + 1 : p + strlen(p);
        char *l = line;
        while (*l == ' ' || *l == '\t' || *l == '\r') l++;
        for (char *q = l + strlen(l); q > l && (q[-1] == ' ' || q[-1] == '\r' || q[-1] == '\t');) *--q = 0;
        if (!*l || *l == '#' || *l == ';') continue;
        if (!strncmp(l, "[preset ", 8) && l[strlen(l) - 1] == ']') {
            cur = s->npresets < SMENU_PRESETS ? s->npresets++ : -1;
            if (cur >= 0) snprintf(s->preset_key[cur], sizeof s->preset_key[cur], "%.*s", (int)(strlen(l) - 9), l + 8);
            continue;
        }
        if (*l == '[') { cur = -1; continue; }
        if (cur < 0 || !strchr(l, '=')) continue;
        const size_t ln = strlen(l);
        if (at[cur] + ln + 2 > cap[cur]) {
            cap[cur] = (at[cur] + ln + 2) * 2;
            char *g = realloc(s->preset_text[cur], cap[cur]);
            if (!g) continue;
            s->preset_text[cur] = g;
        }
        memcpy(s->preset_text[cur] + at[cur], l, ln);
        at[cur] += ln;
        s->preset_text[cur][at[cur]++] = '\n';
        s->preset_text[cur][at[cur]] = 0;
    }
}

/* Calls fn for each "key = value" line of preset p; stops when fn returns nonzero (its value). */
static int preset_each(const SettingsMenu *s, int p, int (*fn)(void *ctx, const char *key, const char *value), void *ctx) {
    char line[512];
    for (const char *q = s->preset_text[p] ? s->preset_text[p] : ""; *q;) {
        const char *e = strchr(q, '\n');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, q, len);
        line[len] = 0;
        q = e ? e + 1 : q + len;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *k = line, *v = eq + 1;
        for (char *t = eq; t > k && (t[-1] == ' ' || t[-1] == '\t');) *--t = 0;
        *eq = 0;
        while (*v == ' ' || *v == '\t') v++;
        const int r = fn(ctx, k, v);
        if (r) return r;
    }
    return 0;
}

static int same_value(const char *key, const char *a, const char *b) {
    const SmSetting *st = sm_setting(sm_find(key));
    if (!a || !b) return 0;
    if (st && st->type == SM_FLOAT) return fabs(strtod(a, NULL) - strtod(b, NULL)) < 1e-4;
    if (st && st->type == SM_BOOL) return sm_truthy(a) == sm_truthy(b);
    return !strcmp(a, b);
}

static int differs(void *ctx, const char *key, const char *value) {
    const char *v = sm_get((const SmValues *)ctx, key);
    return v && !same_value(key, v, value);
}

int smenu_detect_preset(const SettingsMenu *s, const SmValues *vals) {
    for (int p = 0; p < s->npresets; p++)
        if (!preset_each(s, p, differs, (void *)vals)) return p;
    return -1;
}

/* ---- rows ---- */

static int adjustable(const SmSetting *st) { return st->type != SM_STR && st->type != SM_PATH; }

const char *smenu_badge(const SettingsMenu *s, int i, const char *value) {
    const SmSetting *st = sm_setting(i);
    if (!st) return "";
    if (s->ops.can_live && s->ops.can_live(st->key, value)) return tx(s, T_LIVE);
    const char cls = s->ops.start ? sm_change_class(s->ops.start(), i, value) : st->cls;
    if (s->ops.live_tiers && cls == SM_PASS) return tx(s, T_PASS);
    if (s->ops.live_tiers && cls == SM_REINIT) return tx(s, T_REINIT);
    return tx(s, T_RESTART_BADGE);
}

int smenu_queued_count(const SettingsMenu *s) {
    int n = 0;
    for (int i = 0; i < sm_count() && i < (int)sizeof s->queued; i++) n += s->queued[i] != 0;
    return n;
}

static const char *label_of(const SettingsMenu *s, const SmSetting *st) {
    const char *l = smenu_text(st->label, s->es);
    return l ? l : st->key;
}

static SmTab tab_of(int tab) { return (SmTab)(SM_TAB_DISPLAY + tab); }

static int mod_on(const SettingsMenu *s, int j) {
    const char *v = sm_get(s->ops.now(), "mods");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (!strcmp(t, s->mod[j])) return 1;
    return 0;
}

/* The rows of the current tab (labels into items). */
static int build_rows(SettingsMenu *s, char *items, size_t cap) {
    size_t at = 0;
    s->nrows = 0;
#define ADD(kind_, setting_, text_)                                                                    \
    do {                                                                                               \
        if (s->nrows < MODMENU_ITEMS_MAX) {                                                            \
            s->row[s->nrows].kind = kind_, s->row[s->nrows].setting = setting_, s->nrows++;            \
            int w_ = snprintf(items + at, cap - at, "%s%s", at ? "\n" : "", text_);                    \
            if (w_ > 0 && (size_t)w_ < cap - at) at += (size_t)w_;                                     \
        }                                                                                              \
    } while (0)
    if (s->tab == 0 && s->npresets) ADD(SMENU_ROW_PRESET, -1, smenu_text("preset", s->es));
    for (int i = 0; i < sm_count(); i++) {
        const SmSetting *st = sm_setting(i);
        if (st->tab == tab_of(s->tab) && (st->flags & SM_MENU) && adjustable(st)) ADD(SMENU_ROW_SETTING, i, label_of(s, st));
    }
    if (tab_of(s->tab) == SM_TAB_MODS)
        for (int j = 0; j < s->nmods; j++) ADD(SMENU_ROW_MOD, j, s->mod[j]);
    if (!s->nrows) ADD(SMENU_ROW_INFO, -1, tx(s, tab_of(s->tab) == SM_TAB_MODS ? T_NO_MODS : T_PENDING_NONE));
    const int q = smenu_queued_count(s);
    if (q) {
        char t[96];
        snprintf(t, sizeof t, tx(s, T_PENDING), q, q == 1 ? "" : "s");
        ADD(SMENU_ROW_PENDING, -1, t);
        if (s->ops.restart) ADD(SMENU_ROW_RESTART, -1, tx(s, T_RESTART_ROW));
    }
#undef ADD
    return s->nrows;
}

/* The first key named in a prerequisite expression. */
static void first_key(const char *expr, char *buf, size_t n) {
    size_t k = strcspn(expr, "=!|&");
    if (k >= n) k = n - 1;
    memcpy(buf, expr, k);
    buf[k] = 0;
}

static void note_for(SettingsMenu *s, int r) {
    char *note = s->m.note;
    const size_t cap = sizeof s->m.note;
    note[0] = 0;
    if (r < 0 || r >= s->nrows) return;
    const int kind = s->row[r].kind;
    if (kind == SMENU_ROW_SETTING) {
        const SmSetting *st = sm_setting(s->row[r].setting);
        size_t at = 0;
        if (s->m.grey[r]) {
            char key[64];
            first_key(st->requires, key, sizeof key);
            const SmSetting *dep = sm_setting(sm_find(key));
            at = (size_t)snprintf(note, cap, tx(s, T_NEEDS), dep ? label_of(s, dep) : key);
            if (at >= cap) at = cap - 1;
        }
        const char *d = smenu_text(st->desc, s->es);
        if (d) snprintf(note + at, cap - at, "%s", d);
    } else if (kind == SMENU_ROW_PRESET) {
        snprintf(note, cap, "%s", tx(s, T_PRESET_NOTE));
    } else if (kind == SMENU_ROW_MOD) {
        snprintf(note, cap, "%s", tx(s, T_MOD_NOTE));
    } else if (kind == SMENU_ROW_PENDING) {
        size_t at = (size_t)snprintf(note, cap, "%s", tx(s, s->ops.saved ? T_PENDING_SAVED : T_PENDING_SESSION));
        int first = 1;
        for (int i = 0; i < sm_count() && i < (int)sizeof s->queued && at < cap; i++) {
            if (!s->queued[i]) continue;
            const SmSetting *st = sm_setting(i);
            char v[64];
            if (!strcmp(st->key, "mods")) snprintf(v, sizeof v, "%s", sm_get(s->ops.now(), "mods"));
            else smenu_value_label(st->key, sm_get(s->ops.now(), st->key), s->es, v, sizeof v);
            int w = snprintf(note + at, cap - at, "%s%s: %s", first ? "" : ", ", label_of(s, st), v);
            if (w > 0) at += (size_t)w;
            first = 0;
        }
        if (at < cap && s->ops.saved && !s->ops.restart) snprintf(note + at, cap - at, ". %s", tx(s, T_PENDING_EXIT));
        if (!smenu_queued_count(s)) snprintf(note, cap, "%s", tx(s, T_PENDING_NONE));
    } else if (kind == SMENU_ROW_RESTART) {
        snprintf(note, cap, "%s", tx(s, s->ops.restart_ok ? T_RESTART_NOTE : T_RESTART_NOTE_EXIT));
    } else if (kind == SMENU_ROW_INFO && tab_of(s->tab) == SM_TAB_MODS) {
        snprintf(note, cap, "%s", tx(s, T_NO_MODS_NOTE));
    }
}

/* Values, badges, greyed flags and the note of the current rows. */
static void fill(SettingsMenu *s) {
    const SmValues *now = s->ops.now(), *start = s->ops.start();
    for (int r = 0; r < s->nrows && r < s->m.n; r++) {
        char *val = s->m.value[r], *badge = s->m.badge[r];
        val[0] = badge[0] = 0;
        s->m.grey[r] = 0;
        if (s->row[r].kind == SMENU_ROW_SETTING) {
            const int i = s->row[r].setting;
            const SmSetting *st = sm_setting(i);
            const char *v = sm_get(now, st->key), *v0 = sm_get(start, st->key);
            smenu_value_label(st->key, v, s->es, val, MODMENU_VALUE_MAX);
            char probe[256];
            const char *bv = v;   /* the badge of the change made, or of the next one */
            if (v0 && same_value(st->key, v, v0) &&
                (smenu_adjust(i, v, 1, probe, sizeof probe) == 0 || smenu_adjust(i, v, -1, probe, sizeof probe) == 0))
                bv = probe;
            snprintf(badge, MODMENU_VALUE_MAX, "%s", smenu_badge(s, i, bv));
            s->m.grey[r] = !sm_available(now, i);
        } else if (s->row[r].kind == SMENU_ROW_PRESET) {
            const int p = smenu_detect_preset(s, now);
            char id[48];
            snprintf(id, sizeof id, "preset_%s", p >= 0 ? s->preset_key[p] : "custom");
            const char *l = smenu_text(id, s->es);
            snprintf(val, MODMENU_VALUE_MAX, "%s", l ? l : p >= 0 ? s->preset_key[p] : "custom");
        } else if (s->row[r].kind == SMENU_ROW_MOD) {
            snprintf(val, MODMENU_VALUE_MAX, "%s", tx(s, mod_on(s, s->row[r].setting) ? T_ON : T_OFF));
            snprintf(badge, MODMENU_VALUE_MAX, "%s", tx(s, T_RESTART_BADGE));
        }
    }
    note_for(s, s->m.sel);
}

/* Rebuilds the rows of the tab (selection kept when keep, else the first row). */
static void rebuild(SettingsMenu *s, int keep) {
    static char items[MODMENU_ITEMS_MAX * MODMENU_TEXT_MAX];
    const int sel = keep ? s->m.sel : 0, top = s->m.top;
    if (build_rows(s, items, sizeof items) && modmenu_set_items(&s->m, items, sel) && keep) {
        s->m.top = top;
        if (s->m.sel < s->m.top) s->m.top = s->m.sel;
        if (s->m.sel >= s->m.top + modmenu_rows(&s->m)) s->m.top = s->m.sel - modmenu_rows(&s->m) + 1;
        if (s->m.top > s->m.n - modmenu_rows(&s->m)) s->m.top = s->m.n - modmenu_rows(&s->m);
        if (s->m.top < 0) s->m.top = 0;
    }
    fill(s);
}

/* ---- changes ---- */

static void change(SettingsMenu *s, const char *key, const char *value) {
    const int i = sm_find(key);
    if (i < 0) return;
    const int r = s->ops.set(key, value);
    if (r != 0 && r != -3) return;   /* -3: kept for the session, the pending file failed */
    const int live = s->ops.live ? s->ops.live(key, value) : 0;
    const char *v0 = sm_get(s->ops.start(), key);
    if (i < (int)sizeof s->queued) s->queued[i] = !(v0 && same_value(key, value, v0)) && !live;
}

static int apply_one(void *ctx, const char *key, const char *value) {
    SettingsMenu *s = ctx;
    const char *cur = sm_get(s->ops.now(), key);
    if (cur && !same_value(key, cur, value)) change(s, key, value);
    return 0;
}

static void adjust_row(SettingsMenu *s, int r, int dir) {
    if (r < 0 || r >= s->nrows) return;
    if (s->row[r].kind == SMENU_ROW_SETTING) {
        const SmSetting *st = sm_setting(s->row[r].setting);
        char v[256];
        if (smenu_adjust(s->row[r].setting, sm_get(s->ops.now(), st->key), dir, v, sizeof v) == 0) change(s, st->key, v);
    } else if (s->row[r].kind == SMENU_ROW_PRESET && s->npresets) {
        const int p = smenu_detect_preset(s, s->ops.now());
        int q = p < 0 ? (dir > 0 ? 0 : s->npresets - 1) : p + dir;
        if (q < 0 || q >= s->npresets) return;
        preset_each(s, q, apply_one, s);
    } else if (s->row[r].kind == SMENU_ROW_MOD) {
        const int j = s->row[r].setting, on = mod_on(s, j);
        if (on == (dir > 0)) return;
        char list[1024] = "";
        size_t at = 0;
        for (int k = 0; k < s->nmods; k++) {
            const int keep = k == j ? !on : mod_on(s, k);
            if (!keep) continue;
            int w = snprintf(list + at, sizeof list - at, "%s%s", at ? "," : "", s->mod[k]);
            if (w > 0 && (size_t)w < sizeof list - at) at += (size_t)w;
        }
        change(s, "mods", list);
    }
}

/* ---- the menu ---- */

void smenu_init(SettingsMenu *s, const SmenuOps *ops, const char *lang, const char *presets_text) {
    memset(s, 0, sizeof *s);
    s->ops = *ops;
    s->es = lang && !strcmp(lang, "es");
    presets_parse(s, presets_text);
    const char *mods = s->ops.start ? sm_get(s->ops.start(), "mods") : NULL;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", mods ? mods : "");
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t && s->nmods < SMENU_MODS; t = strtok_r(NULL, ",", &save))
        snprintf(s->mod[s->nmods++], sizeof s->mod[0], "%s", t);
}

void smenu_free(SettingsMenu *s) {
    for (int p = 0; p < SMENU_PRESETS; p++) {
        free(s->preset_text[p]);
        s->preset_text[p] = NULL;
    }
    s->npresets = 0;
}

void smenu_open(SettingsMenu *s, double now_ms) {
    static char items[MODMENU_ITEMS_MAX * MODMENU_TEXT_MAX];
    char tabs[256];
    snprintf(tabs, sizeof tabs, "%s\n%s\n%s\n%s\n%s\n%s", tx(s, T_TAB_DISPLAY), tx(s, T_TAB_EFFECTS), tx(s, T_TAB_AUDIO), tx(s, T_TAB_CONTROLS),
             tx(s, T_TAB_GAME), tx(s, T_TAB_MODS));
    s->tab = s->tab < 0 || s->tab >= SMENU_TABS ? 0 : s->tab;
    build_rows(s, items, sizeof items);
    modmenu_open(&s->m, tx(s, T_TITLE), items, tx(s, T_FOOTER), 0, now_ms);
    modmenu_values(&s->m, 1);
    modmenu_tabs(&s->m, tabs, s->tab);
    fill(s);
}

void smenu_close(SettingsMenu *s) { modmenu_close(&s->m); }
int smenu_is_open(const SettingsMenu *s) { return s->m.open; }

int smenu_step(SettingsMenu *s, unsigned controls, double now_ms) {
    int i = 0;
    const int ev = modmenu_step(&s->m, controls, now_ms, &i);
    switch (ev) {
    case MM_EV_TAB:
        s->tab = i;
        rebuild(s, 0);
        break;
    case MM_EV_ADJUST:
        adjust_row(s, i, s->m.adjust);
        rebuild(s, 1);
        break;
    case MM_EV_CHOOSE:
        if (i < s->nrows && s->row[i].kind == SMENU_ROW_RESTART) {
            modmenu_confirm(&s->m, tx(s, T_RESTART_Q), tx(s, T_YES), tx(s, T_NO));
        } else if (i < s->nrows && s->row[i].kind == SMENU_ROW_SETTING && sm_setting(s->row[i].setting)->type == SM_BOOL) {
            adjust_row(s, i, sm_truthy(sm_get(s->ops.now(), sm_setting(s->row[i].setting)->key)) ? -1 : 1);   /* A toggles */
            rebuild(s, 1);
        } else if (i < s->nrows && s->row[i].kind == SMENU_ROW_MOD) {
            adjust_row(s, i, mod_on(s, s->row[i].setting) ? -1 : 1);
            rebuild(s, 1);
        }
        break;
    case MM_EV_CONFIRM:
        if (i && s->ops.restart) s->ops.restart();
        break;
    default:
        break;
    }
    if (s->m.open && ev != MM_EV_TAB && ev != MM_EV_ADJUST) note_for(s, s->m.sel);   /* the selection may have moved */
    return ev;
}

unsigned smenu_controls(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly, int key_q, int key_e) {
    unsigned c = 0;
    if ((buttons & 0x1) || ly > 16384) c |= MM_UP;
    if ((buttons & 0x2) || ly < -16384) c |= MM_DOWN;
    if ((buttons & 0x4) || lx < -16384) c |= MM_LEFT;
    if ((buttons & 0x8) || lx > 16384) c |= MM_RIGHT;
    if ((buttons & 0x10) || analog[0] > 0x40) c |= MM_OK;
    if ((buttons & 0x20) || analog[1] > 0x40) c |= MM_BACK;
    if (analog[5] > 0x40 || key_q) c |= MM_TAB_PREV;
    if (analog[4] > 0x40 || key_e) c |= MM_TAB_NEXT;
    return c;
}
