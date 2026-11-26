/* Tests of src/hle_modmenu.c: the Lua functions of the mod menu and music channel on a fake Lua
 * stack, the pad input and the music policy against fake music voices (no game, no disc). */
#include "../src/hle_devlua.h"
#include "../src/hle_modmenu.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, what)                                                     \
    do {                                                                   \
        if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } \
        else printf("ok   %s\n", what);                                    \
    } while (0)

/* ---- a fake Lua stack: the call's arguments, then what the function pushed ---- */
static struct { int type; char s[512]; } arg[8];
static int nargs;
static struct { int type; char s[64]; double n; } ret[8];
static int nret;
static int f_gettop(uint32_t L) { (void)L; return nargs; }
static int f_type(uint32_t L, int i) { (void)L; return i >= 1 && i <= nargs ? arg[i - 1].type : 1; }
static const char *f_tostring(uint32_t L, int i) { (void)L; return i >= 1 && i <= nargs ? arg[i - 1].s : NULL; }
const DevLuaApi devlua_title_api = {f_gettop, NULL, NULL, f_type, f_tostring, NULL, NULL, NULL};
static void p_num(uint32_t L, double n) { (void)L; ret[nret].type = 2; ret[nret++].n = n; }
static void p_nil(uint32_t L) { (void)L; ret[nret++].type = 1; }
static void p_str(uint32_t L, const char *s) { (void)L; ret[nret].type = 3; snprintf(ret[nret++].s, sizeof ret[0].s, "%s", s); }

/* Calls fn with arguments: "s:text", "n:number" or "nil". */
static int lua(int (*fn)(uint32_t), int n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    nargs = n;
    for (int i = 0; i < n; i++) {
        const char *a = __builtin_va_arg(ap, const char *);
        arg[i].type = !strncmp(a, "s:", 2) ? 3 : !strncmp(a, "n:", 2) ? 2 : 1;
        snprintf(arg[i].s, sizeof arg[i].s, "%s", arg[i].type == 1 ? "" : a + 2);
    }
    __builtin_va_end(ap);
    nret = 0;
    return fn(0x1234);
}

/* ---- fake music voices ---- */
static int starts, stops, last_cue, last_loop, last_radio, alive[16], muted_n;
static float level[16], game = 1;
static int a_valid(uint32_t c) { return c < 100; }
static int a_start(uint32_t c, int loop, int radio, float l) {
    starts++;
    last_cue = (int)c, last_loop = loop, last_radio = radio;
    for (int i = 1; i < 16; i++)
        if (!alive[i]) { alive[i] = 1; level[i] = l; return i; }
    return 0;
}
static void a_level(int id, float l) { level[id] = l; }
static int a_alive(int id) { return id > 0 && id < 16 && alive[id]; }
static void a_stop(int id) { stops++; alive[id] = 0; }
static void a_mute(const uint32_t *c, int n) { (void)c; muted_n = n; }
static void a_game(float l) { game = l; }
static const HleMenuAudio fake = {a_valid, a_start, a_level, a_alive, a_stop, a_mute, a_game};
static int32_t cue_of(const char *name) {
    return !strcmp(name, "vags/music/a") ? 10 : !strcmp(name, "vags/music/b") ? 11 : -1;
}
static double t;
static double clock_fn(void) { return t; }
static const uint8_t none[8], a_btn[8] = {0xff}, b_btn[8] = {0, 0xff};

/* Advances the clock by ms in 10 ms frames, each one a neutral pad sample and a music tick. */
static void run(double ms) {
    for (double e = 0; e < ms; e += 10) {
        t += 10;
        hle_modmenu_input(0, none, 0, 0);
        hle_modmenu_tick();
    }
}
static int live(void) {
    int n = 0;
    for (int i = 1; i < 16; i++) n += alive[i];
    return n;
}

