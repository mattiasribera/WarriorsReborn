/* Menus and music for mods (src/hle_modmenu.h). */
#include "hle_modmenu.h"
#include "disc.h"
#include "hle_devlua.h"
#include "kbm_glyphs.h"
#include "xtex.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static HleMenuAudio au;
static int32_t (*cue_of)(const char *name);
static char disc_dir[1024];
static double (*clock_ms)(void);

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static double now(void) { return clock_ms ? clock_ms() : mono_ms(); }

void hle_modmenu_setup(const HleMenuAudio *audio, int32_t (*cue_fn)(const char *name), const char *disc, double (*now_ms)(void)) {
    if (audio) au = *audio;
    cue_of = cue_fn;
    snprintf(disc_dir, sizeof disc_dir, "%s", disc ? disc : "");
    clock_ms = now_ms;
}

/* ---- art from the player's disc ---- */

/* The texture chunk `chunk` of the disc file with name hash `file`, decoded (malloc'd B8G8R8A8). */
static uint32_t *disc_texture(uint32_t file, uint32_t chunk, int *w, int *h, const uint8_t *buf, size_t n) {
    uint8_t *own = NULL;
    if (!buf) {
        if (!disc_dir[0] || !(own = disc_read_hash(disc_dir, file, 64u << 20, &n))) return NULL;
        buf = own;
    }
    static DiscChunk ch[4096];
    int nc = disc_walk_buffer(buf, n, 0, ch, (int)(sizeof ch / sizeof *ch));
    uint32_t *px = NULL;
    for (int i = 0; i < nc && i < (int)(sizeof ch / sizeof *ch) && !px; i++) {
        if (ch[i].cls != DISC_CLASS_TEXTURE || ch[i].hash != chunk || (uint64_t)ch[i].off + ch[i].size > n) continue;
        const uint8_t *p = buf + ch[i].off;
        uint32_t fmt, size, off;
        XTexInfo t;
        if (disc_tex_parse(p, ch[i].size, &fmt, &size, &off) || xtex_info(fmt, size, &t)) continue;
        if (t.w > 4096 || t.h > 4096 || (uint64_t)off + xtex_level_bytes(&t, 0) > ch[i].size) continue;
        px = malloc((size_t)t.w * t.h * 4);
        if (px && xtex_decode_level(&t, 0, p + off, px)) { free(px); px = NULL; }
        if (px) { *w = (int)t.w; *h = (int)t.h; }
    }
    free(own);
    return px;
}

/* The game's main font: the atlas texture of paks\global.pak and its glyph rects (record
 * code + KBM_RECT_BASE, src/kbm_glyphs.h). Loaded once. */
#define FONT_CHUNK 0x5cd2a2bcu
static KbmRect font_rects[KBM_RECTS_MAX];
static uint32_t *font_px;
static int font_w, font_h, font_tried, font_n;

static void font_load(void) {
    if (font_tried) return;
    font_tried = 1;
    size_t n = 0;
    uint8_t *pak = disc_dir[0] ? disc_read_hash(disc_dir, KBM_PAK_NAME_HASH, 16u << 20, &n) : NULL;
    if (!pak) {
        printf("[menu] no paks\\global.pak in the disc folder: menus are drawn without text\n");
        return;
    }
    int r = kbm_rects_parse(pak, n, font_rects, KBM_RECTS_MAX);
    font_px = r > KBM_RECT_BASE + 32 ? disc_texture(KBM_PAK_NAME_HASH, FONT_CHUNK, &font_w, &font_h, pak, n) : NULL;
    font_n = font_px ? (r > KBM_RECTS_MAX ? KBM_RECTS_MAX : r) - KBM_RECT_BASE - 32 : 0;
    if (font_n > 224) font_n = 224;
    free(pak);
    printf("[menu] font: %s\n", font_px ? "the game's main font from paks\\global.pak" : "not found (menus without text)");
}

