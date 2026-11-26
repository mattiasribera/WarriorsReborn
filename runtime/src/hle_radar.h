/* Radar dot colours for mods: the Lua function ModRadarColor(char_id, colour).
 *
 * The title's radar (GUI\RadarHUD.cpp) keeps 0x80 items of 0x38 bytes from radar + 0xac: the object
 * handle (+0xac), its colour (+0xb0, 0xRRGGBBAA), its frame (+0xb4), the dot object (+0xc0), its kind
 * (+0xc8, short) and in-use flag (+0xcd). Two radars live in the HUD (+0x11b8 and +0x2e68) and every
 * human dot is added to both.
 *
 * When a character is created (0x14bb80) the radar type of his CfgChar picks his dot: 0 (enemy) adds
 * kind 7 (colour 0xffffffff, frame 0x16d) or kind 9 (0x787878ff, frame 0x16a) through 0xd88f0, 1
 * (police) kind 8 (0xffffffff, frame 0x164) through 0xdc750, which then sets colour 0xffffff00. The
 * item is created by RadarHUD::Add (0xf2b60, thiscall radar: handle, colour, kind, layer); the colour
 * is the tint of the "hud_radar_dot" object, which RadarHUD::SetColour (0xf12a0, thiscall radar:
 * handle, colour, layer) changes later (0x110810 writes the dot's +0xa4/+0xa8). The game's own green
 * objective markers are tinted that way (0xd86e0: 0x63db4bff).
 *
 * ModRadarColor(char_id, "RRGGBB") makes the dot of every character of that CfgChar id (human + 0xa8)
 * that colour: both entries are hooked (written the first time a colour is set) and, for a human
 * dot (kinds 7, 8, 9) on layer 0 of a character with a colour, the colour argument is replaced.
 * ModRadarColor(char_id) removes one id, ModRadarColor() every id. The table is the runtime's and
 * lasts until the game exits (the gang characters are configured once, in the front end's Lua). */
#ifndef HLE_RADAR_H
#define HLE_RADAR_H
#include <stdint.h>

#define RADAR_ADD_FN 0x000f2b60u      /* RadarHUD::Add: sub esp,0x18; mov eax,[esp+0x28] (7 bytes) */
#define RADAR_ADD_LEN 7u
#define RADAR_COLOUR_FN 0x000f12a0u   /* RadarHUD::SetColour: push esi; mov esi,ecx; cmp byte [esi],0 */
#define RADAR_COLOUR_LEN 6u
#define RADAR_FRAME_FN 0x000f1190u    /* RadarHUD::SetFrame(handle, frame, layer, size): same first bytes as SetColour */
#define RADAR_ITEM_FRAME_FN 0x000f08f0u /* RadarHUD::SetItemFrame(index, frame, size): cmp byte [ecx],0; jz 0xf09af */
#define RADAR_ITEM_FRAME_LEN 9u
#define RADAR_DOT_FRAME 0x16d         /* the plain white round dot (the Warriors'): the tint shows as it is */
#define RADAR_ITEMS 0x80
#define RADAR_ITEM_BASE 0xacu         /* radar + 0xac + 0x38 * i: the handle */
#define RADAR_ITEM_STRIDE 0x38u
#define RADAR_ITEM_KIND 0x1cu         /* item + 0x1c (radar + 0xc8): kind (short) */
#define RADAR_ITEM_FRAME 0x08u        /* item + 0x08 (radar + 0xb4): frame */
#define RADAR_CHAR_ID 0xa8u           /* human + 0xa8: CfgChar id */

#define RADAR_CHARS 512               /* character ids with a colour at once */

/* Colour of a character id: rgb 0xRRGGBB (shown opaque), or -1 to remove it. Returns 0, or -1 when
 * the id is not 1-65535 or the table is full. radar_clear removes all. */
int radar_set(int char_id, int32_t rgb);
void radar_clear(void);
/* The game colour (0xRRGGBBAA) of a character id, 0 without one. */
uint32_t radar_colour_of(int char_id);
/* 1 for the kinds of item that are a character's dot (6 to 9). */
int radar_human_kind(int kind);
/* 1 for the game's character dot frames (0x160, 0x161, 0x164, 0x167, 0x16a, 0x16d). */
int radar_char_frame(int32_t frame);
/* The colour to use: for a character dot, on layer 0 or on a layer that shows a character frame
 * (frame -1: not set yet), the id's colour times the game's tint (white: the colour; the game's grey
 * dims it) with the game's alpha; else the game's (alert marks on layer 1 keep theirs). */
uint32_t radar_pick(int char_id, int kind, int layer, int32_t frame, uint32_t game_colour);
/* The frame to use: RADAR_DOT_FRAME in place of a character frame of a coloured character's dot. */
int32_t radar_pick_frame(int char_id, int kind, int32_t frame);
/* "RRGGBB" or "#RRGGBB" (hex) to 0xRRGGBB, -1 when it is not that. */
int32_t radar_parse_rgb(const char *s);

/* Installs both hooks if needed (1 = in place, -1 unexpected bytes). */
int radar_patch(void);
/* ModRadarColor for the title's Lua (cdecl C function, registered by hle_mods). */
void radar_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L));
int __attribute__((cdecl)) hle_radar_lua(uint32_t L);
extern int hle_radar_trace;
#endif
