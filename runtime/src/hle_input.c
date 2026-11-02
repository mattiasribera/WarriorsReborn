/* HLE input boundary (see hle_input.h). How the title uses the entry
 * points: it calls XInitDevices once,
 * XGetDevices for XDEVICE_TYPE_GAMEPAD, opens every connected port with auto-polling, reads
 * XInputGetState every frame and checks XGetDeviceChanges for hot-plug. All entry points are
 * stdcall; argument bytes come from each function's own "ret N". */
#define _GNU_SOURCE
#include "hle_input.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#define ERROR_SUCCESS 0u
#define ERROR_IO_PENDING 997u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define XINPUT_DEVSUBTYPE_GC_GAMEPAD 1u

#define G(p) ((void *)(uintptr_t)(p))
#define GA(p) ((uint32_t)(uintptr_t)(p))

/* ---- host input state ---- */
static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t src_mask[INPUT_SRC_COUNT];   /* port 0: keyboard and script */
static uint32_t port_script[INPUT_PORTS];    /* ports 1-3: script controls (port 0 uses src_mask) */
static uint32_t script_plugged;              /* ports 1-3 plugged by the script */
static XPad ctl_pad[INPUT_PORTS];
static int ctl_present[INPUT_PORTS];
static int16_t mouse_rx, mouse_ry;
static int spin_on;
static double spin_t0;
static XPad last_pad[INPUT_PORTS];
static uint32_t packet[INPUT_PORTS];
/* Connected ports, and what the title was last told (XGetDevices / XGetDeviceChanges). Port 0
 * never leaves. Changes in between accumulate as on the Xbox: a pad pulled and put back before
 * the title looks is reported as both a removal and an insertion. */
static uint32_t ports = 1u, reported = 1u, ports_ins, ports_rem;

typedef struct { uint32_t magic, port; } PadHandle;
#define PAD_MAGIC 0x44415058u   /* "XPAD" */
static PadHandle handles[INPUT_PORTS];

static void ports_update_locked(void) {
    uint32_t now = 1u;
    for (int p = 1; p < INPUT_PORTS; p++)
        if (ctl_present[p] || script_plugged >> p & 1) now |= 1u << p;
    uint32_t gone = ports & ~now;
    ports_rem |= gone;
    ports_ins |= now & ~ports;
    for (int p = 0; p < INPUT_PORTS; p++)
        if (gone >> p & 1) handles[p].magic = 0;   /* its handle stops working, as on the Xbox */
    ports = now;
}
uint32_t input_connected_ports(void) {
    pthread_mutex_lock(&in_lock);
    uint32_t c = ports;
    pthread_mutex_unlock(&in_lock);
    return c;
}
int input_controller_connected(void) {
    pthread_mutex_lock(&in_lock);
    int any = 0;
    for (int p = 0; p < INPUT_PORTS; p++) any |= ctl_present[p];
    pthread_mutex_unlock(&in_lock);
    return any;
}
int input_pick_port(uint32_t used, int prev) {
    if (prev >= 0 && prev < INPUT_PORTS && !(used >> prev & 1)) return prev;
    for (int p = 0; p < INPUT_PORTS; p++) if (!(used >> p & 1)) return p;
    return -1;
}

/* --pad-slot reservations, one per player (port): set before the controllers are opened and
 * read by the video thread only. nth 0: any controller with that GUID. */
static struct { int set, nth; uint8_t guid[16]; } pad_slot[INPUT_PORTS];

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
    pad_slot[n - 1].set = 1;
    pad_slot[n - 1].nth = (int)nth;
    memcpy(pad_slot[n - 1].guid, guid, 16);
    return 0;
}
void input_pad_slots_clear(void) { memset(pad_slot, 0, sizeof pad_slot); }
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

static const char *const control_names[IN_COUNT] = {
    "up", "down", "left", "right", "start", "back", "lthumb", "rthumb",
    "a", "b", "x", "y", "black", "white", "lt", "rt",
    "lsup", "lsdown", "lsleft", "lsright", "rsup", "rsdown", "rsleft", "rsright",
};
const char *input_control_name(int c) { return c >= 0 && c < IN_COUNT ? control_names[c] : "?"; }

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

void input_set_control(int src, int control, int down) {
    if (src < 0 || src >= INPUT_SRC_COUNT || control < 0 || control >= IN_COUNT) return;
    pthread_mutex_lock(&in_lock);
    if (down) src_mask[src] |= 1u << control;
    else src_mask[src] &= ~(1u << control);
    pthread_mutex_unlock(&in_lock);
}

void input_set_mask(int src, uint32_t mask) {
    if (src < 0 || src >= INPUT_SRC_COUNT) return;
    pthread_mutex_lock(&in_lock);
    src_mask[src] = mask & ((1u << IN_COUNT) - 1);
    pthread_mutex_unlock(&in_lock);
}

void input_set_mouse_stick(int16_t rx, int16_t ry) {
    pthread_mutex_lock(&in_lock);
    mouse_rx = rx;
    mouse_ry = ry;
    pthread_mutex_unlock(&in_lock);
}

float input_spin_rate = 2.0f;
static double mono_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
double (*input_clock)(void) = mono_now;

void input_set_spin(int on) {
    pthread_mutex_lock(&in_lock);
    if (on && !spin_on) spin_t0 = input_clock();   /* each press starts at up */
    spin_on = on != 0;
    pthread_mutex_unlock(&in_lock);
}

static volatile int last_device;
void input_note_device(int device) { last_device = device; }
int input_last_device(void) { return last_device; }
int input_pad_active(const XPad *p) {
    if (p->buttons) return 1;
    for (int i = 0; i < 8; i++) if (p->analog[i] > 64) return 1;
    return abs(p->lx) > 16384 || abs(p->ly) > 16384 || abs(p->rx) > 16384 || abs(p->ry) > 16384;
}