int hle_modmenu_cover_parse(const char *spec, uint32_t *file, uint32_t *chunk, int rect[4]) {
    unsigned f, c;
    int x = 0, y = 0, w = 0, h = 0, used = 0;
    if (!spec || strncmp(spec, "disc:", 5)) return -1;
    if (sscanf(spec + 5, "%8x:%8x%n", &f, &c, &used) != 2) return -1;
    const char *rest = spec + 5 + used;
    if (*rest) {
        int u2 = 0;
        if (sscanf(rest, ":%d,%d,%d,%d%n", &x, &y, &w, &h, &u2) != 4 || rest[u2] || x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    }
    *file = f;
    *chunk = c;
    rect[0] = x, rect[1] = y, rect[2] = w, rect[3] = h;
    return 0;
}

#define COVERS 8
static struct {
    char spec[96];
    uint32_t *px;
    int w, h;
    float uv[4];
} covers[COVERS];
static int ncovers;

/* The cover of a spec, decoded on first use (kept for the run); -1 when none. */
static int cover_get(const char *spec) {
    for (int i = 0; i < ncovers; i++)
        if (!strcmp(covers[i].spec, spec)) return covers[i].px ? i : -1;
    if (ncovers == COVERS) return -1;
    int i = ncovers++, r[4];
    uint32_t file, chunk;
    snprintf(covers[i].spec, sizeof covers[i].spec, "%s", spec);
    if (hle_modmenu_cover_parse(spec, &file, &chunk, r)) {
        printf("[menu] cover \"%.90s\": not a disc:<file>:<chunk>[:x,y,w,h] spec\n", spec);
        return -1;
    }
    covers[i].px = disc_texture(file, chunk, &covers[i].w, &covers[i].h, NULL, 0);
    if (!covers[i].px) {
        printf("[menu] cover %08x:%08x not found on the disc\n", file, chunk);
        return -1;
    }
    float W = (float)covers[i].w, H = (float)covers[i].h;
    if (!r[2]) r[2] = covers[i].w, r[3] = covers[i].h;
    covers[i].uv[0] = r[0] / W, covers[i].uv[1] = r[1] / H;
    covers[i].uv[2] = fminf((r[0] + r[2]) / W, 1), covers[i].uv[3] = fminf((r[1] + r[3]) / H, 1);
    printf("[menu] cover %08x:%08x: %dx%d texture from the disc\n", file, chunk, covers[i].w, covers[i].h);
    return i;
}

/* ---- the menu ---- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ModMenu menu;
static int32_t item_cue[MODMENU_ITEMS_MAX];
static char item_sound[MODMENU_ITEMS_MAX][MODMENU_TEXT_MAX];
static int cur_cover = -1;
static uint32_t cover_serial;
static int swallow;                 /* keep the pad neutral until released after the menu closed */
static volatile int escape_pending;
static const char *result_kind;     /* "chosen" / "closed" until ModMenuState returned it */
static int result_index;
static int result_dir = -2;          /* a value menu's choice: 0 A, -1 / 1 left / right; -2 a plain menu */
static char item_note[MODMENU_ITEMS_MAX][MODMENU_NOTE_MAX];   /* a value menu's notes (ModMenuValues) */
/* ModMenuKey: when the player last asked for a mod's menu (-1 none) and the Back + LB hold. */
#define KEY_HOLD_MS 500.0
#define KEY_FRESH_MS 1000.0
static volatile double key_ms = -1;
static double key_hold_t0;
static int key_hold_done;

/* ---- the music channel ---- */
#define VOICES 6
typedef struct {
    int used, id, release;
    int32_t cue;
    char name[MODMENU_TEXT_MAX];
    ModFade f;
    float applied;
} Voice;
static Voice voices[VOICES];
static int base = -1, preview = -1;
static ModFade game;
static float game_applied = 1, game_target = 1;

static int voice_start(int32_t cue, const char *name, int loop, double t) {
    if (!au.start || cue < 0) return -1;
    int v = -1;
    for (int i = 0; i < VOICES && v < 0; i++)
        if (!voices[i].used) v = i;
    if (v < 0) return -1;
    int id = au.start((uint32_t)cue, loop, 1, 0);
    if (!id) return -1;
    Voice *s = &voices[v];
    *s = (Voice){1, id, 0, cue, "", {0}, 0};
    snprintf(s->name, sizeof s->name, "%s", name);
    modfade_init(&s->f, 0);
    modfade_set(&s->f, 1, MENU_FADE_MS, t);
    return v;
}

static void voice_fade(int v, float to, int release, double t) {
    if (v < 0 || !voices[v].used) return;
    modfade_set(&voices[v].f, to, MENU_FADE_MS, t);
    voices[v].release = release;
}

static void voices_update(double t) {
    int active = 0;
    for (int i = 0; i < VOICES; i++) {
        Voice *s = &voices[i];
        if (!s->used) continue;
        float g = modfade_gain(modfade_level(&s->f, t));
        if (g != s->applied && au.level) au.level(s->id, g);
        s->applied = g;
        /* faded out, or ended (a one-shot song, or the game stopped its source: the radio as the
         * player leaves the hideout): the game's own music comes back */
        if ((s->release && modfade_done(&s->f, t)) || (au.alive && !au.alive(s->id))) {
            if (au.stop) au.stop(s->id);
            s->used = 0;
            if (base == i) base = -1;
            if (preview == i) preview = -1;
            continue;
        }
        if (!s->release) active = 1;
    }
    const float target = active ? 0.0f : 1.0f;
    if (target != game_target) {
        game_target = target;
        modfade_set(&game, target, MENU_FADE_MS, t);
    }
    const float g = modfade_gain(modfade_level(&game, t));
    if (g != game_applied && au.game_level) au.game_level(g);
    game_applied = g;
}

/* The selection now previews cue (-1: none). */
static void preview_to(int32_t cue, const char *name, double t) {
    if (cue < 0 || (base >= 0 && voices[base].cue == cue)) {
        voice_fade(preview, 0, 1, t);
        preview = -1;
        voice_fade(base, 1, 0, t);
    } else if (preview < 0 || voices[preview].cue != cue) {
        voice_fade(preview, 0, 1, t);
        preview = voice_start(cue, name, 1, t);
        voice_fade(base, 0, 0, t);
        if (preview >= 0) printf("[menu] preview %s\n", name);
    }
    voices_update(t);
}

static void menu_event(int ev, int i, double t) {
    if (ev == MM_EV_HOVER) {
        preview_to(item_cue[i], item_sound[i], t);
    } else if (menu.values && (ev == MM_EV_CHOOSE || ev == MM_EV_ADJUST)) {   /* a value menu stays open */
        result_kind = "chosen";
        result_index = i;
        result_dir = ev == MM_EV_ADJUST ? (menu.adjust > 0 ? 1 : -1) : 0;
        printf("[menu] chosen item %d (%s)\n", i + 1, ev == MM_EV_ADJUST ? (menu.adjust > 0 ? "right" : "left") : "A");
    } else if (ev == MM_EV_CHOOSE || ev == MM_EV_CANCEL) {
        result_kind = ev == MM_EV_CHOOSE ? "chosen" : "closed";
        result_index = i;
        result_dir = -2;
        if (ev == MM_EV_CHOOSE && item_cue[i] >= 0) {
            if (preview < 0 || voices[preview].cue != item_cue[i]) preview_to(item_cue[i], item_sound[i], t);
            if (preview >= 0) {   /* the previewed song stays: it becomes the mod's song */
                voice_fade(base, 0, 1, t);
                base = preview;
                preview = -1;
                voice_fade(base, 1, 0, t);
            }
        } else {
            preview_to(-1, "", t);
        }
        printf("[menu] %s item %d%s%s\n", result_kind, i + 1, base >= 0 ? ", playing " : "", base >= 0 ? voices[base].name : "");
        voices_update(t);
    }
    fflush(stdout);
}

void hle_modmenu_request(const char *how) {
    if (menu.open) return;
    key_ms = now();
    printf("[menu] %s: a mod's menu requested\n", how ? how : "key");
    fflush(stdout);
}

int hle_modmenu_input(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly) {
    if (!menu.open && (buttons & 0x20) && analog[5] > 0x40) {   /* Back + LB (white) held: ModMenuKey */
        const double t = now();
        if (!key_hold_t0) key_hold_t0 = t;
        else if (!key_hold_done && t - key_hold_t0 >= KEY_HOLD_MS) {
            key_hold_done = 1;
            hle_modmenu_request("Back + LB");
        }
    } else {
        key_hold_t0 = 0;
        key_hold_done = 0;
    }
    if (!menu.open && !swallow) return 0;
    const unsigned c = modmenu_controls(buttons, analog, lx, ly);
    if (!menu.open && !c) {   /* released after the menu closed: the game's pad again */
        swallow = 0;
        return 0;
    }
    const double t = now();
    if (menu.open) {
        int i = 0, ev;
        pthread_mutex_lock(&lock);
        if (escape_pending) {
            menu.open = 0;
            i = menu.sel;
            ev = MM_EV_CANCEL;
        } else {
            ev = modmenu_step(&menu, c, t, &i);
        }
        if (menu.values) snprintf(menu.note, sizeof menu.note, "%s", item_note[menu.sel]);
        escape_pending = 0;
        pthread_mutex_unlock(&lock);
        if (ev != MM_EV_NONE) menu_event(ev, i, t);
    }
    if (!menu.open) swallow = c != 0;
    return 1;
}

int hle_modmenu_escape(void) {
    if (!menu.open) return 0;
    escape_pending = 1;
    return 1;
}

void hle_modmenu_tick(void) {
    for (int i = 0; i < VOICES; i++)
        if (voices[i].used) { voices_update(now()); return; }
    if (game_applied != 1) voices_update(now());
}

static void stop_all(void) {
    for (int i = 0; i < VOICES; i++)
        if (voices[i].used && au.stop) au.stop(voices[i].id);
    memset(voices, 0, sizeof voices);
    base = preview = -1;
    modfade_init(&game, 1);
    game_target = 1;
    if (game_applied != 1 && au.game_level) au.game_level(1);
    game_applied = 1;
}

void hle_modmenu_new_state(void) {
    pthread_mutex_lock(&lock);
    menu.open = 0;
    pthread_mutex_unlock(&lock);
    swallow = 0;
    escape_pending = 0;
    result_kind = NULL;
    key_ms = -1;
    key_hold_t0 = 0;
    key_hold_done = 0;
    stop_all();
    if (au.mute_set) au.mute_set(NULL, 0);   /* the old level's set and radio */
}

int hle_modmenu_view(ModMenu *out, HleMenuArt *art) {
    pthread_mutex_lock(&lock);
    const int open = menu.open;
    if (open) {
        *out = menu;
        memset(art, 0, sizeof *art);
        if (font_px) {
            art->font = (ModFont){(const float (*)[4])(const void *)&font_rects[KBM_RECT_BASE + 32], 32, font_n, font_w, font_h};
            art->font_px = font_px;
        }
        if (cur_cover >= 0) {
            art->cover_px = covers[cur_cover].px;
            art->cover_w = covers[cur_cover].w;
            art->cover_h = covers[cur_cover].h;
            memcpy(art->cover_uv, covers[cur_cover].uv, sizeof art->cover_uv);
            art->cover_serial = cover_serial;
        }
    }
    pthread_mutex_unlock(&lock);
    return open;
}

void hle_modmenu_font(HleMenuArt *art) {
    pthread_mutex_lock(&lock);
    font_load();
    if (font_px) {
        art->font = (ModFont){(const float (*)[4])(const void *)&font_rects[KBM_RECT_BASE + 32], 32, font_n, font_w, font_h};
        art->font_px = font_px;
    }
    pthread_mutex_unlock(&lock);
}

void hle_modmenu_set_disc(const char *disc) {
    if (!disc_dir[0]) snprintf(disc_dir, sizeof disc_dir, "%s", disc ? disc : "");
}

void hle_modmenu_reset(void) {
    hle_modmenu_new_state();
    memset(&menu, 0, sizeof menu);
    for (int i = 0; i < ncovers; i++) free(covers[i].px);
    memset(covers, 0, sizeof covers);
    ncovers = 0;
    cur_cover = -1;
    free(font_px);
    font_px = NULL;
    font_tried = font_n = 0;
    game_applied = 1;
}

/* ---- Lua ---- */
static void (*push_number)(uint32_t L, double n);
static void (*push_nil)(uint32_t L);
static void (*push_string)(uint32_t L, const char *s);

void hle_modmenu_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L),
                          void (*pushstring)(uint32_t L, const char *s)) {
    push_number = pushnumber;
    push_nil = pushnil;
    push_string = pushstring;
}

