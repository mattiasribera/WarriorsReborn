/* The controller-port rules of hle_input.h that need no HLE state: which port a host controller
 * takes (input_pick_port, input_pick_port_for and the --pad-slot reservations) and the pad state of
 * a set of controls (input_from_controls). Kept apart from hle_input.c so the x86_64 render
 * helper (tools/render64/r64_helper.c), which owns the SDL
 * window and its controllers, links the same rules as the game. */
#include "hle_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int input_pick_port(uint32_t used, int prev) {
    if (prev >= 0 && prev < INPUT_PORTS && !(used >> prev & 1)) return prev;
    for (int p = 0; p < INPUT_PORTS; p++) if (!(used >> p & 1)) return p;
    return -1;
}

/* --pad-slot reservations, one per player (port): set before the controllers are opened and
 * read by the video thread only. nth 0: any controller with that GUID. */
static struct { int set, nth; uint8_t guid[16]; } pad_slot[INPUT_PORTS];
static char pad_slot_text[INPUT_PORTS][48];   /* the spec as given, for the render64 helper's command line */

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
int input_pad_guid_parse(const char *s, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) {
        int hi = hex_digit(s[2 * i]), lo = hi < 0 ? -1 : hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}
/* The same controller: SDL 2.26+ puts a CRC of the name in bytes 2-3 of some GUIDs, older
 * versions leave them 0, so a 0 on either side matches any. */
static int guid_same(const uint8_t *a, const uint8_t *b) {
    if (!memcmp(a, b, 16)) return 1;
    return (!(a[2] | a[3]) || !(b[2] | b[3])) && !memcmp(a, b, 2) && !memcmp(a + 4, b + 4, 12);
}
int input_pad_slot_parse(const char *spec, char *err, size_t errlen) {
    char *end;
    long n = strtol(spec, &end, 10);
    uint8_t guid[16];
    if (end == spec || *end != '=' || n < 1 || n > INPUT_PORTS) {
        snprintf(err, errlen, "'%s': expected N=GUID with player N 1-%d", spec, INPUT_PORTS);
        return -1;
    }
    const char *g = end + 1;
    if (strnlen(g, 33) < 32 || input_pad_guid_parse(g, guid) < 0 || (g[32] && g[32] != '#')) {
        snprintf(err, errlen, "'%s': the GUID is 32 hex digits (as in gamecontrollerdb.txt)", spec);
        return -1;
    }
    long nth = 0;
    if (g[32] == '#') {
        nth = strtol(g + 33, &end, 10);
        if (end == g + 33 || *end || nth < 1 || nth > 99) {
            snprintf(err, errlen, "'%s': #K counts the identical controllers from 1", spec);
            return -1;
        }
    }
    snprintf(pad_slot_text[n - 1], sizeof pad_slot_text[n - 1], "%s", spec);
    pad_slot[n - 1].set = 1;
    pad_slot[n - 1].nth = (int)nth;
    memcpy(pad_slot[n - 1].guid, guid, 16);
    return 0;
}
void input_pad_slots_clear(void) { memset(pad_slot, 0, sizeof pad_slot); }
const char *input_pad_slot_spec(int player) {
    return player >= 1 && player <= INPUT_PORTS && pad_slot[player - 1].set ? pad_slot_text[player - 1] : NULL;
}
int input_pad_reserved(const uint8_t guid[16]) {
    for (int p = 0; p < INPUT_PORTS; p++)
        if (pad_slot[p].set && guid_same(pad_slot[p].guid, guid)) return 1;
    return 0;
}
int input_pick_port_for(uint32_t used, int prev, const uint8_t guid[16], int nth) {
    uint32_t reserved = 0;
    for (int p = 0; p < INPUT_PORTS; p++) if (pad_slot[p].set) reserved |= 1u << p;
    if (!reserved) return input_pick_port(used, prev);
    for (int pass = 0; pass < 2; pass++)   /* its own reservation, then another free one of its GUID */
        for (int p = 0; p < INPUT_PORTS; p++)
            if ((reserved & ~used) >> p & 1 && guid_same(pad_slot[p].guid, guid) &&
                (pass || !pad_slot[p].nth || pad_slot[p].nth == nth)) return p;
    int port = input_pick_port(used | reserved, prev);   /* a port nobody chose, then any free one */
    return port >= 0 ? port : input_pick_port(used, -1);
}

static int16_t axis(uint32_t mask, int neg, int pos) {
    return (int16_t)((mask >> pos & 1 ? 32767 : 0) - (mask >> neg & 1 ? 32767 : 0));
}
void input_from_controls(uint32_t mask, XPad *out) {
    memset(out, 0, sizeof *out);
    out->buttons = mask & 0xFF;
    for (int i = 0; i < 8; i++) out->analog[i] = mask >> (IN_A + i) & 1 ? 0xFF : 0;
    out->lx = axis(mask, IN_LS_LEFT, IN_LS_RIGHT);
    out->ly = axis(mask, IN_LS_DOWN, IN_LS_UP);
    out->rx = axis(mask, IN_RS_LEFT, IN_RS_RIGHT);
    out->ry = axis(mask, IN_RS_DOWN, IN_RS_UP);
}
