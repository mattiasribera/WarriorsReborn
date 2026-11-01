/* HLE of the XAPI input boundary: the XInitDevices/XGetDevices/XInput* entry points the title
 * calls are replaced with host functions backed by up to four virtual standard gamepads
 * (ports 0-3), so the statically linked XAPI USB/OHCI code never runs. The pad state comes from
 * the host: keyboard (video backends), SDL game controllers (sdl backend) and a frame-based
 * script (--input-script). Host writers and guest readers share it under one lock. */
#ifndef HLE_INPUT_H
#define HLE_INPUT_H
#include "hle_audio.h"   /* HleLookup, HLE_SIG_LEN */
#include <stddef.h>
#include <stdint.h>

/* Pad controls. 0..7 are the XINPUT_GAMEPAD wButtons bits, 8..15 the bAnalogButtons indices
 * (+8), 16..23 full deflections of the two thumbsticks. */
enum {
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_START, IN_BACK, IN_LTHUMB, IN_RTHUMB,
    IN_A, IN_B, IN_X, IN_Y, IN_BLACK, IN_WHITE, IN_LT, IN_RT,
    IN_LS_UP, IN_LS_DOWN, IN_LS_LEFT, IN_LS_RIGHT, IN_RS_UP, IN_RS_DOWN, IN_RS_LEFT, IN_RS_RIGHT,
    IN_COUNT
};
enum { INPUT_SRC_KEYBOARD, INPUT_SRC_SCRIPT, INPUT_SRC_COUNT };

/* Host view of XINPUT_GAMEPAD (the guest copy is packed: 18 bytes). */
typedef struct {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t lx, ly, rx, ry;   /* +32767 = right / up */
} XPad;

/* Host keys and their bindings live in input_map.h; sources publish 1 << IN_* masks here. */
void input_set_control(int src, int control, int down);
void input_set_mask(int src, uint32_t mask);
void input_set_controller(const XPad *pad);           /* NULL: no host controller (port 0) */

/* Up to INPUT_PORTS controllers, as on the Xbox's four ports. Port 0 is always connected: it
 * carries the keyboard, the mouse, the spin, the script and the first host controller. Ports
 * 1-3 are connected while a host controller (or a scripted plug, --input-script pN/plug) is in
 * them; the title sees them come and go through XGetDevices/XGetDeviceChanges. */
#define INPUT_PORTS 4
void input_set_port_controller(int port, const XPad *pad);   /* NULL: that port's controller left */
void input_get_port(int port, XPad *out, uint32_t *packet);  /* merged state of one port */
uint32_t input_connected_ports(void);                        /* bit N: port N connected */
int input_controller_connected(void);                        /* 1 while a host controller is in any port */
/* Port for a newly attached host controller given the ports in use: `prev` (its previous port)
 * when it is free, else the lowest free port; -1 when all are in use. */
int input_pick_port(uint32_t used, int prev);
void input_set_mouse_stick(int16_t rx, int16_t ry);   /* mouse look; 0, 0 when idle */
/* Spin (keyboard): while on, the left stick turns in the order the title's car theft takes as
 * clockwise (up, left, down, right in XInput axes) at input_spin_rate turns per second from
 * the moment it was switched on, over the keys and the controller. The angle comes from input_clock at each read, so the rate does not
 * depend on how often the title polls; at 2 turns/s and 60 polls/s each quadrant is seen for
 * about 7 polls. */
void input_set_spin(int on);
extern float input_spin_rate;                         /* turns per second (default 2) */
extern double (*input_clock)(void);                   /* monotonic seconds; tests replace it */
void input_get(XPad *out, uint32_t *packet);          /* merged state; packet bumps on change */
void input_from_controls(uint32_t mask, XPad *out);   /* 1 << IN_* bits to a pad state */

/* --input-script FRAME:[pN/]CONTROLS[:FRAMES][,...]: holds CONTROLS ('+'-joined names such as
 * "start" or "a+up") from Swap number FRAME for FRAMES frames (default 6), on port N (p0-p3;
 * default p0, player 1). FRAME:pN/plug and FRAME:pN/unplug (N = 1-3) connect or disconnect a
 * scripted controller in port N from that frame on (the title sees the insertion/removal).
 * Returns 0, or -1 with a message in err. */
int input_script_parse(const char *spec, char *err, size_t errlen);
void input_frame(uint32_t frame);                     /* called by Swap with its frame number */
/* Called on the guest thread at each XInputGetState (NULL: none); --stats measures input latency. */
extern void (*input_read_hook)(void);
/* Called on the guest thread at each XInputSetState with the port's motor speeds (NULL: no output). */
extern void (*input_rumble_hook)(int port, uint16_t left, uint16_t right);
const char *input_control_name(int control);
/* Off (--no-rumble): XInputSetState requests complete without reaching input_rumble_hook. */
extern int input_rumble_enabled;

/* The device the player used last, for button icons that follow it (--button-icons auto):
 * keyboard/mouse (a key press or mouse motion, input_map.c) or a controller (a button, trigger or
 * stick past half way on any port). INPUT_DEVICE_NONE until either is used. */
enum { INPUT_DEVICE_NONE, INPUT_DEVICE_KBM, INPUT_DEVICE_PAD };
void input_note_device(int device);
int input_last_device(void);
int input_pad_active(const XPad *pad);   /* 1 when a pad state counts as controller use */

/* Patches the XAPI input entry points (same rules as hle_audio_install). */
int hle_input_install(HleLookup lookup);
int hle_input_patched(const char *name);              /* 1 when that entry was replaced */
extern int hle_input_log_limit;                       /* log lines per function (0 = silent) */

/* Table access for tests. */
int hle_input_entry_count(void);
uint32_t hle_input_entry_addr(int i);
int hle_input_entry_argbytes(int i);
const char *hle_input_entry_name(int i);
const uint8_t *hle_input_entry_sig(int i);   /* HLE_SIG_LEN bytes */

#define XAPI_GAMEPAD_TYPE 0x0040954cu   /* XAPILIB__g_DeviceType_Gamepad (XDEVICE_TYPE_GAMEPAD) */
#endif