int main(void) {
    hle_modmenu_setup(&fake, cue_of, "", clock_fn);
    hle_modmenu_lua_init(p_num, p_nil, p_str);
    t = 1000;

    CHECK(hle_modmenu_input(0, a_btn, 0, 0) == 0, "input: no menu, the game gets the pad");
    lua(hle_modmenu_lua_open, 4, "s:Rocola", "s:Song A\tvags/music/a\nSong B\tvags/music/b\nShuffle", "s:A elegir", "nil");
    CHECK(nret == 1 && ret[0].type == 2 && ret[0].n == 3, "ModMenuOpen: three items");
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 1 && !strcmp(ret[0].s, "open"), "ModMenuState: open");
    CHECK(hle_modmenu_input(0, a_btn, 0, 0) == 1, "input: the game gets a neutral pad while the menu is open");
    ModMenu view;
    HleMenuArt art;
    CHECK(hle_modmenu_view(&view, &art) && view.n == 3 && !strcmp(view.item[1], "Song B") && !art.cover_px && !art.font_px,
          "view: labels without the sound names, no art without a disc");
    run(200);
    CHECK(starts == 1 && last_cue == 10 && last_loop && last_radio, "hover: item 1 previews its song (looping, on the radio)");
    run(400);
    CHECK(level[1] == 1 && game == 0, "hover: the preview faded in, the game's music out");
    hle_modmenu_input(0x2, none, 0, 0);   /* d-pad down */
    run(50);
    CHECK(starts == 1, "debounce: no new preview while the selection has not rested");
    run(200);
    CHECK(starts == 2 && last_cue == 11, "hover: item 2 previews its song after resting");
    run(400);
    CHECK(!alive[1] && level[2] == 1 && stops == 1, "crossfade: the old preview faded out and stopped");
    hle_modmenu_input(0, a_btn, 0, 0);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 2 && !strcmp(ret[0].s, "chosen") && ret[1].n == 2, "A: ModMenuState -> chosen, 2");
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuState: the result is returned once");
    CHECK(starts == 2 && alive[2], "choose: the previewed song keeps playing (no restart)");
    lua(hle_modmenu_lua_music_playing, 0);
    CHECK(nret == 1 && !strcmp(ret[0].s, "vags/music/b"), "ModMusicPlaying: the chosen song");
    CHECK(hle_modmenu_input(0, a_btn, 0, 0) == 1 && hle_modmenu_input(0, none, 0, 0) == 0,
          "input: neutral until the buttons are released after closing, then the game's again");

    lua(hle_modmenu_lua_open, 4, "s:Rocola", "s:Song A\tvags/music/a\nSong B\tvags/music/b\nShuffle", "s:", "n:2");
    run(200);
    CHECK(starts == 2 && level[2] == 1, "reopen on the playing song: hovering it starts nothing");
    hle_modmenu_input(0x2, none, 0, 0);
    run(600);
    CHECK(starts == 2 && level[2] == 1, "hover an item without a song: the mod's song stays");
    hle_modmenu_input(0x1, none, 0, 0);   /* up: back to Song B, the mod's song */
    run(300);
    CHECK(starts == 2 && level[2] == 1, "hover the mod's own song: nothing restarts");
    hle_modmenu_input(0x1, none, 0, 0);   /* up again: Song A */
    run(600);
    CHECK(starts == 3 && last_cue == 10 && level[2] == 0, "hover another song: it previews, the mod's song fades down");
    hle_modmenu_input(0, b_btn, 0, 0);
    run(600);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 2 && !strcmp(ret[0].s, "closed") && ret[1].n == 1, "B: ModMenuState -> closed, 1");
    CHECK(alive[2] && level[2] == 1 && live() == 1 && game == 0, "B: the preview stops, the mod's song comes back");

    lua(hle_modmenu_lua_music_play, 1, "nil");
    run(400);
    CHECK(live() == 0 && game == 1, "ModMusicPlay(nil): the mod's song fades out, the game's music back");
    lua(hle_modmenu_lua_music_playing, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMusicPlaying: nil without a song");
    lua(hle_modmenu_lua_music_play, 2, "s:vags/music/zzz", "n:0");
    CHECK(nret == 1 && ret[0].type == 1, "ModMusicPlay: an unknown sound -> nil");
    lua(hle_modmenu_lua_music_play, 2, "s:vags/music/a", "n:0");
    CHECK(nret == 1 && ret[0].n == 1 && last_cue == 10 && !last_loop, "ModMusicPlay(name, 0): plays once");
    run(400);
    CHECK(game == 0, "a song plays: the game's music stays down");
    for (int i = 1; i < 16; i++) alive[i] = 0;   /* the game stopped the radio's source */
    run(400);
    lua(hle_modmenu_lua_music_playing, 0);
    CHECK(game == 1 && nret == 1 && ret[0].type == 1, "a song stopped by the game: the game's music comes back, ModMusicPlaying nil");
    lua(hle_modmenu_lua_music_mute, 1, "s:vags/music/a\nvags/music/b\nnope");
    CHECK(nret == 1 && ret[0].n == 2 && muted_n == 2, "ModMusicMute: the known names");

    lua(hle_modmenu_lua_open, 3, "s:t", "s:x\ny", "s:");
    CHECK(hle_modmenu_escape() == 1, "Escape: closes an open menu");
    hle_modmenu_input(0, none, 0, 0);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 2 && !strcmp(ret[0].s, "closed") && hle_modmenu_escape() == 0, "Escape: closed; with no menu Escape is the window's");
    lua(hle_modmenu_lua_open, 3, "s:t", "s:x\ny", "s:");
    lua(hle_modmenu_lua_close, 0);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 2 && !strcmp(ret[0].s, "closed"), "ModMenuClose: closed");
    lua(hle_modmenu_lua_open, 1, "s:t");
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuOpen without items -> nil");
    lua(hle_modmenu_lua_open, 3, "s:t", "s:x\ny", "s:");
    hle_modmenu_new_state();
    CHECK(live() == 0 && !hle_modmenu_view(&view, &art) && game == 1 && muted_n == 0,
          "new Lua state: menu closed, the mod's music stopped, the mute set cleared");

    /* ModMenuValues (mods version 1.11): a value menu that stays open on A and left / right. */
    lua(hle_modmenu_lua_values, 1, "s:On");
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuValues without an open menu -> nil");
    lua(hle_modmenu_lua_open, 4, "s:Cheats", "s:Health\nRage\nKnife", "s:", "n:1");
    lua(hle_modmenu_lua_values, 2, "s:Off\nOn", "s:Health note\nRage note\nKnife note");
    CHECK(nret == 1 && ret[0].n == 3, "ModMenuValues: the open menu's three rows");
    CHECK(hle_modmenu_view(&view, &art) && view.values && !strcmp(view.value[0], "Off") && !strcmp(view.value[1], "On") &&
              !view.value[2][0] && !strcmp(view.note, "Health note"),
          "view: a value menu, values per row (missing = none), the selected row's note");
    run(50);
    hle_modmenu_input(0, a_btn, 0, 0);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 3 && !strcmp(ret[0].s, "chosen") && ret[1].n == 1 && ret[2].n == 0 && hle_modmenu_view(&view, &art),
          "value menu A: chosen, 1, 0 and the menu stays open");
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 1 && !strcmp(ret[0].s, "open"), "value menu: the choice is returned once, then open");
    hle_modmenu_input(0, none, 0, 0);
    hle_modmenu_input(0x2, none, 0, 0);   /* down */
    hle_modmenu_input(0, none, 0, 0);
    CHECK(hle_modmenu_view(&view, &art) && view.sel == 1 && !strcmp(view.note, "Rage note"), "value menu: the note follows the selection");
    hle_modmenu_input(0x8, none, 0, 0);   /* right */
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 3 && !strcmp(ret[0].s, "chosen") && ret[1].n == 2 && ret[2].n == 1, "value menu right: chosen, 2, 1");
    hle_modmenu_input(0, none, 0, 0);
    lua(hle_modmenu_lua_values, 1, "s:Off\nOff");
    CHECK(nret == 1 && hle_modmenu_view(&view, &art) && view.sel == 1 && !strcmp(view.value[1], "Off") && !strcmp(view.note, "Rage note"),
          "ModMenuValues again: values change, selection and notes stay");
    hle_modmenu_input(0, b_btn, 0, 0);
    lua(hle_modmenu_lua_state, 0);
    CHECK(nret == 2 && !strcmp(ret[0].s, "closed") && ret[1].n == 2, "value menu B: closed, 2");
    hle_modmenu_input(0, none, 0, 0);
    lua(hle_modmenu_lua_open, 3, "s:t", "s:x\ny", "s:");
    CHECK(hle_modmenu_view(&view, &art) && !view.values && !view.note[0], "ModMenuOpen: a plain menu again");
    lua(hle_modmenu_lua_close, 0);
    lua(hle_modmenu_lua_state, 0);
    hle_modmenu_input(0, none, 0, 0);

    /* ModMenuKey (mods version 1.11): Back + LB held 0.5 s, or the keyboard key (hle_modmenu_request). */
    static const uint8_t lb[8] = {0, 0, 0, 0, 0, 0xff};
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: nil before any request");
    for (int i = 0; i < 30; i++) t += 10, hle_modmenu_input(0x20, lb, 0, 0);
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: Back + LB held 0.3 s is not a request");
    for (int i = 0; i < 25; i++) t += 10, hle_modmenu_input(0x20, lb, 0, 0);
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].n == 1, "ModMenuKey: Back + LB held 0.5 s -> 1");
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: returned once");
    for (int i = 0; i < 100; i++) t += 10, hle_modmenu_input(0x20, lb, 0, 0);
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: one request per hold");
    hle_modmenu_input(0, none, 0, 0);
    for (int i = 0; i < 60; i++) t += 10, hle_modmenu_input(0x20, none, 0, 0);
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: Back alone is not a request");
    hle_modmenu_input(0, none, 0, 0);
    hle_modmenu_request("F8");
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].n == 1, "ModMenuKey: the keyboard key -> 1");
    hle_modmenu_request("F8");
    t += 1500;
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: a request older than 1 s is dropped (nobody asked in time)");
    lua(hle_modmenu_lua_open, 3, "s:t", "s:x\ny", "s:");
    hle_modmenu_request("F8");
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: no request while a menu is open");
    lua(hle_modmenu_lua_close, 0);
    lua(hle_modmenu_lua_state, 0);
    hle_modmenu_request("F8");
    hle_modmenu_new_state();
    lua(hle_modmenu_lua_key, 0);
    CHECK(nret == 1 && ret[0].type == 1, "ModMenuKey: a new Lua state drops a pending request");

    uint32_t f, c;
    int r[4];
    CHECK(!hle_modmenu_cover_parse("disc:f0529bb6:c9b3c1ce", &f, &c, r) && f == 0xf0529bb6u && c == 0xc9b3c1ceu && r[2] == 0,
          "cover: disc:<file>:<chunk>");
    CHECK(!hle_modmenu_cover_parse("disc:f0529bb6:c9b3c1ce:0,100,262,262", &f, &c, r) && r[1] == 100 && r[3] == 262, "cover: with a rect");
    CHECK(hle_modmenu_cover_parse("disc:zz:1", &f, &c, r) && hle_modmenu_cover_parse("file:1:2", &f, &c, r) &&
              hle_modmenu_cover_parse("disc:1:2:0,0,0,5", &f, &c, r) && hle_modmenu_cover_parse("disc:1:2:1,2,3", &f, &c, r),
          "cover: bad specs refused");
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