void input_set_port_controller(int port, const XPad *pad) {
    if (port < 0 || port >= INPUT_PORTS) return;
    if (pad && input_pad_active(pad)) input_note_device(INPUT_DEVICE_PAD);
    pthread_mutex_lock(&in_lock);
    ctl_present[port] = pad != NULL;
    if (pad) ctl_pad[port] = *pad;
    ports_update_locked();
    pthread_mutex_unlock(&in_lock);
}
void input_set_controller(const XPad *pad) { input_set_port_controller(0, pad); }

static void merge_controller(XPad *p, uint32_t mask, const XPad *c) {
    p->buttons |= c->buttons;
    for (int i = 0; i < 8; i++) if (c->analog[i] > p->analog[i]) p->analog[i] = c->analog[i];
    if (!(mask & (0xFu << IN_LS_UP))) { p->lx = c->lx; p->ly = c->ly; }
    if (!(mask & (0xFu << IN_RS_UP))) { p->rx = c->rx; p->ry = c->ry; }
}

/* Keyboard and script controls merged with the controller: buttons are ORed, analog buttons
 * take the larger value, a stick follows the keys while one of its keys is held. The right
 * stick then follows the mouse while it moves, else the controller; the left stick follows the
 * spin while it is on (angle 0 is up, then left, down, right: x = -sin, y = cos; the title's
 * car-theft screws read that order as clockwise). */
void input_get_port(int port, XPad *out, uint32_t *pkt) {
    if (port < 0 || port >= INPUT_PORTS) { memset(out, 0, sizeof *out); if (pkt) *pkt = 0; return; }
    pthread_mutex_lock(&in_lock);
    XPad p;
    uint32_t mask = 0;
    if (port == 0)
        for (int i = 0; i < INPUT_SRC_COUNT; i++) mask |= src_mask[i];
    else
        mask = port_script[port];
    input_from_controls(mask, &p);
    if (ctl_present[port]) merge_controller(&p, mask, &ctl_pad[port]);
    if (port == 0) {   /* the mouse and the spin belong to player 1 */
        if (!(mask & (0xFu << IN_RS_UP)) && (mouse_rx || mouse_ry)) { p.rx = mouse_rx; p.ry = mouse_ry; }
        if (spin_on) {
            double turns = (input_clock() - spin_t0) * input_spin_rate;
            double a = 2 * M_PI * (turns - floor(turns));
            p.lx = (int16_t)lrint(-sin(a) * 32767);
            p.ly = (int16_t)lrint(cos(a) * 32767);
        }
    }
    if (memcmp(&p, &last_pad[port], sizeof p)) { last_pad[port] = p; packet[port]++; }
    *out = p;
    if (pkt) *pkt = packet[port];
    pthread_mutex_unlock(&in_lock);
}
void input_get(XPad *out, uint32_t *pkt) { input_get_port(0, out, pkt); }

/* ---- scripted input ---- */
/* A step holds `mask` on `port` for `frames` frames from Swap `frame`; a plug step (SCRIPT_PLUG /
 * SCRIPT_UNPLUG) connects or disconnects a port from that frame on. */
enum { SCRIPT_HOLD, SCRIPT_PLUG, SCRIPT_UNPLUG };
typedef struct { uint32_t frame, frames, mask; uint8_t port, kind; } ScriptStep;
#define SCRIPT_MAX_STEPS 1024   /* long headless gameplay runs */
static ScriptStep script[SCRIPT_MAX_STEPS];
static int script_len;

static int parse_steps(char *buf, char *err, size_t errlen);
int input_script_parse(const char *spec, char *err, size_t errlen) {
    char *buf = strdup(spec);
    if (!buf) { snprintf(err, errlen, "out of memory"); return -1; }
    int n = parse_steps(buf, err, errlen);
    free(buf);
    return n;
}
static int parse_steps(char *buf, char *err, size_t errlen) {
    int n = 0;
    char *save = NULL;
    for (char *step = strtok_r(buf, ",", &save); step; step = strtok_r(NULL, ",", &save)) {
        if (n == (int)(sizeof script / sizeof *script)) { snprintf(err, errlen, "too many steps (max %d)", SCRIPT_MAX_STEPS); return -1; }
        char *end, *names = strchr(step, ':');
        ScriptStep s = {strtoul(step, &end, 10), 6, 0, 0, SCRIPT_HOLD};
        if (!names || end != names || end == step) { snprintf(err, errlen, "'%s': expected FRAME:[pN/]CONTROLS[:FRAMES]", step); return -1; }
        names++;
        char *dur = strchr(names, ':');
        if (dur) {
            *dur++ = 0;
            s.frames = strtoul(dur, &end, 10);
            if (*end || !*dur || !s.frames) { snprintf(err, errlen, "'%s': bad frame count", dur); return -1; }
        }
        if (names[0] == 'p' && names[1] >= '0' && names[1] <= '9' && names[2] == '/') {   /* pN/: port N */
            if (names[1] - '0' >= INPUT_PORTS) { snprintf(err, errlen, "'%.3s': port must be p0-p%d", names, INPUT_PORTS - 1); return -1; }
            s.port = (uint8_t)(names[1] - '0');
            names += 3;
        }
        if (!strcmp(names, "plug") || !strcmp(names, "unplug")) {
            if (!s.port) { snprintf(err, errlen, "port 0 is always connected (plug/unplug need p1-p%d)", INPUT_PORTS - 1); return -1; }
            s.kind = names[0] == 'p' ? SCRIPT_PLUG : SCRIPT_UNPLUG;
            script[n++] = s;
            continue;
        }
        char *save2 = NULL;
        for (char *nm = strtok_r(names, "+", &save2); nm; nm = strtok_r(NULL, "+", &save2)) {
            int c = 0;
            while (c < IN_COUNT && strcmp(nm, control_names[c])) c++;
            if (c == IN_COUNT) { snprintf(err, errlen, "unknown control '%s'", nm); return -1; }
            s.mask |= 1u << c;
        }
        if (!s.mask) { snprintf(err, errlen, "step at frame %u has no controls", s.frame); return -1; }
        script[n++] = s;
    }
    script_len = n;
    return 0;
}

