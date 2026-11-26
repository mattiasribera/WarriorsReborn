/* Radar dot colours for mods (see hle_radar.h). */
#include "hle_radar.h"
#include "hle_huspeed.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HLE_RADAR_TEST
#include "hle_devlua.h"
#include "platform.h"
#endif

int hle_radar_trace;
static struct { int id; uint32_t colour; } table[RADAR_CHARS];
static int nids;

int radar_set(int char_id, int32_t rgb) {
    if (char_id < 1 || char_id > 65535 || rgb > 0xffffff) return -1;
    int i;
    for (i = 0; i < nids && table[i].id != char_id; i++) {}
    if (rgb < 0) {
        if (i < nids) table[i] = table[--nids];
        return 0;
    }
    if (i == nids) {
        if (nids == RADAR_CHARS) return -1;
        nids++;
    }
    table[i].id = char_id;
    table[i].colour = (uint32_t)rgb << 8 | 0xffu;
    return 0;
}

void radar_clear(void) { nids = 0; }

uint32_t radar_colour_of(int char_id) {
    for (int i = 0; i < nids; i++)
        if (table[i].id == char_id) return table[i].colour;
    return 0;
}

int radar_human_kind(int kind) { return kind >= 6 && kind <= 9; }

int radar_char_frame(int32_t frame) {
    return frame == 0x160 || frame == 0x161 || frame == 0x164 || frame == 0x167 || frame == 0x16a || frame == RADAR_DOT_FRAME;
}

uint32_t radar_pick(int char_id, int kind, int layer, int32_t frame, uint32_t game_colour) {
    if (!radar_human_kind(kind)) return game_colour;
    if (layer != 0 && frame >= 0 && !radar_char_frame(frame)) return game_colour;
    const uint32_t c = radar_colour_of(char_id);
    if (!c) return game_colour;
    /* The game's tint over a dot of the id's colour: white keeps the colour, the game's grey dims
     * it as it dims its own dot; the game's alpha stays. */
    uint32_t out = game_colour & 0xffu;
    for (int sh = 8; sh < 32; sh += 8)
        out |= (((c >> sh) & 0xffu) * ((game_colour >> sh) & 0xffu) + 127u) / 255u << sh;
    return out;
}

int32_t radar_pick_frame(int char_id, int kind, int32_t frame) {
    return radar_human_kind(kind) && radar_char_frame(frame) && radar_colour_of(char_id) ? RADAR_DOT_FRAME : frame;
}

int32_t radar_parse_rgb(const char *s) {
    if (!s) return -1;
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i])) return -1;
    return (int32_t)strtol(s, NULL, 16);
}

#ifndef HLE_RADAR_TEST
static int state;   /* 0 not written, 1 in place, -1 unexpected bytes */

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

/* The human a handle names (high word: slot of the handle table, low word: its serial), 0 when the
 * handle is stale or not a human of the pool (as hle_huspeed). */
static uint32_t human_of(uint32_t handle) {
    const uint32_t slot = handle >> 16;
    if (!handle || slot >= 0x4000) return 0;
    const uint32_t e = HUSPEED_HANDLES + 8u * slot;
    if ((rd32(e + 4) & 0xffff) != (handle & 0xffff)) return 0;
    const uint32_t h = rd32(e);
    if (h < HUSPEED_POOL || h >= HUSPEED_POOL + HUSPEED_SLOTS * HUSPEED_STRIDE || (h - HUSPEED_POOL) % HUSPEED_STRIDE)
        return 0;
    return h;
}

static int char_of(uint32_t handle) {
    const uint32_t h = nids ? human_of(handle) : 0;
    return h ? (int)rd32(h + RADAR_CHAR_ID) : 0;
}

static int16_t kind_of(uint32_t item) { return *(const volatile int16_t *)(uintptr_t)(item + RADAR_ITEM_KIND); }

/* The layer-th item of a handle (as the game's 0xf0a30), 0 without one. */
static uint32_t item_of(uint32_t radar, uint32_t handle, uint32_t layer) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < RADAR_ITEMS; i++) {
        const uint32_t item = radar + RADAR_ITEM_BASE + i * RADAR_ITEM_STRIDE;
        if (rd32(item) == handle && n++ == layer) return item;
    }
    return 0;
}