static const char *arg_str(uint32_t L, int i) {
    const int t = devlua_title_api.type(L, i);
    return t == 2 || t == 3 ? devlua_title_api.tostring(L, i) : NULL;
}
static int arg_num(uint32_t L, int i, double *out) {
    if (devlua_title_api.type(L, i) != 2) return 0;
    *out = strtod(devlua_title_api.tostring(L, i), NULL);
    return isfinite(*out);
}
static int ret_nil(uint32_t L) {
    if (push_nil) push_nil(L);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_open(uint32_t L) {
    const char *title = arg_str(L, 1), *items = arg_str(L, 2), *footer = arg_str(L, 3), *cover = arg_str(L, 5);
    double sel = 1;
    arg_num(L, 4, &sel);
    if (!items) return ret_nil(L);
    font_load();
    /* labels for the menu; the sound after a tab per line */
    static char labels[MODMENU_ITEMS_MAX * MODMENU_TEXT_MAX];
    size_t at = 0;
    int n = 0;
    for (const char *p = items; *p && n < MODMENU_ITEMS_MAX; n++) {
        const char *e = strchr(p, '\n'), *end = e ? e : p + strlen(p), *tab = memchr(p, '\t', (size_t)(end - p));
        const char *lend = tab ? tab : end;
        size_t ln = (size_t)(lend - p);
        if (ln > MODMENU_TEXT_MAX - 1) ln = MODMENU_TEXT_MAX - 1;
        memcpy(labels + at, p, ln);
        at += ln;
        labels[at++] = '\n';
        item_sound[n][0] = 0;
        item_cue[n] = -1;
        if (tab) {
            size_t sn = (size_t)(end - tab - 1);
            if (sn > MODMENU_TEXT_MAX - 1) sn = MODMENU_TEXT_MAX - 1;
            memcpy(item_sound[n], tab + 1, sn);
            item_sound[n][sn] = 0;
            item_cue[n] = cue_of ? cue_of(item_sound[n]) : -1;
            if (item_cue[n] >= 0 && au.valid && !au.valid((uint32_t)item_cue[n])) item_cue[n] = -1;
            if (item_cue[n] < 0) printf("[menu] no game sound \"%.80s\" (item %d plays nothing)\n", item_sound[n], n + 1);
        }
        if (!e) break;
        p = e + 1;
    }
    labels[at ? at - 1 : 0] = 0;
    const int cv = cover ? cover_get(cover) : -1;
    pthread_mutex_lock(&lock);
    int got = modmenu_open(&menu, title, labels, footer, (int)sel - 1, now());
    if (cv != cur_cover) cover_serial++;
    cur_cover = cv;
    pthread_mutex_unlock(&lock);
    result_kind = NULL;
    escape_pending = 0;
    printf("[menu] open \"%.60s\": %d items%s\n", title ? title : "", got, cv >= 0 ? ", cover" : "");
    fflush(stdout);
    if (!got) return ret_nil(L);
    if (push_number) push_number(L, got);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_state(uint32_t L) {
    if (menu.open && !result_kind) {   /* a value menu's choice comes first, while it stays open */
        if (push_string) push_string(L, "open");
        return 1;
    }
    if (!result_kind) return ret_nil(L);
    const char *k = result_kind;
    result_kind = NULL;
    if (push_string) push_string(L, k);
    if (push_number) push_number(L, result_index + 1);
    if (result_dir == -2) return 2;
    if (push_number) push_number(L, result_dir);
    result_dir = -2;
    return 3;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_close(uint32_t L) {
    (void)L;
    if (menu.open) {
        pthread_mutex_lock(&lock);
        menu.open = 0;
        const int i = menu.sel;
        pthread_mutex_unlock(&lock);
        menu_event(MM_EV_CANCEL, i, now());
    }
    return 0;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_music_play(uint32_t L) {
    const char *name = arg_str(L, 1);
    double loop = 1;
    arg_num(L, 2, &loop);
    const double t = now();
    if (!name) {   /* back to the game's own music */
        voice_fade(base, 0, 1, t);
        base = -1;
        voices_update(t);
        if (push_number) push_number(L, 1);
        return 1;
    }
    const int32_t cue = cue_of ? cue_of(name) : -1;
    if (cue < 0 || (au.valid && !au.valid((uint32_t)cue))) {
        printf("[menu] ModMusicPlay: no game sound \"%.80s\"\n", name);
        fflush(stdout);
        return ret_nil(L);
    }
    const int v = voice_start(cue, name, loop != 0, t);
    if (v < 0) return ret_nil(L);
    voice_fade(base, 0, 1, t);
    base = v;
    voices_update(t);
    printf("[menu] music %s%s\n", name, loop != 0 ? " (looping)" : "");
    fflush(stdout);
    if (push_number) push_number(L, 1);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_music_playing(uint32_t L) {
    if (base < 0 || !voices[base].used || !au.alive || !au.alive(voices[base].id)) return ret_nil(L);
    if (push_string) push_string(L, voices[base].name);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_music_mute(uint32_t L) {
    const char *names = arg_str(L, 1);
    static uint32_t set[512];
    int n = 0, unknown = 0;
    for (const char *p = names ? names : ""; *p && n < 512;) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        char nm[MODMENU_TEXT_MAX];
        if (ln > sizeof nm - 1) ln = sizeof nm - 1;
        memcpy(nm, p, ln);
        nm[ln] = 0;
        const int32_t c = nm[0] && cue_of ? cue_of(nm) : -1;
        if (c >= 0) set[n++] = (uint32_t)c;
        else if (nm[0]) unknown++;
        if (!e) break;
        p = e + 1;
    }
    if (au.mute_set) au.mute_set(set, n);
    printf("[menu] %d game sounds muted while a mod's music plays%s\n", n, unknown ? " (some names unknown)" : "");
    fflush(stdout);
    if (push_number) push_number(L, n);
    return 1;
}

/* Splits text into one line per row of dst (width bytes each, rows rows); missing lines empty. */
static void split_rows(char *dst, size_t width, int rows, const char *text) {
    const char *p = text ? text : "";
    for (int i = 0; i < rows; i++) {
        char *d = dst + (size_t)i * width;
        const char *e = *p ? strchr(p, '\n') : NULL;
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        if (ln > width - 1) ln = width - 1;
        while (ln && ((unsigned char)p[ln] & 0xc0) == 0x80) ln--;   /* cut at a character boundary */
        memcpy(d, p, ln);
        d[ln] = 0;
        p = e ? e + 1 : p + strlen(p);
    }
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_values(uint32_t L) {
    const char *values = arg_str(L, 1), *notes = arg_str(L, 2);
    if (!menu.open) return ret_nil(L);
    pthread_mutex_lock(&lock);
    const int first = !menu.values;
    modmenu_values(&menu, 1);
    split_rows(&menu.value[0][0], MODMENU_VALUE_MAX, MODMENU_ITEMS_MAX, values);
    if (notes || first) split_rows(&item_note[0][0], MODMENU_NOTE_MAX, MODMENU_ITEMS_MAX, notes);
    snprintf(menu.note, sizeof menu.note, "%s", item_note[menu.sel]);
    const int n = menu.n;
    pthread_mutex_unlock(&lock);
    if (push_number) push_number(L, n);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) int hle_modmenu_lua_key(uint32_t L) {
    const double k = key_ms;
    if (k < 0 || menu.open || now() - k > KEY_FRESH_MS) {
        if (k >= 0 && !menu.open) key_ms = -1;
        return ret_nil(L);
    }
    key_ms = -1;
    if (push_number) push_number(L, 1);
    return 1;
}