static void log_script_mask(uint32_t frame, int port, uint32_t mask) {
    char names[160] = "";
    int len = 0;
    for (int c = 0; c < IN_COUNT; c++)
        if (mask >> c & 1 && len < (int)sizeof names - 12) len += snprintf(names + len, sizeof names - len, "%s%s", len ? "+" : "", control_names[c]);
    if (port) printf("[input] frame %u: script p%d/%s\n", frame, port, len ? names : "released");
    else printf("[input] frame %u: script %s\n", frame, len ? names : "released");
}

void input_frame(uint32_t frame) {
    if (!script_len) return;
    uint32_t mask[INPUT_PORTS] = {0}, plug_frame[INPUT_PORTS] = {0}, plugged = 0;
    for (int i = 0; i < script_len; i++) {
        const ScriptStep *st = &script[i];
        if (frame < st->frame) continue;
        if (st->kind == SCRIPT_HOLD) {
            if (frame - st->frame < st->frames) mask[st->port] |= st->mask;
        } else if (st->frame >= plug_frame[st->port]) {   /* the latest plug/unplug wins */
            plug_frame[st->port] = st->frame;
            plugged = st->kind == SCRIPT_PLUG ? plugged | 1u << st->port : plugged & ~(1u << st->port);
        }
    }
    pthread_mutex_lock(&in_lock);
    for (int p = 0; p < INPUT_PORTS; p++) {   /* one line per script transition */
        uint32_t *cur = p ? &port_script[p] : &src_mask[INPUT_SRC_SCRIPT];
        if (mask[p] != *cur) log_script_mask(frame, p, mask[p]);
        *cur = mask[p];
        if ((plugged ^ script_plugged) >> p & 1) printf("[input] frame %u: script p%d/%s\n", frame, p, plugged >> p & 1 ? "plug" : "unplug");
    }
    script_plugged = plugged;
    ports_update_locked();
    pthread_mutex_unlock(&in_lock);
}

/* ---- XAPI entry points ---- */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t stub_ret, index, ret_addr;
    uint32_t args[];
} InFrame;

static PadHandle *handle_get(uint32_t h) {
    PadHandle *p = G(h);
    for (int i = 0; i < INPUT_PORTS; i++) if (p == &handles[i] && p->magic == PAD_MAGIC) return p;
    return NULL;
}

static uint32_t init_devices(const uint32_t *a) { (void)a; return 0; }   /* (count, pPreallocTypes) */
static uint32_t enum_status(const uint32_t *a) { (void)a; return 0; }    /* XDEVICE_ENUMERATION_IDLE */
static uint32_t get_devices(const uint32_t *a) {                        /* (type) -> port bitmap */
    if (a[0] != XAPI_GAMEPAD_TYPE) return 0;
    pthread_mutex_lock(&in_lock);
    uint32_t c = reported = ports;   /* also resets the change tracking */
    ports_ins = ports_rem = 0;
    pthread_mutex_unlock(&in_lock);
    return c;
}
/* (type, pInsertions, pRemovals) -> TRUE when something changed since the last call. Only ports
 * the title was told about are reported removed, only ports still there inserted. */
static uint32_t get_device_changes(const uint32_t *a) {
    uint32_t ins = 0, rem = 0;
    if (a[0] == XAPI_GAMEPAD_TYPE) {
        pthread_mutex_lock(&in_lock);
        rem = ports_rem & reported;
        ins = ports_ins & ports;
        ports_ins = ports_rem = 0;
        reported = ports;
        pthread_mutex_unlock(&in_lock);
    }
    if (a[1]) *(uint32_t *)G(a[1]) = ins;
    if (a[2]) *(uint32_t *)G(a[2]) = rem;
    return (ins | rem) != 0;
}
/* (type, dwPort, dwSlot, pPollingParameters) -> handle, NULL when nothing is in that port */
static uint32_t input_open(const uint32_t *a) {
    if (a[0] != XAPI_GAMEPAD_TYPE || a[1] >= INPUT_PORTS) return 0;
    pthread_mutex_lock(&in_lock);
    uint32_t r = 0;
    if (ports >> a[1] & 1) {
        handles[a[1]] = (PadHandle){PAD_MAGIC, a[1]};
        r = GA(&handles[a[1]]);
    }
    pthread_mutex_unlock(&in_lock);
    return r;
}
static uint32_t input_close(const uint32_t *a) { (void)a; return 0; }
static uint32_t input_poll(const uint32_t *a) { return handle_get(a[0]) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED; }
static void put_pad(uint8_t *d, const XPad *p) {   /* packed XINPUT_GAMEPAD, 18 bytes */
    memcpy(d, &p->buttons, 2);
    memcpy(d + 2, p->analog, 8);
    memcpy(d + 10, &p->lx, 2);
    memcpy(d + 12, &p->ly, 2);
    memcpy(d + 14, &p->rx, 2);
    memcpy(d + 16, &p->ry, 2);
}
/* (h, pCapabilities): packed XINPUT_CAPABILITIES {SubType, Reserved(2), In.Gamepad(18), Out.Rumble(4)} */
static uint32_t input_caps(const uint32_t *a) {
    if (!handle_get(a[0])) return ERROR_DEVICE_NOT_CONNECTED;
    uint8_t *d = G(a[1]);
    if (!d) return ERROR_SUCCESS;
    memset(d, 0, 25);
    d[0] = XINPUT_DEVSUBTYPE_GC_GAMEPAD;
    memset(d + 3, 0xFF, 22);   /* every button, axis and motor is supported */
    return ERROR_SUCCESS;
}
/* (h, pState): XINPUT_STATE {dwPacketNumber, Gamepad} */
void (*input_read_hook)(void);
static uint32_t input_get_state(const uint32_t *a) {
    PadHandle *ph = handle_get(a[0]);
    if (!ph) return ERROR_DEVICE_NOT_CONNECTED;
    if (input_read_hook && ph->port == 0) input_read_hook();
    if (!a[1]) return ERROR_SUCCESS;
    XPad p;
    uint32_t pkt;
    input_get_port((int)ph->port, &p, &pkt);
    uint8_t *d = G(a[1]);
    memcpy(d, &pkt, 4);
    put_pad(d + 4, &p);
    return ERROR_SUCCESS;
}
void (*input_rumble_hook)(int port, uint16_t left, uint16_t right);
int input_rumble_enabled = 1;
/* (h, pFeedback): XINPUT_FEEDBACK = header {dwStatus, hEvent, Reserved[58]} then XINPUT_RUMBLE
 * {wLeftMotorSpeed, wRightMotorSpeed} at +66; the motors go to input_rumble_hook and the request
 * completes at once (header dwStatus). */