/* RadarHUD::Add(handle, colour, kind, layer): args[1] is the colour (the frame is set after it). */
__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_add(uint32_t radar, uint32_t *args) {
    (void)radar;
    const int id = char_of(args[0]);
    if (!id) return;
    const uint32_t c = radar_pick(id, (int)args[2], (int)args[3], -1, args[1]);
    if (hle_radar_trace)
        printf("[radar] dot of %08x (character %d, kind %u, layer %u): %08x -> %08x\n", args[0], id, args[2], args[3], args[1], c);
    args[1] = c;
}

/* RadarHUD::SetColour(handle, colour, layer). */
__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_colour(uint32_t radar, uint32_t *args) {
    const int id = char_of(args[0]);
    const uint32_t item = id && radar ? item_of(radar, args[0], args[2]) : 0;
    if (item) args[1] = radar_pick(id, kind_of(item), (int)args[2], (int32_t)rd32(item + RADAR_ITEM_FRAME), args[1]);
}

/* RadarHUD::SetFrame(handle, frame, layer, size). */
__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_frame(uint32_t radar, uint32_t *args) {
    const int id = char_of(args[0]);
    const uint32_t item = id && radar ? item_of(radar, args[0], args[2]) : 0;
    if (item) args[1] = (uint32_t)radar_pick_frame(id, kind_of(item), (int32_t)args[1]);
}

/* RadarHUD::SetItemFrame(index, frame, size). */
__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_item_frame(uint32_t radar, uint32_t *args) {
    if (!radar || args[0] >= RADAR_ITEMS) return;
    const uint32_t item = radar + RADAR_ITEM_BASE + args[0] * RADAR_ITEM_STRIDE;
    const int id = char_of(rd32(item));
    if (id) args[1] = (uint32_t)radar_pick_frame(id, kind_of(item), (int32_t)args[1]);
}

static const uint8_t sig_add[RADAR_ADD_LEN] = {0x83, 0xec, 0x18, 0x8b, 0x44, 0x24, 0x28};
static const uint8_t sig_colour[RADAR_COLOUR_LEN] = {0x56, 0x8b, 0xf1, 0x80, 0x3e, 0x00};
/* cmp byte [ecx], 0; jz 0xf09af */
static const uint8_t sig_item_frame[RADAR_ITEM_FRAME_LEN] = {0x80, 0x39, 0x00, 0x0f, 0x84, 0xb6, 0x00, 0x00, 0x00};

/* pushad; lea eax, [esp + 0x24]; push eax; push ecx; call hook; add esp, 8; popad; <the entry's
 * first instructions>; jmp entry + len. The hook sees the radar (ecx) and the stack arguments. With
 * jz_len 6, the entry's last instruction is a jz rel32, written again for its new place. */
