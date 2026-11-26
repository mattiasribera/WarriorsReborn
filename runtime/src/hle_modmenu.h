/* Menus and music for mods (docs/MODDING.md "Menus and music"): the Lua
 * functions, the input they take from the pad and keyboard, the art they read from the player's
 * own disc and the mod music channel's policy. The pure parts are in src/modmenu.h; the drawing is
 * in src/hle_d3d.c (hle_d3d_modmenu_draw); the music voices in src/hle_audio.h (hle_audio_music_*).
 *
 * Lua (the mod's state; all run on the title's thread):
 *   ModMenuOpen(title, items, footer, sel, cover) -> item count, or nil
 *       items: one line per item ("\n"); a line "Label\tvags/music/name" previews that game sound
 *       while the item is selected (crossfade MENU_FADE_MS, after the selection rested 150 ms).
 *       sel: the item selected at first (1-based; nil = 1). cover: nil, or "disc:<file>:<chunk>" /
 *       "disc:<file>:<chunk>:<x>,<y>,<w>,<h>" (hex XBoxWad.idx name hash of a file, hex hash of
 *       a texture chunk in it, a texel rect): a texture of the player's disc shown as the cover.
 *       While the menu is open the game gets a neutral pad (the player stands still).
 *   ModMenuState() -> "open" | "chosen", i | "closed", i | nil
 *       "chosen" / "closed" (A / B) are returned once, with the item (1-based), then nil.
 *   ModMenuClose()  closes it as B would.
 *   ModMusicPlay(name, loop) -> 1 or nil   the mod's song (a game sound name, e.g.
 *       "vags/music/in_the_city"), looping unless loop is 0, on the game's radio (where the game
 *       played its last muted song), crossfaded; nil name: back to the game's own music.
 *   ModMusicPlaying() -> name or nil    the mod's song while it plays.
 *   ModMusicMute(names)  the game's sounds ("\n" separated names) kept silent while a mod's song
 *       or a menu preview plays (they fade back when it stops).
 *   ModMenuValues(values, notes) -> row count, or nil without an open menu   (mods version 1.11)
 *       makes the open menu a value menu (the settings menu's look): values one per row ("\n";
 *       a missing or empty line shows none), notes one per row (the selected row's text under the
 *       list). A, left and right report "chosen", i, dir (0 A, -1 left, 1 right) through
 *       ModMenuState and the menu stays open;
 *       called again it only changes the values (and notes, when given).
 *   ModMenuKey() -> 1 once after the player asked for a mod's menu (Back + LB held 0.5 s, or the
 *       keyboard key main.c passes to hle_modmenu_request: F8 unless bound) in the last second
 *       while no menu was open, else nil   (mods version 1.11)
 * Choosing (A) an item with a preview makes that song the mod's song (looping) without restarting
 * it; B fades the preview out and brings back what played before.
 *
 * Input: A, Y or Start (Enter, E) choose; B or Back (Backspace, F) and Escape close; d-pad / left
 * stick (arrows) move, left / right by a page. A new Lua state (a level load) closes the menu, stops
 * the mod's music and clears the mute set. */
#ifndef HLE_MODMENU_H
#define HLE_MODMENU_H
#include "modmenu.h"
#include <stdint.h>

#define MENU_FADE_MS 300.0

/* The music voices (src/hle_audio.h; tests pass fakes). */
typedef struct {
    int (*valid)(uint32_t cue);
    int (*start)(uint32_t cue, int loop, int on_radio, float level);
    void (*level)(int id, float level);
    int (*alive)(int id);
    void (*stop)(int id);
    void (*mute_set)(const uint32_t *cue, int n);
    void (*game_level)(float level);
} HleMenuAudio;
/* Wiring (main.c): the audio, the cue index of a sound name (-1 unknown), the disc folder and a
 * monotonic clock in ms (NULL: CLOCK_MONOTONIC). */
void hle_modmenu_setup(const HleMenuAudio *audio, int32_t (*cue_of)(const char *name), const char *disc_dir,
                       double (*now_ms)(void));
void hle_modmenu_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L),
                          void (*pushstring)(uint32_t L, const char *s));
int __attribute__((cdecl)) hle_modmenu_lua_open(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_state(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_close(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_music_play(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_music_playing(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_music_mute(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_values(uint32_t L);
int __attribute__((cdecl)) hle_modmenu_lua_key(uint32_t L);
/* The player asked for a mod's menu (how: for the log, e.g. "F8"); ignored while a menu is open. */
void hle_modmenu_request(const char *how);

/* A pad sample of port 0 on its way to the game (src/hle_input.c input_menu_filter): while the menu
 * is open (and until the buttons are released after it closed) it drives the menu and the game
 * gets a neutral pad (returns 1). buttons / analog / sticks as XINPUT_GAMEPAD. */
int hle_modmenu_input(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);
/* Escape (src/video.c): 1 = it closed the menu (the window keeps it), 0 = no menu open. */
int hle_modmenu_escape(void);
/* The music fades, every XACTEngineDoWork (hle_audio_music_tick). */
void hle_modmenu_tick(void);
/* A new Lua state booted (a level load): the menu closes and the mod's music stops. */
void hle_modmenu_new_state(void);

/* For the drawing thread: a copy of the open menu and its art (pixels stay valid for the run;
 * serial changes with the cover). Returns 0 when no menu is open. */
typedef struct {
    ModFont font;
    const uint32_t *font_px;       /* B8G8R8A8 atlas, NULL without the disc's font */
    const uint32_t *cover_px;      /* NULL: no cover */
    int cover_w, cover_h;
    float cover_uv[4];
    uint32_t cover_serial;
} HleMenuArt;
int hle_modmenu_view(ModMenu *out, HleMenuArt *art);
/* The game's font for another menu (the settings menu, src/hle_settings.h): loaded from the disc folder
 * on first use; art->font / font_px stay empty without it. */
void hle_modmenu_font(HleMenuArt *art);
void hle_modmenu_set_disc(const char *disc_dir);   /* the disc folder alone (no mods: no hle_modmenu_setup) */
/* Drawn before (instead of) a mod's menu when it returns 1: the settings menu's view (src/hle_d3d.c
 * modmenu_draw; set by src/hle_settings.c). */
extern int (*hle_d3d_menu_view)(ModMenu *out, HleMenuArt *art);

/* Parses a cover spec (see ModMenuOpen): 0 and the fields, or -1. rect w = 0: the whole texture. */
int hle_modmenu_cover_parse(const char *spec, uint32_t *file, uint32_t *chunk, int rect[4]);
/* Clears every state (tests). */
void hle_modmenu_reset(void);
#endif