static uint32_t input_set_state(const uint32_t *a) {
    PadHandle *ph = handle_get(a[0]);
    if (!ph) return ERROR_DEVICE_NOT_CONNECTED;
    if (a[1]) {
        uint8_t *fb = G(a[1]);
        uint16_t m[2];
        memcpy(m, fb + 66, 4);
        if (input_rumble_hook && input_rumble_enabled) input_rumble_hook((int)ph->port, m[0], m[1]);
        *(uint32_t *)fb = ERROR_SUCCESS;
    }
    return ERROR_IO_PENDING;
}

typedef uint32_t (*InFn)(const uint32_t *a);
typedef struct {
    const char *name;
    uint32_t addr;
    int argbytes;
    InFn fn;
    uint8_t sig[HLE_SIG_LEN];
    uint32_t calls;
    int patched;
} InEntry;

static InEntry entries[] = {
    {"XInitDevices", 0x409863, 8, init_devices, {0x55, 0x8d, 0x6c, 0x24, 0x90}, 0, 0},
    {"XGetDevices", 0x40a6b1, 4, get_devices, {0x56, 0xff, 0x15, 0x30, 0xfc}, 0, 0},
    {"XGetDeviceChanges", 0x40a6d3, 12, get_device_changes, {0x55, 0x8b, 0xec, 0x56, 0x8b}, 0, 0},
    {"XGetDeviceEnumerationStatus", 0x40c125, 0, enum_status, {0x56, 0x33, 0xf6, 0xff, 0x15}, 0, 0},
    {"XInputOpen", 0x40a315, 16, input_open, {0x55, 0x8b, 0xec, 0x51, 0x8b}, 0, 0},
    {"XInputClose", 0x40a36b, 4, input_close, {0x8b, 0x4c, 0x24, 0x04, 0xe8}, 0, 0},
    {"XInputGetCapabilities", 0x40a377, 8, input_caps, {0x55, 0x8b, 0xec, 0x83, 0xec}, 0, 0},
    {"XInputGetState", 0x40a54f, 8, input_get_state, {0x53, 0x56, 0x33, 0xdb, 0xff}, 0, 0},
    {"XInputSetState", 0x40a5c2, 8, input_set_state, {0x8b, 0x4c, 0x24, 0x04, 0x8d}, 0, 0},
    {"XInputPoll", 0x40a5f5, 4, input_poll, {0x53, 0x56, 0x33, 0xf6, 0xff}, 0, 0},
};
#define NENTRIES ((int)(sizeof entries / sizeof *entries))
int hle_input_log_limit = 0;

int hle_input_entry_count(void) { return NENTRIES; }
uint32_t hle_input_entry_addr(int i) { return entries[i].addr; }
int hle_input_entry_argbytes(int i) { return entries[i].argbytes; }
const char *hle_input_entry_name(int i) { return entries[i].name; }
const uint8_t *hle_input_entry_sig(int i) { return entries[i].sig; }
int hle_input_patched(const char *name) {
    for (int i = 0; i < NENTRIES; i++) if (!strcmp(entries[i].name, name)) return entries[i].patched;
    return 0;
}

void hle_input_dispatch(InFrame *f) {
    InEntry *e = &entries[f->index];
    uint32_t r = e->fn(f->args);
    f->eax = r;
    uint32_t n = __atomic_add_fetch(&e->calls, 1, __ATOMIC_SEQ_CST);
    if ((int)n > hle_input_log_limit) return;
    char buf[96];
    int len = 0;
    for (int i = 0; i < e->argbytes / 4; i++) len += snprintf(buf + len, sizeof buf - len, "%s%x", i ? ", " : "", f->args[i]);
    buf[len] = 0;
    printf("[input] %s(%s) -> %x [from %08x]\n", e->name, buf, r, f->ret_addr);
}

/* Same frame as hle_entry_common (hle_thunk.S): the per-entry stub pushes the index and calls
 * this; pushal/popal hand eax back from the dispatcher. */
__asm__(".text\n.globl " PLAT_ASM_SYM(hle_input_entry_common) "\n" PLAT_ASM_SYM(hle_input_entry_common) ":\n"
        "pushal\ncld\nmovl %esp,%ebp\nandl $-16,%esp\nsubl $12,%esp\npushl %ebp\n"
        "call " PLAT_ASM_SYM(hle_input_dispatch) "\nmovl %ebp,%esp\npopal\nret\n");