static void write_entry(uint32_t fn, const uint8_t *sig, uint32_t len, void *hook, uint32_t jz_len) {
    uint8_t *s = plat_alloc_exec(48), *q = s;
    *q++ = 0x60;
    *q++ = 0x8d; *q++ = 0x44; *q++ = 0x24; *q++ = 0x24;
    *q++ = 0x50;
    *q++ = 0x51;
    int32_t rel = (int32_t)((uintptr_t)hook - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x08;
    *q++ = 0x61;
    memcpy(q, sig, len - jz_len); q += len - jz_len;
    if (jz_len) {
        int32_t old;
        memcpy(&old, sig + len - 4, 4);
        const uint32_t target = fn + len + (uint32_t)old;
        *q++ = 0x0f; *q++ = 0x84;
        rel = (int32_t)(target - (uint32_t)(uintptr_t)(q + 4));
        memcpy(q, &rel, 4); q += 4;
    }
    rel = (int32_t)(fn + len - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4);
    uint8_t *p = (uint8_t *)(uintptr_t)fn;
    rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
    p[0] = 0xe9; memcpy(p + 1, &rel, 4);
    for (uint32_t i = 5; i < len; i++) p[i] = 0xcc;
}

int radar_patch(void) {
    if (state) return state;
    if (memcmp((const void *)(uintptr_t)RADAR_ADD_FN, sig_add, sizeof sig_add) ||
        memcmp((const void *)(uintptr_t)RADAR_COLOUR_FN, sig_colour, sizeof sig_colour) ||
        memcmp((const void *)(uintptr_t)RADAR_FRAME_FN, sig_colour, sizeof sig_colour) ||
        memcmp((const void *)(uintptr_t)RADAR_ITEM_FRAME_FN, sig_item_frame, sizeof sig_item_frame)) {
        printf("[radar] skip dot colours at %08x, %08x, %08x and %08x: unexpected bytes\n", RADAR_ADD_FN, RADAR_COLOUR_FN,
               RADAR_FRAME_FN, RADAR_ITEM_FRAME_FN);
        return state = -1;
    }
    write_entry(RADAR_ADD_FN, sig_add, RADAR_ADD_LEN, (void *)hook_add, 0);
    write_entry(RADAR_COLOUR_FN, sig_colour, RADAR_COLOUR_LEN, (void *)hook_colour, 0);
    write_entry(RADAR_FRAME_FN, sig_colour, RADAR_COLOUR_LEN, (void *)hook_frame, 0);
    write_entry(RADAR_ITEM_FRAME_FN, sig_item_frame, RADAR_ITEM_FRAME_LEN, (void *)hook_item_frame, 6);
    printf("[radar] dot colours at %08x (add), %08x (colour), %08x and %08x (frame)\n", RADAR_ADD_FN, RADAR_COLOUR_FN,
           RADAR_FRAME_FN, RADAR_ITEM_FRAME_FN);
    return state = 1;
}

static void (*push_number)(uint32_t L, double n);
static void (*push_nil)(uint32_t L);

void radar_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L)) {
    push_number = pushnumber;
    push_nil = pushnil;
}

/* ModRadarColor(char_id, "RRGGBB") / ModRadarColor(char_id) / ModRadarColor(): 1, or nil (logged). */
__attribute__((force_align_arg_pointer, cdecl)) int hle_radar_lua(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    int ok = -1;
    const int t1 = a->type(L, 1), t2 = a->type(L, 2);
    if (t1 <= 0) {   /* none or nil: clear */
        radar_clear();
        ok = 0;
    } else if (t1 == 2) {
        char *e = NULL;
        const char *id = a->tostring(L, 1);
        const long n = strtol(id, &e, 10);
        const int32_t rgb = t2 <= 0 ? -1 : t2 == 3 ? radar_parse_rgb(a->tostring(L, 2)) : -2;
        if (*e == '\0' && rgb >= -1 && radar_set((int)n, rgb) == 0) {
            ok = 0;
            if (rgb >= 0 && radar_patch() < 0) ok = -1;
        }
        if (ok == 0 && hle_radar_trace) printf("[radar] character %ld: %s\n", n, rgb >= 0 ? a->tostring(L, 2) : "game colour");
    }
    if (ok) printf("[radar] ModRadarColor: not a character id and an \"RRGGBB\" colour, %d ids already set, or the hook is not available; ignored\n", RADAR_CHARS);
    fflush(stdout);
    if (ok == 0 && push_number) push_number(L, 1);
    else if (push_nil) push_nil(L);
    return 1;
}
#endif

