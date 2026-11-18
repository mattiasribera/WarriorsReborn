/* Platform-neutral host input layer between the window backends (video.c: x11, sdl; later a
 * native Windows backend) and the pad state in hle_input.c. Backends translate their events to
 * the INPUT_KEY_* namespace below and feed them in; this module owns the key bindings, the held
 * key state and the auto-repeat filtering, and publishes the resulting pad controls. */
#ifndef INPUT_MAP_H
#define INPUT_MAP_H
#include "hle_input.h"
#include <stdint.h>

/* Host keys: lowercase ASCII for printable keys, '\r' Enter, '\b' Backspace, '\t' Tab, 27
 * Escape, plus these. */
enum {
    INPUT_KEY_ESCAPE = 27,
    INPUT_KEY_UP = 0x100, INPUT_KEY_DOWN, INPUT_KEY_LEFT, INPUT_KEY_RIGHT,
    INPUT_KEY_LSHIFT, INPUT_KEY_RSHIFT, INPUT_KEY_LCTRL, INPUT_KEY_RCTRL, INPUT_KEY_LALT, INPUT_KEY_RALT,
    INPUT_KEY_F1 = 0x110,   /* F1..F12 consecutive; F1 (capture toggle) and Escape are reserved */
    INPUT_MOUSE_LEFT = 0x120, INPUT_MOUSE_MIDDLE, INPUT_MOUSE_RIGHT, INPUT_MOUSE_X1, INPUT_MOUSE_X2,
    INPUT_MOUSE_WHEEL,      /* never held: each notch taps its control (input_mouse_wheel) */
    INPUT_KEY_COUNT = 0x130
};

/* Key or mouse button change from a backend. Repeated presses (repeat != 0, e.g. SDL's key
 * repeat) are ignored; a press of a held key or a release of a free key changes nothing. */
void input_key_event(int key, int down, int repeat);
void input_release_all(void);                      /* focus lost: every key and button up */
int input_key_down(int key);                       /* 1 while that key is held */
int input_key_control(int key);                    /* bound IN_* control, INPUT_CTL_SPIN or -1 */
/* Virtual control beyond the pad ones: while a key bound to it is held the left stick spins
 * (input_set_spin). Config name spin_left_stick; default key G. */
#define INPUT_CTL_SPIN IN_COUNT
#define INPUT_SPIN_RATE_DEFAULT 2.0f
/* Virtual control mash_a: while a key bound to it is held, A is tapped at mash.rate taps per
 * second (input_set_mash), for the gym workouts that count fast presses of A. Default key H and
 * the mouse wheel (one tap of A per notch). */
#define INPUT_CTL_MASH (IN_COUNT + 1)
#define INPUT_MASH_RATE_DEFAULT 12.0f
/* Mouse wheel notches (either direction) from a backend: each one taps the control bound to
 * mouse.wheel once (mash_a: A), see input_tap. */
void input_mouse_wheel(int notches);
void input_map_defaults(void);                     /* PC layout (README "Input"), default mouse settings */
const char *input_key_name(int key);               /* config name ("w", "lshift", "mouse.left") or NULL */
int input_key_from_name(const char *name);         /* inverse, -1 when unknown */
/* The key a prompt shows for a pad control (button icons, kbm_glyphs.h): the first mouse button
 * bound to it (left, middle, right, x1, x2), else the bound key with the lowest code; -1 when
 * nothing is bound to it. */
int input_key_for_control(int control);

/* Key map config: lines `key.NAME = CONTROL`, `mouse.BUTTON = CONTROL` (CONTROL a pad control
 * name, spin_left_stick or `none`), `mouse.sensitivity = 1.0`, `mouse.invert_y = false`,
 * `mouse.deadzone`, `mouse.smoothing`, `spin.rate` (turns per second), the controller remap
 * (`pad.BUTTON = CONTROL`, `pad.swap_sticks`, below); `#` starts a comment,
 * names are case-insensitive. Each valid line changes the current map; each invalid line is
 * passed to report (1-based line number) and skipped. Returns the number of invalid lines. */
typedef void (*InputMapReport)(void *ctx, int line, const char *msg);
int input_map_parse(const char *text, InputMapReport report, void *ctx);
/* Reads and applies a config file, reporting invalid lines on stdout; -1 when it cannot be read. */
int input_map_load_file(const char *path);

/* Controller button remap (`pad.BUTTON = CONTROL` lines of the key map file, the launcher's
 * Controls page writes them): the physical controller input BUTTON (one of the 16 pad controls
 * IN_UP..IN_RT, named as in key lines: a, x, left_trigger, dpad_up, start, left_thumb...) does
 * CONTROL (one of those 16, or none) for every controller. `pad.swap_sticks = true` swaps the two
 * sticks. Without such lines (the default) every controller state passes unchanged. */
#define INPUT_PAD_INPUTS 16                        /* IN_UP..IN_RT */
#define INPUT_PAD_TRIGGER_THRESHOLD 30             /* a trigger bound to a digital button presses it past this */
extern int input_pad_swap_sticks;
int input_pad_control(int button);                 /* what that physical input does: a control or -1 */
int input_pad_remapped(void);                      /* 1 when any input does another control or the sticks swap */
/* The physical input a prompt for control shows: control itself when it still does it, else the
 * lowest input bound to it; -1 when none is. */
int input_pad_button_for_control(int control);
int input_pad_icons_moved(void);                   /* 1 when that differs from control for any control */
/* Physical controller state -> the state the title sees: each input's value goes to its control
 * (an analog button or trigger keeps its pressure; a digital input gives full pressure; a trigger
 * presses a digital button past INPUT_PAD_TRIGGER_THRESHOLD; several inputs on one control: the
 * strongest wins). out may be in. */
void input_pad_apply(const XPad *in, XPad *out);

/* Auto-repeat filter for backends whose server reports a held key as KeyRelease + KeyPress
 * pairs (X11 without detectable auto-repeat): the release is synthetic when the next queued
 * event is a press of the same key code with the same timestamp. next is NULL when nothing is
 * queued. */
typedef struct { int press; unsigned code; unsigned long time; } RawKeyEvent;
int input_is_repeat_release(const RawKeyEvent *release, const RawKeyEvent *next);

/* Mouse look: relative pointer motion drives the right stick. Each update turns the motion
 * since the previous one into a speed (counts per second; MOUSE_FULL_SPEED at sensitivity 1
 * is a full deflection), follows it with an exponential filter of time constant `smoothing`
 * seconds (so the stick eases back to centre when the mouse stops), clamps it to the stick
 * range and lifts any non-zero output past `deadzone` (a fraction of full deflection) so slow
 * motion is not lost in the title's stick dead zone. Mouse down is stick down unless invert_y. */
#define MOUSE_FULL_SPEED 1500.0
typedef struct { float sensitivity, deadzone, smoothing; int invert_y; } MouseCfg;
typedef struct { float x, y; } MouseLook;   /* filtered deflection, -1..1 */
extern MouseCfg input_mouse_cfg;
void mouse_look_update(MouseLook *m, const MouseCfg *c, double dx, double dy, double dt, int16_t *rx, int16_t *ry);

/* Backend feed (main thread): motion while the pointer is captured, then one input_mouse_frame
 * per event pump with a monotonic time in seconds; input_mouse_reset centres the stick and
 * releases the mouse buttons when the pointer is released. */
void input_mouse_motion(int dx, int dy);
void input_mouse_frame(double now_s);
void input_mouse_reset(void);
#endif