extern void hle_input_entry_common(void);

int hle_input_install(HleLookup lookup) {
    uint8_t *stubs = plat_alloc_exec(NENTRIES * 16);
    if (!stubs) return 0;
    int n = 0;
    for (int i = 0; i < NENTRIES; i++) {
        InEntry *e = &entries[i];
        uint8_t *p = G(e->addr);
        char sym[96];
        snprintf(sym, sizeof sym, "XAPILIB__%s", e->name);
        uint32_t s = lookup ? lookup(sym) : 0;
        if (s && s != e->addr) { printf("[input] skip %s: symbol at %08x, table expects %08x\n", e->name, s, e->addr); continue; }
        if (memcmp(p, e->sig, HLE_SIG_LEN)) { printf("[input] skip %s at %08x: unexpected entry bytes\n", e->name, e->addr); continue; }
        /* Stub: push i; call hle_input_entry_common; add esp,4; ret argbytes */
        uint8_t *st = stubs + i * 16;
        st[0] = 0x68; memcpy(st + 1, &i, 4);
        int32_t rel = (int32_t)((uintptr_t)hle_input_entry_common - (uintptr_t)(st + 10));
        st[5] = 0xE8; memcpy(st + 6, &rel, 4);
        st[10] = 0x83; st[11] = 0xC4; st[12] = 0x04;
        st[13] = 0xC2; st[14] = e->argbytes & 0xFF; st[15] = e->argbytes >> 8;
        rel = (int32_t)((uintptr_t)st - (uintptr_t)(p + 5));
        p[0] = 0xE9; memcpy(p + 1, &rel, 4);
        e->patched = 1;
        n++;
    }
    printf("[input] %d of %d XAPI input entry points replaced (gamepads in ports 0-3; port 0 always connected)\n", n, NENTRIES);
    return n;
}

#ifdef HLE_INPUT_TEST
/* Game-independent test (build/hle_input_test, run by tests/test_input.py): maps the XPP range,
 * plants each entry's first bytes, installs the patches and calls them like the title does. */
