/* The in-game settings menu: the launcher's settings in tabs on
 * top of the mod menu (src/modmenu.h, a value menu with tabs), game-independent and pure.
 *
 * Tabs Pantalla / Efectos / Audio / Controles / Juego / Mods (en Display / Effects / Audio /
 * Controls / Game / Mods); each tab lists the model's SM_MENU rows of that tab (settings_model.h)
 * that the menu can change (no free-text values), as "Label  < value >  badge":
 *   - Pantalla starts with Preajuste (Preset): the launcher's presets (its presets file, W7);
 *     choosing one sets every key of it; any other change shows Personalizado (Custom).
 *   - Mods: one on / off row per mod the game started with (the "mods" list).
 *   - the badge says how a change applies now: "al instante" (live, at the next frame), "breve
 *     pausa" / "pantalla negra ~1 s" (classes b / c once the renderer applies them live: live_tiers)
 *     or "al reiniciar" (everything else); greyed rows are rows whose prerequisite is off
 *     (sm_available), the note says which.
 *   - when changes wait for a restart, every tab ends with "Aplicar al reiniciar (N cambios)" (the
 *     note lists them) and "Reiniciar ahora" (a confirm dialog, then ops.restart).
 * The note under the list is the selected row's description (the launcher's i18n texts,
 * launcher/src/i18n_strings.h); the texts are in neutral Spanish (tú) or English.
 *
 * Every change goes through ops.set (record it: settings_sync_set) and ops.live (apply it live when
 * it can: settings_apply's sa_request); tests pass fakes (tools/settings_menu_test.c). */
#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H
#include "modmenu.h"
#include "settings_model.h"

typedef struct SmenuOps {
    const SmValues *(*start)(void);                       /* the values the game started with */
    const SmValues *(*now)(void);                         /* the current values (after ops.set) */
    int (*set)(const char *key, const char *value);       /* records a change: 0, or < 0 refused */
    int (*live)(const char *key, const char *value);      /* 1 when it applies live (queued for the next frame) */
    int (*can_live)(const char *key, const char *value);  /* 1 when that change would apply live (badges) */
    void (*restart)(void);                                /* "Reiniciar ahora" confirmed */
    int saved;        /* the launcher keeps the changes (else the note says they last this session) */
    int restart_ok;   /* the launcher relaunches the game after a restart */
    int live_tiers;   /* classes b / c apply live (W3): their own badges instead of "al reiniciar" */
} SmenuOps;

#define SMENU_TABS 6
#define SMENU_PRESETS 8
#define SMENU_MODS 32
enum { SMENU_ROW_SETTING, SMENU_ROW_PRESET, SMENU_ROW_MOD, SMENU_ROW_PENDING, SMENU_ROW_RESTART, SMENU_ROW_INFO };

typedef struct SettingsMenu {
    ModMenu m;
    SmenuOps ops;
    int es;                                  /* Spanish texts */
    int tab;
    int nrows;
    struct { int kind, setting; } row[MODMENU_ITEMS_MAX];   /* setting: the model index or the mod */
    int npresets;
    char preset_key[SMENU_PRESETS][24];
    char *preset_text[SMENU_PRESETS];        /* "key = value\n" lines (malloc'd) */
    int nmods;
    char mod[SMENU_MODS][64];
    unsigned char queued[256];               /* model index: changed and waiting for the restart */
} SettingsMenu;

/* Sets the menu up (closed): ops, the language ("es" or anything else for English) and the
 * launcher's presets file text (NULL / "": no Preajuste row). Call smenu_free before setting it up again. */
void smenu_init(SettingsMenu *s, const SmenuOps *ops, const char *lang, const char *presets_text);
void smenu_free(SettingsMenu *s);
void smenu_open(SettingsMenu *s, double now_ms);
void smenu_close(SettingsMenu *s);
int smenu_is_open(const SettingsMenu *s);
/* One input sample (MM_* bits, with MM_TAB_PREV / MM_TAB_NEXT). Returns the MM_EV_* it handled. */
int smenu_step(SettingsMenu *s, unsigned controls, double now_ms);

/* The menu controls of a pad sample (XINPUT wButtons, the 8 analog buttons, the left stick) and
 * the raw Q / E keys: d-pad / stick directions, A or Start = OK, B or Back = BACK, white (LB) or Q
 * = previous tab, black (RB) or E = next tab. Y is not OK here (E, the keyboard's Y, switches tabs). */
unsigned smenu_controls(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly, int key_q, int key_e);

/* Texts and values (exposed for the tests). */
const char *smenu_text(const char *i18n_id, int es);   /* launcher/src/i18n_strings.h; NULL unknown */
/* The value shown for key = value ("Sí", "-6 dB", "Calidad", ...). Returns buf. */
const char *smenu_value_label(const char *key, const char *value, int es, char *buf, size_t n);
/* The next value of setting i from value in direction dir (-1 / +1) in its domain; 0 and the value
 * in buf, or -1 at the end of the domain / not adjustable. */
int smenu_adjust(int i, const char *value, int dir, char *buf, size_t n);
/* The preset the values match (its index), -1 = custom. */
int smenu_detect_preset(const SettingsMenu *s, const SmValues *vals);
/* The badge of a change of setting i to value under ops ("" none). */
const char *smenu_badge(const SettingsMenu *s, int i, const char *value);
/* Changes waiting for the restart. */
int smenu_queued_count(const SettingsMenu *s);

#endif