#ifdef HLE_RADAR_TEST
/* Game-independent test: the colour table, the colour parser and the pick of a dot's colour. */
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    CHECK(radar_colour_of(180) == 0, "no colours at start");
    CHECK(radar_parse_rgb("1E3CB4") == 0x1e3cb4 && radar_parse_rgb("#ff8000") == 0xff8000, "RRGGBB and #RRGGBB parse");
    CHECK(radar_parse_rgb("ff800") == -1 && radar_parse_rgb("ff80001") == -1 && radar_parse_rgb("gg8000") == -1 &&
              radar_parse_rgb("") == -1 && radar_parse_rgb(NULL) == -1,
          "other texts are refused");
    CHECK(radar_set(180, 0x1e3cb4) == 0 && radar_colour_of(180) == 0x1e3cb4ffu, "a colour is stored opaque (0xRRGGBBAA)");
    CHECK(radar_set(0, 0xffffff) == -1 && radar_set(65536, 0) == -1 && radar_set(5, 0x1000000) == -1, "bad ids and colours are refused");
    /* The pick: character dots (kinds 6-9: 6 gangs, 7 and 9 Warriors and radar type 0, 8 police), layer 0 only. */
    CHECK(radar_pick(180, 7, 0, -1, 0xffffffffu) == 0x1e3cb4ffu, "a dot of kind 7 takes the id's colour");
    CHECK(radar_pick(180, 9, 0, -1, 0xffffffffu) == 0x1e3cb4ffu, "kind 9 too");
    CHECK(radar_pick(180, 6, 0, -1, 0x808080ffu) == 0x0f1e5affu, "the game's grey tint dims the colour as it dims its own dot");
    CHECK(radar_pick(180, 8, 0, -1, 0xffffff00u) == 0x1e3cb400u, "the game's alpha is kept");
    CHECK(radar_set(245, 0x33ff33) == 0 && radar_pick(245, 8, 0, -1, 0xffffffffu) == 0x33ff33ffu, "a police dot (kind 8) takes it too");
    CHECK(radar_pick(180, 6, 1, 0x167, 0xffffffffu) == 0x1e3cb4ffu && radar_pick(180, 8, 1, -1, 0xffffffffu) == 0x1e3cb4ffu,
          "layer 1 takes it while it shows a character frame (or none yet)");
    CHECK(radar_pick(180, 6, 1, 0x21, 0xffffffffu) == 0xffffffffu && radar_pick(180, 6, 1, 0x20, 0xffffffffu) == 0xffffffffu,
          "an alert mark on layer 1 (frames 0x20, 0x21) keeps the game's colour");
    CHECK(radar_pick_frame(180, 6, 0x160) == RADAR_DOT_FRAME && radar_pick_frame(180, 6, 0x167) == RADAR_DOT_FRAME &&
              radar_pick_frame(180, 8, 0x164) == RADAR_DOT_FRAME && radar_pick_frame(180, 9, 0x16a) == RADAR_DOT_FRAME &&
              radar_pick_frame(180, 6, 0x161) == RADAR_DOT_FRAME,
          "a coloured character's dot frames (gang 0x160/0x167, alerted 0x161, police 0x164, 0x16a) become the white dot 0x16d");
    CHECK(radar_pick_frame(180, 6, 0x21) == 0x21 && radar_pick_frame(180, 6, 350) == 350 && radar_pick_frame(181, 8, 0x164) == 0x164 &&
              radar_pick_frame(180, 2, 0x160) == 0x160,
          "other frames, ids without a colour and other kinds keep the game's frame");
    CHECK(radar_pick(180, 6, 0, -1, 0xffffffffu) == 0x1e3cb4ffu, "a gang dot (kind 6) takes it too");
    CHECK(radar_pick(180, 2, 0, -1, 0x63db4bffu) == 0x63db4bffu && radar_pick(180, 5, 0, -1, 0x808080ffu) == 0x808080ffu &&
              radar_pick(180, 10, 0, -1, 0xffffffffu) == 0xffffffffu,
          "objectives and other markers keep the game's colour");
    CHECK(radar_pick(181, 7, 0, -1, 0xffffffffu) == 0xffffffffu, "an id without a colour keeps the game's");
    CHECK(radar_set(180, 0xe0b000) == 0 && radar_pick(180, 7, 0, -1, 0xffffffffu) == 0xe0b000ffu, "an id can change colour");
    CHECK(radar_set(180, -1) == 0 && radar_colour_of(180) == 0 && radar_colour_of(245) == 0x33ff33ffu, "removing one id keeps the others");
    int full = 1;
    radar_clear();
    for (int i = 0; i < RADAR_CHARS; i++) full &= radar_set(1 + i, i) == 0;
    CHECK(full && radar_set(9999, 1) == -1 && radar_set(1, 0x123456) == 0, "the table holds %d ids (an id already set can still change)", RADAR_CHARS);
    radar_clear();
    CHECK(radar_colour_of(1) == 0 && radar_colour_of(245) == 0, "clear removes every colour");
    CHECK(radar_human_kind(6) && radar_human_kind(7) && radar_human_kind(8) && radar_human_kind(9) && !radar_human_kind(5) &&
              !radar_human_kind(10),
          "character dot kinds are 6 to 9");
    printf(failures ? "FAILED %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