__asm__(".text\n.globl tcall\ntcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\nmov %esp,%ebp\n"
        "mov 20(%ebp),%eax\nmov 24(%ebp),%ecx\nmov 28(%ebp),%esi\n"
        "1: test %ecx,%ecx\njz 2f\ndec %ecx\npushl (%esi,%ecx,4)\njmp 1b\n"
        "2: call *%eax\nmov %esp,%ecx\nsub %ebp,%ecx\nmov 32(%ebp),%edx\nmov %ecx,(%edx)\n"
        "mov %ebp,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define A(...) (const uint32_t[]){__VA_ARGS__}
#define P(x) ((uint32_t)(uintptr_t)(x))

static uint32_t call(const char *name, int nargs, const uint32_t *args) {
    int i = 0;
    while (i < NENTRIES && strcmp(entries[i].name, name)) i++;
    if (i == NENTRIES) { printf("FAIL no entry %s\n", name); failures++; return 0; }
    int32_t delta = 1;
    uint32_t r = tcall(entries[i].addr, nargs, args, &delta);
    CHECK(delta == 0 && nargs * 4 == entries[i].argbytes, "%s pops %d bytes (esp delta %d)", name, nargs * 4, delta);
    return r;
}
static uint32_t state(uint32_t h, uint8_t *st) { return call("XInputGetState", 2, A(h, P(st))); }
static int16_t s16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static struct { int port; uint16_t left, right; } rumble_seen = {-1, 0, 0};
static void test_rumble(int port, uint16_t left, uint16_t right) {
    rumble_seen.port = port;
    rumble_seen.left = left;
    rumble_seen.right = right;
}

int main(void) {
    const uint32_t lo = 0x409000, hi = 0x410000;   /* XPP section */
    uint8_t *m = plat_map_fixed(lo, hi - lo);
    if (!m) { fprintf(stderr, "cannot map %08x\n", lo); return 1; }
    memset(m, 0xCC, hi - lo);
    for (int i = 0; i < NENTRIES; i++) memcpy(G(entries[i].addr), entries[i].sig, HLE_SIG_LEN);
    uint8_t *bad = G(entries[NENTRIES - 1].addr);   /* XInputPoll: planted mismatch stays unpatched */
    bad[0] ^= 0xFF;
    int patched = hle_input_install(NULL);
    CHECK(patched == NENTRIES - 1 && !hle_input_patched("XInputPoll") && hle_input_patched("XInputGetState"),
          "install patched %d of %d entries (one planted mismatch)", patched, NENTRIES);
    CHECK(bad[0] == (0x53 ^ 0xFF), "mismatching entry left alone");

    /* A host controller in any port counts as connected (the auto icon style's text choice). */
    CHECK(!input_controller_connected(), "no host controller connected at start");
    XPad idle = {0};
    input_set_port_controller(2, &idle);
    CHECK(input_controller_connected(), "a controller in port 2 is connected");
    input_set_port_controller(2, NULL);
    CHECK(!input_controller_connected(), "after its removal none is");

    /* Device discovery as the title does it (input init at 0x6f9b0, per-frame 0x6fae0). */
    call("XInitDevices", 2, A(1, 0));
    CHECK(call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE)) == 1, "XGetDevices(gamepad) reports port 0");
    CHECK(call("XGetDevices", 1, A(0x4095b0)) == 0, "XGetDevices(other type) reports nothing");
    uint32_t ins = 9, rem = 9;
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 0 && ins == 0 && rem == 0,
          "no changes after XGetDevices (ins %x rem %x)", ins, rem);
    CHECK(call("XGetDeviceEnumerationStatus", 0, NULL) == 0, "enumeration idle");
    uint32_t poll[2] = {0x00080803, 0xffffffff};
    uint32_t h = call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 0, 0, P(poll)));
    CHECK(h != 0, "XInputOpen port 0 -> handle %x", h);
    CHECK(call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 1, 0, P(poll))) == 0, "XInputOpen port 1 -> NULL");
    uint8_t caps[32];
    memset(caps, 0xAA, sizeof caps);
    CHECK(call("XInputGetCapabilities", 2, A(h, P(caps))) == 0 && caps[0] == 1 && caps[3] == 0xFF && caps[24] == 0xFF && caps[25] == 0xAA,
          "capabilities: gamepad subtype, 25 bytes written");
    CHECK(call("XInputGetCapabilities", 2, A(0x1234, P(caps))) == ERROR_DEVICE_NOT_CONNECTED, "bad handle -> not connected");

    /* State and packet numbers. */
    uint8_t st[24];
    memset(st, 0xAA, sizeof st);
    CHECK(state(h, st) == 0 && st[22] == 0xAA, "XInputGetState writes 22 bytes");
    uint32_t p0 = u32(st);
    state(h, st);
    CHECK(u32(st) == p0, "packet number stable while nothing changes (%u)", p0);
    input_set_control(INPUT_SRC_KEYBOARD, IN_START, 1);
    state(h, st);
    CHECK(u32(st) == p0 + 1 && (st[4] & 0x10), "Start bit, packet %u -> %u", p0, u32(st));
    input_set_control(INPUT_SRC_KEYBOARD, IN_A, 1);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LS_UP, 1);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LT, 1);
    state(h, st);
    CHECK(st[6] == 0xFF && st[6 + 6] == 0xFF && s16(st + 16) == 32767 && s16(st + 14) == 0,
          "A analog, left trigger, left stick up");
    input_set_control(INPUT_SRC_KEYBOARD, IN_START, 0);
    input_set_control(INPUT_SRC_KEYBOARD, IN_A, 0);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LS_UP, 0);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LT, 0);
    state(h, st);
    uint8_t zero[18] = {0};
    CHECK(!memcmp(st + 4, zero, 18), "released keys clear the pad");
    input_set_mask(INPUT_SRC_KEYBOARD, 1u << IN_Y | 1u << IN_RS_LEFT);
    state(h, st);
    CHECK(st[9] == 0xFF && s16(st + 18) == -32767, "input_set_mask: Y analog and right stick left");
    input_set_mask(INPUT_SRC_KEYBOARD, 0);

    /* Controller merge: keys win for a held stick, buttons OR, analog max. */
    XPad c = {0};
    c.buttons = 0x20;
    c.analog[1] = 0x40;
    c.lx = -1000;
    c.ry = 1234;
    input_set_controller(&c);
    input_set_control(INPUT_SRC_KEYBOARD, IN_X, 1);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LS_RIGHT, 1);
    state(h, st);
    CHECK((st[4] & 0x20) && st[7] == 0x40 && st[8] == 0xFF && s16(st + 14) == 32767 && s16(st + 20) == 1234,
          "controller merged with keyboard");
    input_set_control(INPUT_SRC_KEYBOARD, IN_X, 0);
    input_set_control(INPUT_SRC_KEYBOARD, IN_LS_RIGHT, 0);
    input_set_controller(NULL);

    /* Script. */
    char err[128] = "";
    CHECK(input_script_parse("100:start:3,200:a+up", err, sizeof err) == 0, "script parses (%s)", err);
    CHECK(input_script_parse("100:jump", err, sizeof err) < 0, "unknown control rejected (%s)", err);
    CHECK(input_script_parse("x:start", err, sizeof err) < 0, "bad frame rejected (%s)", err);
    CHECK(input_script_parse("5:start:0", err, sizeof err) < 0, "zero frames rejected (%s)", err);
    /* Long gameplay scripts: 1000 steps (about 12 kB of text) parse and the last one is held. */
    {
        static char longspec[16384];
        int len = 0;
        for (int i = 0; i < 1000; i++) len += snprintf(longspec + len, sizeof longspec - len, "%s%d:x:1", i ? "," : "", 1000 + 2 * i);
        CHECK(input_script_parse(longspec, err, sizeof err) == 0, "1000-step script parses (%s)", err);
        input_frame(2998);
        state(h, st);
        CHECK(st[8] == 0xFF, "last of 1000 steps held at frame 2998");
        input_frame(2999);
    }
    input_script_parse("100:start:3,200:a+up", err, sizeof err);
    int seen[8], k = 0;
    for (uint32_t f = 99; f <= 104; f++) { input_frame(f); state(h, st); seen[k++] = st[4] & 0x10; }
    CHECK(!seen[0] && seen[1] && seen[2] && seen[3] && !seen[4] && !seen[5], "Start held for frames 100-102");
    input_frame(205);
    state(h, st);
    CHECK((st[4] & 0x01) && st[6] == 0xFF, "a+up held from frame 200 (default 6 frames)");
    input_frame(206);
    state(h, st);
    CHECK(!memcmp(st + 4, zero, 18), "script released after its frames");

    /* More ports: port 0 is always there, ports 1-3 follow the host controllers. */
    uint32_t fb0[20] = {997, 0};
    CHECK(input_connected_ports() == 1, "only port 0 connected at start (%x)", input_connected_ports());
    XPad c2 = {0};
    c2.buttons = 0x10;   /* Start */
    c2.lx = 2000;
    input_set_port_controller(1, &c2);
    ins = rem = 9;
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 1 && ins == 2 && rem == 0,
          "controller in port 1 reported as an insertion (ins %x rem %x)", ins, rem);
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 0 && ins == 0 && rem == 0,
          "insertion reported once (ins %x rem %x)", ins, rem);
    CHECK(call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE)) == 3, "XGetDevices reports ports 0 and 1");
    uint32_t h1 = call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 1, 0, P(poll)));
    CHECK(h1 != 0 && h1 != h, "XInputOpen port 1 -> handle %x", h1);
    CHECK(call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 2, 0, P(poll))) == 0, "XInputOpen port 2 (empty) -> NULL");
    CHECK(call("XInputGetCapabilities", 2, A(h1, P(caps))) == 0 && caps[0] == 1, "port 1 capabilities");
    input_set_control(INPUT_SRC_KEYBOARD, IN_A, 1);
    CHECK(state(h1, st) == 0 && (st[4] & 0x10) && st[6] == 0 && s16(st + 14) == 2000, "port 1 reads its own pad, not the keyboard");
    uint32_t q1 = u32(st);
    state(h, st);
    CHECK(!(st[4] & 0x10) && st[6] == 0xFF && s16(st + 14) == 0, "port 0 reads the keyboard, not the port 1 pad");
    input_set_control(INPUT_SRC_KEYBOARD, IN_A, 0);
    state(h1, st);
    CHECK(u32(st) == q1, "port 1 packet number unaffected by port 0 changes");
    c2.buttons = 0x20;
    input_set_port_controller(1, &c2);
    state(h1, st);
    CHECK(u32(st) == q1 + 1 && (st[4] & 0x20), "port 1 packet bumps on its own change");
    input_set_port_controller(3, &c2);
    call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem)));
    CHECK(ins == 8 && rem == 0, "port 3 insertion (ins %x rem %x)", ins, rem);
    input_set_port_controller(1, NULL);
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 1 && ins == 0 && rem == 2,
          "port 1 removal (ins %x rem %x)", ins, rem);
    CHECK(state(h1, st) == ERROR_DEVICE_NOT_CONNECTED, "removed port's handle -> not connected");
    CHECK(call("XInputSetState", 2, A(h1, P(fb0))) == ERROR_DEVICE_NOT_CONNECTED, "rumble on a removed port -> not connected");
    CHECK(call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 1, 0, P(poll))) == 0, "XInputOpen on the removed port -> NULL");
    input_set_port_controller(3, NULL);
    input_set_port_controller(3, &c2);
    call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem)));
    CHECK(ins == 8 && rem == 8, "unplug and replug between polls: removal and insertion (ins %x rem %x)", ins, rem);
    input_set_port_controller(2, &c2);
    CHECK(call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE)) == 0xD, "XGetDevices reports ports 0, 2, 3");
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 0 && !ins && !rem, "XGetDevices resets the changes");
    input_set_port_controller(0, &c2);
    input_set_port_controller(0, NULL);
    CHECK(call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem))) == 0 && (input_connected_ports() & 1),
          "port 0 stays connected when its controller goes away");
    input_set_port_controller(4, &c2);
    input_set_port_controller(-1, &c2);
    CHECK(input_connected_ports() == 0xD, "bad port numbers ignored");
    input_set_port_controller(2, NULL);
    input_set_port_controller(3, NULL);
    call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE));
    CHECK(input_connected_ports() == 1, "back to port 0 only");
    /* Port choice for a new host controller: its previous port when free, else the lowest free one. */
    CHECK(input_pick_port(0x0, -1) == 0 && input_pick_port(0x1, -1) == 1 && input_pick_port(0x3, 0) == 2 &&
          input_pick_port(0x1, 2) == 2 && input_pick_port(0x5, 2) == 1 && input_pick_port(0xF, -1) < 0,
          "input_pick_port: previous port when free, else lowest free, -1 when full");

    /* --pad-slot N=GUID[#K]: the player chosen in the launcher for a controller (made-up GUIDs). */
    static const char ga[] = "03000000aaaa00000100000000000000", gb[] = "03000000bbbb00000200000000000000";
    uint8_t a[16], b[16], a_crc[16];
    CHECK(input_pad_guid_parse(ga, a) == 0 && input_pad_guid_parse(gb, b) == 0 && a[0] == 3 && a[4] == 0xaa && a[8] == 1,
          "a GUID string parses to its 16 bytes");
    CHECK(input_pad_guid_parse("03000000aaaa", a_crc) < 0 && input_pad_guid_parse("03000000aaaa0000010000000000000g", a_crc) < 0,
          "a short or non-hex GUID is rejected");
    input_pad_guid_parse("0300605baaaa00000100000000000000", a_crc);   /* the same controller with a name CRC */
    CHECK(input_pad_slot_parse("1=03000000AAAA00000100000000000000", err, sizeof err) == 0, "1=GUID parses (%s)", err);
    CHECK(input_pad_slot_parse("5=03000000aaaa00000100000000000000", err, sizeof err) < 0, "player 5 rejected (%s)", err);
    CHECK(input_pad_slot_parse("0=03000000aaaa00000100000000000000", err, sizeof err) < 0, "player 0 rejected (%s)", err);
    CHECK(input_pad_slot_parse("2=nothex", err, sizeof err) < 0, "a bad GUID rejected (%s)", err);
    CHECK(input_pad_slot_parse("2=03000000aaaa00000100000000000000x", err, sizeof err) < 0, "trailing text rejected (%s)", err);
    CHECK(input_pad_slot_parse("2=03000000aaaa00000100000000000000#0", err, sizeof err) < 0, "#0 rejected (%s)", err);
    CHECK(input_pad_slot_parse("2", err, sizeof err) < 0, "a missing GUID rejected (%s)", err);
    input_pad_slots_clear();
    CHECK(input_pick_port_for(0x0, -1, a, 1) == 0 && input_pick_port_for(0x1, 2, a, 1) == 2 && !input_pad_reserved(a),
          "no slots: as input_pick_port");
    input_pad_slot_parse("2=03000000aaaa00000100000000000000", err, sizeof err);   /* controller A is player 2 */
    CHECK(input_pad_reserved(a) && input_pad_reserved(a_crc) && !input_pad_reserved(b),
          "the reservation matches its GUID, also with a name CRC on one side only");
    CHECK(input_pick_port_for(0x0, -1, a, 1) == 1, "controller A -> port 1 (player 2) with nothing in use");
    CHECK(input_pick_port_for(0x0, -1, b, 1) == 0, "another controller -> port 0");
    CHECK(input_pick_port_for(0x1, -1, b, 1) == 2, "another controller skips the reserved port 1");
    CHECK(input_pick_port_for(0x1, 1, b, 1) == 2, "and its previous port when that one is reserved");
    CHECK(input_pick_port_for(0x0, 3, b, 1) == 3, "a free previous port nobody chose is kept");
    CHECK(input_pick_port_for(0xD, -1, b, 1) == 1, "the reserved port is used when it is the only free one");
    CHECK(input_pick_port_for(0x2, -1, a, 2) == 0, "a second controller A while port 1 is taken: lowest free port");
    CHECK(input_pick_port_for(0xF, -1, a, 1) < 0, "all ports in use: -1");
    input_pad_slot_parse("1=03000000aaaa00000100000000000000#2", err, sizeof err);   /* the second A is player 1 */
    CHECK(input_pick_port_for(0x0, -1, a, 1) == 1 && input_pick_port_for(0x2, -1, a, 2) == 0,
          "two identical controllers: #1 -> player 2 (port 1), #2 -> player 1 (port 0)");
    CHECK(input_pick_port_for(0x1, -1, a, 2) == 1, "a returning controller A takes the free slot of its GUID (port 1)");
    input_pad_slot_parse("2=03000000bbbb00000200000000000000", err, sizeof err);   /* player 2 is now B */
    CHECK(input_pick_port_for(0x0, -1, b, 1) == 1 && input_pick_port_for(0x0, -1, a, 2) == 0 && input_pick_port_for(0x1, -1, a, 1) == 2,
          "a player given to another controller replaces the old reservation");
    input_pad_slots_clear();
    CHECK(!input_pad_reserved(a) && input_pick_port_for(0x0, -1, a, 2) == 0, "cleared");

    /* Script on more ports: pN/ controls, pN/plug and pN/unplug from a frame on. */
    CHECK(input_script_parse("10:p1/plug,12:p1/start:2,12:a,20:p1/unplug,30:p1/plug", err, sizeof err) == 0, "per-port script parses (%s)", err);
    CHECK(input_script_parse("10:p4/plug", err, sizeof err) < 0, "port 4 rejected (%s)", err);
    CHECK(input_script_parse("10:p0/plug", err, sizeof err) < 0, "plugging port 0 rejected (%s)", err);
    CHECK(input_script_parse("10:p1/jump", err, sizeof err) < 0, "unknown port control rejected (%s)", err);
    input_script_parse("10:p1/plug,12:p1/start:2,12:a,20:p1/unplug,30:p1/plug", err, sizeof err);
    call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE));
    input_frame(9);
    CHECK(input_connected_ports() == 1, "port 1 not plugged before frame 10");
    input_frame(10);
    call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem)));
    CHECK(input_connected_ports() == 3 && ins == 2 && rem == 0, "p1/plug at frame 10 inserts port 1 (ins %x rem %x)", ins, rem);
    h1 = call("XInputOpen", 4, A(XAPI_GAMEPAD_TYPE, 1, 0, P(poll)));
    input_frame(12);
    state(h1, st);
    CHECK(h1 && (st[4] & 0x10) && st[6] == 0, "p1/start held on port 1 only");
    state(h, st);
    CHECK(!(st[4] & 0x10) && st[6] == 0xFF, "unprefixed a held on port 0 only");
    input_frame(14);
    state(h1, st);
    CHECK(!(st[4] & 0x10), "p1/start released after its 2 frames");
    input_frame(20);
    call("XGetDeviceChanges", 3, A(XAPI_GAMEPAD_TYPE, P(&ins), P(&rem)));
    CHECK(input_connected_ports() == 1 && rem == 2 && state(h1, st) == ERROR_DEVICE_NOT_CONNECTED, "p1/unplug at frame 20 removes port 1");
    input_frame(30);
    CHECK(input_connected_ports() == 3, "p1/plug again at frame 30");
    input_script_parse("1:x", err, sizeof err);   /* no plug steps: port 1 leaves */
    input_frame(100);
    call("XGetDevices", 1, A(XAPI_GAMEPAD_TYPE));
    CHECK(input_connected_ports() == 1, "script without plugs leaves only port 0");

    /* Rumble and the remaining entries. */
    uint32_t fb[20] = {997, 0};
    uint16_t motors[2] = {0x1234, 0xabcd};
    memcpy((uint8_t *)fb + 66, motors, 4);
    input_rumble_hook = test_rumble;
    CHECK(call("XInputSetState", 2, A(h, P(fb))) == ERROR_IO_PENDING && fb[0] == 0, "XInputSetState completes at once");
    CHECK(rumble_seen.port == 0 && rumble_seen.left == 0x1234 && rumble_seen.right == 0xabcd,
          "XInputSetState passes the motor speeds to the rumble hook (port %d, %04x %04x)", rumble_seen.port, rumble_seen.left,
          rumble_seen.right);
    rumble_seen.port = -1;
    input_rumble_enabled = 0;
    CHECK(call("XInputSetState", 2, A(h, P(fb))) == ERROR_IO_PENDING && fb[0] == 0 && rumble_seen.port == -1,
          "--no-rumble: XInputSetState completes without reaching the rumble hook");
    input_rumble_enabled = 1;
    input_rumble_hook = NULL;
    call("XInputClose", 1, A(h));
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
